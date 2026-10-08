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
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <chrono>
#include <iostream>
#include <mutex>
#include <cstdlib>
#include <functional>
#include <deque>
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
    RouteDigester marking_digest;

    // Fill the same NetDest representation that Garnet keeps in each router.
    // This avoids scanning and converting a second route format at makeLink().
    struct RouteEdge { int src; int dst; int weight; int link; };

    const int total_links = static_cast<int>(m_link_map.size());
    uint64_t route_words = 0;
    for (int type = 0; type < MachineType_NUM; ++type)
        route_words += (MachineType_base_count((MachineType)type) + 63) / 64;
    const uint64_t route_bytes = static_cast<uint64_t>(total_links) *
        static_cast<uint64_t>(m_vnets) * route_words * sizeof(uint64_t);
    topologyInitProgress("unshared route-table backing upper-bound links=" +
        std::to_string(total_links) + " destinations=" +
        std::to_string(m_nodes) + " bytes=" + std::to_string(route_bytes) +
        " (~" + std::to_string(route_bytes / (1024.0 * 1024.0 * 1024.0)) +
        " GiB)");
    std::vector<std::pair<int, int>> link_keys;
    link_keys.reserve(total_links);
    for (const auto &entry : m_link_map)
        link_keys.push_back(entry.first);
    std::vector<std::vector<NetDest>> routing_maps;
    routing_maps.reserve(total_links);
    for (int i = 0; i < total_links; ++i) {
        routing_maps.emplace_back();
        routing_maps.back().reserve(m_vnets);
    }

    std::vector<std::vector<RouteEdge>> route_edges(m_vnets);
    std::vector<int> maximum_weight(m_vnets, 0);
    std::vector<std::vector<std::vector<std::pair<int, int>>>> reverse_adj(
        m_vnets, std::vector<std::vector<std::pair<int, int>>>(num_switches));
    for (int v = 0; v < m_vnets; ++v) {
        for (int i = 0; i < total_links; ++i) {
            const int src = link_keys[i].first;
            const int dst = link_keys[i].second;
            const int weight = topology_weights.get(v, src, dst);
            if (weight > 0 && weight != INFINITE_LATENCY) {
                maximum_weight[v] = std::max(maximum_weight[v], weight);
                // Both GarnetNetwork and SimpleNetwork ignore route masks in
                // makeExtInLink(): injection has no output-port choice here.
                // These virtual input vertices have no incoming edges, so
                // they cannot lie on a router-to-destination path either.
                // Create every real link; full digests retain the original
                // virtual-source distances and masks for historical checks.
                if (digest_routes || src >= m_nodes) {
                    route_edges[v].push_back({src, dst, weight, i});
                    reverse_adj[v][dst].push_back({src, weight});
                }
            }
        }
    }

    using Edge = std::pair<int, int>;
    // Keep the optional legacy distance digest without retaining its matrix.
    // This is a debug-only verification pass; normal runs do not repeat it.
    if (digest_routes) {
        RouteDigester dist_digest;
        for (int v = 0; v < m_vnets; ++v) {
            for (int src = 0; src < num_switches; ++src) {
                std::vector<int> distance(num_switches, INFINITE_LATENCY);
                std::priority_queue<Edge, std::vector<Edge>, std::greater<Edge>> pending;
                distance[src] = 0;
                pending.emplace(0, src);
                while (!pending.empty()) {
                    const auto [cost, node] = pending.top();
                    pending.pop();
                    if (cost != distance[node])
                        continue;
                    for (const auto &[dst, weight] : topology_weights.adj[v][node]) {
                        const int candidate = cost + weight;
                        if (candidate < distance[dst]) {
                            distance[dst] = candidate;
                            pending.emplace(candidate, dst);
                        }
                    }
                }
                for (int dst = 0; dst < num_switches; ++dst)
                    dist_digest.mixInt(distance[dst]);
            }
        }
        topologyRouteDigest("dist", dist_digest.h);
    }

    topologyInitProgress("streaming reverse shortest paths; links=" +
        std::to_string(total_links) + " destinations=" +
        std::to_string(m_nodes));
    const bool report_init_progress = std::getenv("OMELET_INIT_PROGRESS");
    for (int v = 0; v < m_vnets; ++v) {
        int identical_vnet = -1;
        for (int earlier = 0; earlier < v; ++earlier) {
            if (reverse_adj[v] == reverse_adj[earlier]) {
                identical_vnet = earlier;
                break;
            }
        }
        if (identical_vnet >= 0) {
            for (auto &routes : routing_maps)
                routes.push_back(routes[identical_vnet]);
            topologyInitProgress("reverse shortest paths vnet=" +
                std::to_string(v + 1) + "/" + std::to_string(m_vnets) +
                " identical graph; shared routes from vnet=" +
                std::to_string(identical_vnet + 1));
            continue;
        }
        for (auto &routes : routing_maps)
            routes.emplace_back();
        const bool bounded_weights = maximum_weight[v] <= 2;
        // Only terminal sinks may share destination planes. For two machine
        // types with identical single-parent attachments at every offset, all
        // distances and selected nonterminal edges are identical. Their final
        // output edges still carry distinct MachineIDs. Set copies use COW.
        bool terminal_sinks = true;
        for (int node = m_nodes; node < 2 * m_nodes; ++node) {
            for (const auto &[dst, weight] : topology_weights.adj[v][node]) {
                if (weight > 0 && weight != INFINITE_LATENCY)
                    terminal_sinks = false;
            }
        }
        topologyInitProgress("reverse shortest paths vnet=" +
            std::to_string(v + 1) + "/" + std::to_string(m_vnets) +
            " engine=" + (bounded_weights ? "bounded-weight FIFO" : "Dijkstra") +
            " maximum_weight=" + std::to_string(maximum_weight[v]));
        std::vector<int> distance(num_switches, INFINITE_LATENCY);
        std::array<std::deque<Edge>, 3> buckets;
        for (int d = 0; d < m_nodes; ++d) {
            // Decode once per destination, not once for every selected edge.
            int remaining = d;
            int type = 0;
            for (; type < MachineType_NUM; ++type) {
                const int count = MachineType_base_count((MachineType)type);
                if (remaining < count)
                    break;
                remaining -= count;
            }
            fatal_if(type == MachineType_NUM, "Invalid topology destination %d", d);
            const MachineID destination{(MachineType)type, static_cast<NodeID>(remaining)};
            if (remaining == 0 && terminal_sinks) {
                const auto machine = static_cast<MachineType>(type);
                const int count = MachineType_base_count(machine);
                int equivalent_type = -1;
                for (int earlier = 0; earlier < type; ++earlier) {
                    const auto source = static_cast<MachineType>(earlier);
                    if (MachineType_base_count(source) != count)
                        continue;
                    const int source_base = MachineType_base_number(source);
                    bool equivalent = true;
                    for (int offset = 0; offset < count; ++offset) {
                        const auto &parents = reverse_adj[v][m_nodes + d + offset];
                        if (parents.size() != 1 || parents !=
                            reverse_adj[v][m_nodes + source_base + offset]) {
                            equivalent = false;
                            break;
                        }
                    }
                    if (equivalent) {
                        equivalent_type = earlier;
                        break;
                    }
                }
                if (equivalent_type >= 0) {
                    for (const RouteEdge &edge : route_edges[v]) {
                        if (edge.dst >= m_nodes && edge.dst < 2 * m_nodes) {
                            if (edge.dst >= m_nodes + d &&
                                edge.dst < m_nodes + d + count)
                                routing_maps[edge.link][v].add(MachineID{
                                    machine, static_cast<NodeID>(edge.dst - m_nodes - d)});
                        } else {
                            routing_maps[edge.link][v].shareMachineDestinations(
                                machine, static_cast<MachineType>(equivalent_type));
                        }
                    }
                    topologyInitProgress("reverse shortest paths vnet=" +
                        std::to_string(v + 1) + "/" + std::to_string(m_vnets) +
                        " equivalent terminal type=" + std::to_string(type) +
                        " shared from type=" + std::to_string(equivalent_type) +
                        " destinations=" + std::to_string(d + count) + "/" +
                        std::to_string(m_nodes));
                    d += count - 1;
                    continue;
                }
            }
            const int target = m_nodes + d;
            // Omitted virtual sources stay at their initial infinite value.
            std::fill(distance.begin() + (digest_routes ? 0 : m_nodes),
                      distance.end(), INFINITE_LATENCY);
            distance[target] = 0;
            if (bounded_weights) {
                // Dial's queue for positive integer costs <= 2. Three cyclic
                // buckets pop the same minimum distances without a heap;
                // route marking and the weighted graph remain unchanged.
                buckets[0].emplace_back(0, target);
                size_t pending = 1;
                int current_cost = 0;
                while (pending) {
                    while (buckets[current_cost % 3].empty())
                        ++current_cost;
                    auto &bucket = buckets[current_cost % 3];
                    const auto [cost, node] = bucket.front();
                    bucket.pop_front();
                    --pending;
                    if (cost != distance[node])
                        continue;
                    for (const auto &[previous, weight] : reverse_adj[v][node]) {
                        const int candidate = cost + weight;
                        if (candidate < distance[previous]) {
                            distance[previous] = candidate;
                            buckets[candidate % 3].emplace_back(candidate, previous);
                            ++pending;
                        }
                    }
                }
            } else {
                std::priority_queue<Edge, std::vector<Edge>, std::greater<Edge>> pending;
                pending.emplace(0, target);
                while (!pending.empty()) {
                    const auto [cost, node] = pending.top();
                    pending.pop();
                    if (cost != distance[node])
                        continue;
                    for (const auto &[previous, weight] : reverse_adj[v][node]) {
                        const int candidate = cost + weight;
                        if (candidate < distance[previous]) {
                            distance[previous] = candidate;
                            pending.emplace(candidate, previous);
                        }
                    }
                }
            }
            for (const RouteEdge &edge : route_edges[v]) {
                if (edge.weight + distance[edge.dst] != distance[edge.src])
                    continue;
                routing_maps[edge.link][v].add(destination);
            }
            if (report_init_progress && (d % 1024 == 0 || d + 1 == m_nodes))
                topologyInitProgress("reverse shortest paths vnet=" +
                    std::to_string(v + 1) + "/" + std::to_string(m_vnets) +
                    " destinations=" + std::to_string(d + 1) + "/" +
                    std::to_string(m_nodes));
        }
    }

    int link_index = 0;
    for (const auto &link_group : m_link_map) {
        if (link_index % 1024 == 0)
            topologyInitProgress("installing link routes; link groups processed=" +
                std::to_string(link_index) + "/" + std::to_string(total_links));
        const int i = link_group.first.first;
        const int j = link_group.first.second;
        std::vector<NetDest> routingMap(m_vnets);
        bool realLink = false;
        for (int v = 0; v < m_vnets; ++v) {
            const int weight = topology_weights.get(v, i, j);
            if (weight > 0 && weight != INFINITE_LATENCY) {
                realLink = true;
                if (digest_routes) {
                    marking_digest.mixInt(i);
                    marking_digest.mixInt(j);
                    marking_digest.mixInt(v);
                    for (NodeID d : routing_maps[link_index][v].getAllDest())
                        marking_digest.mixInt(static_cast<int>(d));
                    marking_digest.mixInt(0x40000000);
                }
                routingMap[v] = routing_maps[link_index][v];
            }
        }
        if (realLink)
            makeLink(net, i, j, routingMap);
        std::vector<NetDest>().swap(routing_maps[link_index]);
        ++link_index;
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

} // namespace ruby
} // namespace gem5
