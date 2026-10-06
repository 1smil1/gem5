/*
 * Copyright (c) 2020 Advanced Micro Devices, Inc.
 * Copyright (c) 1999-2008 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/ruby/network/Topology.hh"

#include <cassert>
#include <atomic>
#include <cerrno>
#include <climits>
#include <chrono>
#include <iostream>
#include <mutex>
#include <cstdlib>
#include <functional>
#include <future>
#include <queue>
#include <thread>

#include "base/trace.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/common/NetDest.hh"
#include "mem/ruby/network/BasicLink.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"

namespace gem5
{

namespace ruby
{

const int INFINITE_LATENCY = 10000; // Yes, this is a big hack

namespace {
void
topologyInitProgress(const std::string &stage)
{
    if (!std::getenv("OMELET_INIT_PROGRESS"))
        return;
    static const auto started = std::chrono::steady_clock::now();
    static std::mutex output_mutex;
    const std::lock_guard<std::mutex> guard(output_mutex);
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "OMELET_INIT topology " << stage
              << "; elapsed_s=" << elapsed << std::endl;
}

// Omelet route digest: FNV-1a over the canonical route-table inputs
// (weights, distances, per-link NetDest marks) so two topology
// implementations can be compared without changing any simulated behavior.
// Enabled only with OMELET_ROUTE_DIGEST in the environment.
struct RouteDigester
{
    uint64_t h = 1469598103934665603ULL;
    void mix(uint64_t v)
    {
        h ^= v;
        h *= 1099511628211ULL;
    }
    void mixInt(int x) { mix(static_cast<uint64_t>(static_cast<uint32_t>(x))); }
};

void
topologyRouteDigest(const char *label, uint64_t value)
{
    if (!std::getenv("OMELET_ROUTE_DIGEST"))
        return;
    std::cout << "OMELET_ROUTE_DIGEST " << label << "=0x" << std::hex << value
              << std::dec << std::endl;
}

} // anonymous namespace

// WeightMatrix / DistMatrix / DistVector helpers. Defined here so the
// Topology.cc body stays the single source of truth for the Dijkstra path.
void
WeightMatrix::init(int num_vnets, int num_switches)
{
    n = num_switches;
    vnets = num_vnets;
    adj.assign(num_vnets,
               std::vector<std::vector<std::pair<int, int>>>(num_switches));
}

void
WeightMatrix::set(int vnet, int src, int dst, int weight)
{
    assert(vnet >= 0 && vnet < vnets);
    assert(src >= 0 && src < n);
    assert(dst >= 0 && dst < n);
    auto &row = adj[vnet][src];
    for (auto &entry : row) {
        if (entry.first == dst) {
            entry.second = weight;
            return;
        }
    }
    row.emplace_back(dst, weight);
}

int
WeightMatrix::get(int vnet, int src, int dst) const
{
    // Caller contract: only invoked for pairs where the caller already
    // knows an edge exists (link_is_shortest_path_to_node), so a linear
    // scan and the INFINITE_LATENCY fallback are safe.
    assert(vnet >= 0 && vnet < vnets);
    assert(src >= 0 && src < n);
    assert(dst >= 0 && dst < n);
    const auto &row = adj[vnet][src];
    for (const auto &entry : row) {
        if (entry.first == dst) return entry.second;
    }
    return INFINITE_LATENCY;
}

void
DistMatrix::init(int num_vnets, int num_switches)
{
    n = num_switches;
    vnets = num_vnets;
    rows.assign(num_vnets,
                std::vector<DistVector>(num_switches,
                                        DistVector(num_switches,
                                                   INFINITE_LATENCY)));
}

// Note: In this file, we use the first 2*m_nodes SwitchIDs to
// represent the input and output endpoint links.  These really are
// not 'switches', as they will not have a Switch object allocated for
// them. The first m_nodes SwitchIDs are the links into the network,
// the second m_nodes set of SwitchIDs represent the the output queues
// of the network.

Topology::Topology(uint32_t num_nodes, uint32_t num_routers,
                   uint32_t num_vnets,
                   const std::vector<BasicExtLink *> &ext_links,
                   const std::vector<BasicIntLink *> &int_links)
    : m_nodes(MachineType_base_number(MachineType_NUM)),
      m_number_of_switches(num_routers), m_vnets(num_vnets),
      m_ext_link_vector(ext_links), m_int_link_vector(int_links)
{
    // Total nodes/controllers in network
    assert(m_nodes > 1);
    topologyInitProgress("constructor started; controllers=" +
        std::to_string(m_nodes) + " routers=" +
        std::to_string(m_number_of_switches));

    // analyze both the internal and external links, create data structures.
    // The python created external links are bi-directional,
    // and the python created internal links are uni-directional.
    // The networks and topology utilize uni-directional links.
    // Thus each external link is converted to two calls to addLink,
    // one for each direction.
    //
    // External Links
    for (std::vector<BasicExtLink*>::const_iterator i = ext_links.begin();
         i != ext_links.end(); ++i) {
        BasicExtLink *ext_link = (*i);
        const auto processed = i - ext_links.begin();
        if (processed % 1024 == 0)
            topologyInitProgress("external link map processed=" +
                std::to_string(processed) + "/" + std::to_string(ext_links.size()));
        AbstractController *abs_cntrl = ext_link->params().ext_node;
        BasicRouter *router = ext_link->params().int_node;

        int machine_base_idx = MachineType_base_number(abs_cntrl->getType());
        int ext_idx1 = machine_base_idx + abs_cntrl->getVersion();
        int ext_idx2 = ext_idx1 + m_nodes;
        int int_idx = router->params().router_id + 2*m_nodes;

        // create the internal uni-directional links in both directions
        // ext to int
        addLink(ext_idx1, int_idx, ext_link);
        // int to ext
        addLink(int_idx, ext_idx2, ext_link);
    }

    // Internal Links
    for (std::vector<BasicIntLink*>::const_iterator i = int_links.begin();
         i != int_links.end(); ++i) {
        BasicIntLink *int_link = (*i);
        const auto processed = i - int_links.begin();
        if (processed % 1024 == 0)
            topologyInitProgress("internal link map processed=" +
                std::to_string(processed) + "/" + std::to_string(int_links.size()));
        BasicRouter *router_src = int_link->params().src_node;
        BasicRouter *router_dst = int_link->params().dst_node;

        PortDirection src_outport = int_link->params().src_outport;
        PortDirection dst_inport = int_link->params().dst_inport;

        // Store the IntLink pointers for later
        m_int_link_vector.push_back(int_link);

        int src = router_src->params().router_id + 2*m_nodes;
        int dst = router_dst->params().router_id + 2*m_nodes;

        // create the internal uni-directional link from src to dst
        addLink(src, dst, int_link, src_outport, dst_inport);
    }
    topologyInitProgress("constructor completed; link groups=" +
                         std::to_string(m_link_map.size()));
}

void
Topology::createLinks(Network *net)
{
    topologyInitProgress("createLinks started; link groups=" +
                         std::to_string(m_link_map.size()));
    // Find maximum switchID
    SwitchID max_switch_id = 0;
    for (LinkMap::const_iterator i = m_link_map.begin();
         i != m_link_map.end(); ++i) {
        std::pair<SwitchID, SwitchID> src_dest = (*i).first;
        max_switch_id = std::max(max_switch_id, src_dest.first);
        max_switch_id = std::max(max_switch_id, src_dest.second);
    }

    // Initialize weight, latency, and inter switched vectors
    int num_switches = max_switch_id+1;
    // Sparse CSR: only edges with finite positive weight are stored.
    // Memory is O(num_links * vnets) rather than O(num_switches^2 * vnets).
    // For typical mesh degree ~6 and num_switches = 277182, this drops the
    // weights allocation from ~858 GiB/int-matrix to a few hundred MB.
    topologyInitProgress("allocating sparse weights matrix; switches=" +
        std::to_string(num_switches) + " vnets=" + std::to_string(m_vnets));
    WeightMatrix topology_weights;
    topology_weights.init(m_vnets, num_switches);
    topologyInitProgress("weights allocated (sparse)");

    // component_latencies / component_inter_switches of the pre-sparse code
    // were dense num_switches^2 x vnets matrices that only ever received
    // writes and were then discarded by shortest_path() ((void)-cast). No
    // reader exists anywhere in src/, so the active path allocates neither.
    // extend_shortest_path() keeps its Matrix& signature unchanged.

    topologyInitProgress("matrices allocated; initializing identity weights");
    // Set identity weights to zero
    for (int i = 0; i < num_switches; i++) {
        for (int v = 0; v < m_vnets; v++) {
            topology_weights.set(v, i, i, 0);
        }
    }

    // Fill in the topology weights and bandwidth multipliers
    const bool digest_routes = std::getenv("OMELET_ROUTE_DIGEST") != nullptr;
    RouteDigester weights_digest;
    for (auto link_group : m_link_map) {
        std::pair<int, int> src_dest = link_group.first;
        std::vector<bool> vnet_done(m_vnets, 0);
        int src = src_dest.first;
        int dst = src_dest.second;

        // Iterate over all links for this source and destination
        std::vector<LinkEntry> link_entries = link_group.second;
        for (int l = 0; l < link_entries.size(); l++) {
            BasicLink* link = link_entries[l].link;
            if (link->mVnets.size() == 0) {
                for (int v = 0; v < m_vnets; v++) {
                    // Two links connecting same src and destination
                    // cannot carry same vnets.
                    fatal_if(vnet_done[v], "Two links connecting same src"
                    " and destination cannot support same vnets");

                    topology_weights.set(v, src, dst, link->m_weight);
                    vnet_done[v] = true;
                }
            } else {
                for (int v = 0; v < link->mVnets.size(); v++) {
                    int vnet = link->mVnets[v];
                    fatal_if(vnet >= m_vnets, "Not enough virtual networks "
                             "(setting latency and weight for vnet %d)", vnet);
                    // Two links connecting same src and destination
                    // cannot carry same vnets.
                    fatal_if(vnet_done[vnet], "Two links connecting same src"
                    " and destination cannot support same vnets");

                    topology_weights.set(vnet, src, dst, link->m_weight);
                    vnet_done[vnet] = true;
                }
            }
        }
    }

    // Walk topology and hookup the links
    topologyInitProgress("weights populated; shortest paths started");
    if (digest_routes) {
        for (auto link_group : m_link_map) {
            const int src = link_group.first.first;
            const int dst = link_group.first.second;
            for (int v = 0; v < m_vnets; v++) {
                weights_digest.mixInt(src);
                weights_digest.mixInt(dst);
                weights_digest.mixInt(v);
                weights_digest.mixInt(topology_weights.get(v, src, dst));
            }
        }
        topologyRouteDigest("weights", weights_digest.h);
    }
    DistMatrix dist = shortest_path(topology_weights);
    RouteDigester dist_digest;
    if (digest_routes) {
        for (int v = 0; v < m_vnets; v++) {
            for (int src = 0; src < num_switches; src++) {
                const DistVector &row = dist(v, src);
                for (int dst = 0; dst < num_switches; dst++) {
                    dist_digest.mixInt(row[dst]);
                }
            }
        }
        topologyRouteDigest("dist", dist_digest.h);
    }
    RouteDigester marking_digest;

    // Iterate m_link_map directly instead of scanning all (i, j) pairs:
    // every real link has already been registered in m_link_map during
    // Topology::addLink, and the makeLink() payload only ever runs for
    // those pairs. std::map is sorted by (src, dst), so the visit order
    // is deterministic and matches the pre-change iteration order
    // (src ascending, then dst ascending within each src).
    int link_index = 0;
    const int total_links = static_cast<int>(m_link_map.size());
    for (auto link_group : m_link_map) {
        if (link_index % 1024 == 0)
            topologyInitProgress("installing link routes; link groups processed=" +
                std::to_string(link_index) + "/" + std::to_string(total_links));
        ++link_index;
        const int i = link_group.first.first;
        const int j = link_group.first.second;
        std::vector<NetDest> routingMap;
        routingMap.resize(m_vnets);

        // Only vnets that have a positive finite weight on this direct
        // link get a NetDest entry. vnet_done has already enforced the
        // one-vnet-per-link invariant in the weight-population pass, so
        // each vnet maps to at most one link entry here.
        bool realLink = false;

        for (int v = 0; v < m_vnets; v++) {
            int weight = topology_weights.get(v, i, j);
            if (weight > 0 && weight != INFINITE_LATENCY) {
                realLink = true;
                routingMap[v] =
                    shortest_path_to_node(i, j, topology_weights, dist, v);
                if (digest_routes) {
                    marking_digest.mixInt(i);
                    marking_digest.mixInt(j);
                    marking_digest.mixInt(v);
                    for (NodeID d : routingMap[v].getAllDest()) {
                        marking_digest.mixInt(static_cast<int>(d));
                    }
                    marking_digest.mixInt(0x40000000);
                }
            }
        }
        // Make one link for each set of vnets between
        // a given source and destination. We do not
        // want to create one link for each vnet.
        if (realLink) {
            makeLink(net, i, j, routingMap);
        }
    }
    topologyRouteDigest("marking", marking_digest.h);
    topologyInitProgress("createLinks completed");
}

void
Topology::addLink(SwitchID src, SwitchID dest, BasicLink* link,
                  PortDirection src_outport_dirn,
                  PortDirection dst_inport_dirn)
{
    assert(src <= m_number_of_switches+m_nodes+m_nodes);
    assert(dest <= m_number_of_switches+m_nodes+m_nodes);

    std::pair<int, int> src_dest_pair;
    src_dest_pair.first = src;
    src_dest_pair.second = dest;
    LinkEntry link_entry;

    link_entry.link = link;
    link_entry.src_outport_dirn = src_outport_dirn;
    link_entry.dst_inport_dirn  = dst_inport_dirn;

    auto lit = m_link_map.find(src_dest_pair);
    if (lit != m_link_map.end()) {
        // HeteroGarnet allows multiple links between
        // same source-destination pair supporting
        // different vnets. If there is a link already
        // between a given pair of source and destination
        // add this new link to it.
        lit->second.push_back(link_entry);
    } else {
        std::vector<LinkEntry> links;
        links.push_back(link_entry);
        m_link_map[src_dest_pair] = links;
    }
}

void
Topology::makeLink(Network *net, SwitchID src, SwitchID dest,
                   std::vector<NetDest>& routing_table_entry)
{
    // Make sure we're not trying to connect two end-point nodes
    // directly together
    assert(src >= 2 * m_nodes || dest >= 2 * m_nodes);

    std::pair<int, int> src_dest;
    LinkEntry link_entry;

    if (src < m_nodes) {
        src_dest.first = src;
        src_dest.second = dest;
        std::vector<LinkEntry> links = m_link_map[src_dest];
        for (int l = 0; l < links.size(); l++) {
            link_entry = links[l];
            std::vector<NetDest> linkRoute;
            linkRoute.resize(m_vnets);
            BasicLink *link = link_entry.link;
            if (link->mVnets.size() == 0) {
                net->makeExtInLink(src, dest - (2 * m_nodes), link,
                                routing_table_entry);
            } else {
                for (int v = 0; v< link->mVnets.size(); v++) {
                    int vnet = link->mVnets[v];
                    linkRoute[vnet] = routing_table_entry[vnet];
                }
                net->makeExtInLink(src, dest - (2 * m_nodes), link,
                                linkRoute);
            }
        }
    } else if (dest < 2*m_nodes) {
        assert(dest >= m_nodes);
        NodeID node = dest - m_nodes;
        src_dest.first = src;
        src_dest.second = dest;
        std::vector<LinkEntry> links = m_link_map[src_dest];
        for (int l = 0; l < links.size(); l++) {
            link_entry = links[l];
            std::vector<NetDest> linkRoute;
            linkRoute.resize(m_vnets);
            BasicLink *link = link_entry.link;
            if (link->mVnets.size() == 0) {
                net->makeExtOutLink(src - (2 * m_nodes), node, link,
                                 routing_table_entry);
            } else {
                for (int v = 0; v< link->mVnets.size(); v++) {
                    int vnet = link->mVnets[v];
                    linkRoute[vnet] = routing_table_entry[vnet];
                }
                net->makeExtOutLink(src - (2 * m_nodes), node, link,
                                linkRoute);
            }
        }
    } else {
        assert((src >= 2 * m_nodes) && (dest >= 2 * m_nodes));
        src_dest.first = src;
        src_dest.second = dest;
        std::vector<LinkEntry> links = m_link_map[src_dest];
        for (int l = 0; l < links.size(); l++) {
            link_entry = links[l];
            std::vector<NetDest> linkRoute;
            linkRoute.resize(m_vnets);
            BasicLink *link = link_entry.link;
            if (link->mVnets.size() == 0) {
                net->makeInternalLink(src - (2 * m_nodes),
                              dest - (2 * m_nodes), link, routing_table_entry,
                              link_entry.src_outport_dirn,
                              link_entry.dst_inport_dirn);
            } else {
                for (int v = 0; v< link->mVnets.size(); v++) {
                    int vnet = link->mVnets[v];
                    linkRoute[vnet] = routing_table_entry[vnet];
                }
                net->makeInternalLink(src - (2 * m_nodes),
                              dest - (2 * m_nodes), link, linkRoute,
                              link_entry.src_outport_dirn,
                              link_entry.dst_inport_dirn);
            }
        }
    }
}

// The following all-pairs shortest path algorithm is based on the
// discussion from Cormen et al., Chapter 26.1.
void
Topology::extend_shortest_path(Matrix &current_dist, Matrix &latencies,
    Matrix &inter_switches)
{
    int nodes = current_dist[0].size();

    // We find the shortest path for each vnet for a given pair of
    // source and destinations. This is done simply by traversing via
    // all other nodes and finding the minimum distance.
    for (int v = 0; v < m_vnets; v++) {
        // There is a different topology for each vnet. Here we try to
        // build a topology by finding the minimum number of intermediate
        // switches needed to reach the destination
        bool change = true;
        while (change) {
            change = false;
            for (int i = 0; i < nodes; i++) {
                for (int j = 0; j < nodes; j++) {
                    // We follow an iterative process to build the shortest
                    // path tree:
                    // 1. Start from the direct connection (if there is one,
                    // otherwise assume a hypothetical infinite weight link).
                    // 2. Then we iterate through all other nodes considering
                    // new potential intermediate switches. If we find any
                    // lesser weight combination, we set(update) that as the
                    // new weight between the source and destination.
                    // 3. Repeat for all pairs of nodes.
                    // 4. Go to step 1 if there was any new update done in
                    // Step 2.
                    int minimum = current_dist[v][i][j];
                    int previous_minimum = minimum;
                    int intermediate_switch = -1;
                    for (int k = 0; k < nodes; k++) {
                        minimum = std::min(minimum,
                            current_dist[v][i][k] + current_dist[v][k][j]);
                        if (previous_minimum != minimum) {
                            intermediate_switch = k;
                            inter_switches[i][j][v] =
                                inter_switches[i][k][v] +
                                inter_switches[k][j][v] + 1;
                        }
                        previous_minimum = minimum;
                    }
                    if (current_dist[v][i][j] != minimum) {
                        change = true;
                        current_dist[v][i][j] = minimum;
                        assert(intermediate_switch >= 0);
                        assert(intermediate_switch < latencies[i].size());
                        latencies[i][j][v] =
                            latencies[i][intermediate_switch][v] +
                            latencies[intermediate_switch][j][v];
                    }
                }
            }
        }
    }
}

DistMatrix
Topology::shortest_path(const WeightMatrix &weights)
{
    // The route table only consumes minimum link weights. Each source row is
    // independent, so sparse shortest-path searches can run concurrently.
    topologyInitProgress("shortest_path start; using sparse WeightMatrix");
    const int nodes = weights.n;
    const int vnets = weights.vnets;

    unsigned workers = std::thread::hardware_concurrency();
    if (workers == 0) {
        workers = 1;
    }
    if (const char *configured = std::getenv("OMELET_ROUTE_THREADS")) {
        char *end = nullptr;
        errno = 0;
        const unsigned long requested = std::strtoul(configured, &end, 10);
        fatal_if(errno != 0 || end == configured || *end != '\0' ||
                 requested == 0 || requested > UINT_MAX,
                 "OMELET_ROUTE_THREADS must be a positive integer");
        workers = requested;
    }
    const int jobs = vnets * nodes;
    workers = std::min(workers, static_cast<unsigned>(jobs));
    inform("Omelet routing table: %u host threads for %d source rows",
           workers, jobs);
    std::atomic<int> next_job{0};
    std::atomic<int> completed_jobs{0};
    const bool report_init_progress = std::getenv("OMELET_INIT_PROGRESS");

    // Per (vnet, src) DistVector allocated once and filled in by the worker
    // assigned to that source row. Retained memory is O(vnets * num_switches^2).
    topologyInitProgress("allocating per-source distance rows; total=" +
                         std::to_string(jobs));
    DistMatrix dist;
    dist.init(vnets, nodes);

    using Edge = std::pair<int, int>;
    const auto work = [&]() {
        for (int job = next_job.fetch_add(1); job < jobs;
             job = next_job.fetch_add(1)) {
            const int vnet = job / nodes;
            const int src = job % nodes;
            DistVector row(nodes, INFINITE_LATENCY);
            row[src] = 0;
            std::priority_queue<Edge, std::vector<Edge>, std::greater<Edge>> pending;
            pending.emplace(0, src);
            while (!pending.empty()) {
                const auto [distance, node] = pending.top();
                pending.pop();
                if (distance != row[node]) {
                    continue;
                }
                for (const auto &[dst, weight] : weights.adj[vnet][node]) {
                    const int candidate = distance + weight;
                    if (candidate < row[dst]) {
                        row[dst] = candidate;
                        pending.emplace(candidate, dst);
                    }
                }
            }
            dist(vnet, src) = std::move(row);
            if (report_init_progress) {
                const int done = completed_jobs.fetch_add(1) + 1;
                if (done % 1024 == 0 || done == jobs)
                    topologyInitProgress("shortest paths completed rows=" +
                        std::to_string(done) + "/" + std::to_string(jobs));
            }
        }
    };
    topologyInitProgress("parallel shortest paths started; rows=" +
                         std::to_string(jobs));
    std::vector<std::future<void>> tasks;
    tasks.reserve(workers > 0 ? workers - 1 : 0);
    for (unsigned worker = 1; worker < workers; ++worker) {
        tasks.emplace_back(std::async(std::launch::async, work));
    }
    work();
    for (auto &task : tasks) {
        task.get();
    }
    topologyInitProgress("shortest paths completed; source rows=" + std::to_string(jobs));
    return dist;
}

bool
Topology::link_is_shortest_path_to_node(SwitchID src, SwitchID next,
                                        SwitchID final,
                                        const WeightMatrix &weights,
                                        const DistMatrix &dist, int vnet)
{
    return weights.get(vnet, src, next) + dist(vnet, next)[final] ==
        dist(vnet, src)[final];
}

NetDest
Topology::shortest_path_to_node(SwitchID src, SwitchID next,
                                const WeightMatrix &weights,
                                const DistMatrix &dist, int vnet)
{
    NetDest result;
    int d = 0;
    int machines;
    int max_machines;

    machines = MachineType_NUM;
    max_machines = MachineType_base_number(MachineType_NUM);

    for (int m = 0; m < machines; m++) {
        for (NodeID i = 0; i < MachineType_base_count((MachineType)m); i++) {
            // we use "d+max_machines" below since the "destination"
            // switches for the machines are numbered
            // [MachineType_base_number(MachineType_NUM)...
            //  2*MachineType_base_number(MachineType_NUM)-1] for the
            // component network
            if (link_is_shortest_path_to_node(src, next, d + max_machines,
                    weights, dist, vnet)) {
                MachineID mach = {(MachineType)m, i};
                result.add(mach);
            }
            d++;
        }
    }

    DPRINTF(RubyNetwork, "Returning shortest path\n"
            "(src-(2*max_machines)): %d, (next-(2*max_machines)): %d, "
            "src: %d, next: %d, vnet:%d result: %s\n",
            (src-(2*max_machines)), (next-(2*max_machines)),
            src, next, vnet, result);

    return result;
}

} // namespace ruby
} // namespace gem5
