/*
 * Copyright (c) 2008 Princeton University
 * Copyright (c) 2016 Georgia Institute of Technology
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


#include "mem/ruby/network/garnet/RoutingUnit.hh"

#include <cstdlib>

#include "base/cast.hh"
#include "base/compiler.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/garnet/InputUnit.hh"
#include "mem/ruby/network/garnet/Router.hh"
#include "mem/ruby/slicc_interface/Message.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

RoutingUnit::RoutingUnit(Router *router)
{
    m_router = router;
    m_routing_table.clear();
    m_weight_table.clear();
}

void
RoutingUnit::addRoute(std::vector<NetDest>& routing_table_entry)
{
    if (routing_table_entry.size() > m_routing_table.size()) {
        m_routing_table.resize(routing_table_entry.size());
    }
    for (int v = 0; v < routing_table_entry.size(); v++) {
        m_routing_table[v].push_back(routing_table_entry[v]);
    }
}

void
RoutingUnit::addWeight(int link_weight)
{
    m_weight_table.push_back(link_weight);
}

bool
RoutingUnit::supportsVnet(int vnet, std::vector<int> sVnets)
{
    // If all vnets are supported, return true
    if (sVnets.size() == 0) {
        return true;
    }

    // Find the vnet in the vector, return true
    if (std::find(sVnets.begin(), sVnets.end(), vnet) != sVnets.end()) {
        return true;
    }

    // Not supported vnet
    return false;
}

/*
 * This is the default routing algorithm in garnet.
 * The routing table is populated during topology creation.
 * Routes can be biased via weight assignments in the topology file.
 * Correct weight assignments are critical to provide deadlock avoidance.
 */
int
RoutingUnit::lookupRoutingTable(int vnet, NetDest msg_destination,
                                PortDirection inport_dirn,
                                int src_router, int dest_router)
{
    // First find all possible output link candidates
    // For ordered vnet, just choose the first
    // (to make sure different packets don't choose different routes)
    // For unordered vnet, randomly choose any of the links
    // To have a strict ordering between links, they should be given
    // different weights in the topology file

    int output_link = -1;
    int min_weight = INFINITE_;
    std::vector<int> output_link_candidates;
    int num_candidates = 0;

    const bool hierarchical_escape =
        std::getenv("OMELET_DETERMINISTIC_ROUTES") != nullptr;
    auto startsWith = [](const PortDirection &direction,
                         const char *prefix) {
        return direction.rfind(prefix, 0) == 0;
    };
    bool has_direct_destination_up = false;
    if (m_router->role() == "interposer" &&
        src_router >= 0 && dest_router >= 0) {
        auto *network = m_router->get_net_ptr();
        for (int link = 0; link < m_routing_table[vnet].size(); ++link) {
            const auto &direction = m_outports_idx2dirn.at(link);
            if (!startsWith(direction, "Up") ||
                !msg_destination.intersectionIsNotEmpty(
                    m_routing_table[vnet][link]))
                continue;
            const char *suffix = direction.c_str() + 2;
            char *end = nullptr;
            long target_id = std::strtol(suffix, &end, 10);
            if (end != suffix && *end == '\0' && target_id >= 0 &&
                target_id < network->getNumRouters() &&
                network->getRouter(static_cast<int>(target_id))->role() ==
                    network->getRouter(dest_router)->role()) {
                has_direct_destination_up = true;
                break;
            }
        }
    }
    auto forbiddenHierarchicalUturn = [&](const PortDirection &outport_dirn) {
        if (!hierarchical_escape)
            return false;

        if (src_router < 0 || dest_router < 0)
            return false;

        auto *network = m_router->get_net_ptr();
        const auto &src_role = network->getRouter(src_router)->role();
        const auto &dst_role = network->getRouter(dest_router)->role();

        // A same-chiplet packet stays in its NoC.  A cross-chiplet packet
        // leaves the source NoC once and may not use a NoC as an interposer.
        if (startsWith(outport_dirn, "Down")) {
            // A 3D packet may enter an intermediate chiplet through NoL and
            // then descend into NoI.  Only a packet that already entered
            // this NoC from NoI must not return to NoI.
            if (m_router->role() != src_role && inport_dirn == "Down")
                return true;
        }

        // NoI is the only inter-chiplet fabric.  Once a packet is in NoI,
        // its next NoC hop must be in the destination chiplet.  This blocks
        // NoI -> source NoC -> ... -> the same NoI channel cycles while
        // preserving source-NoC -> NoI -> destination-NoC paths.  In 3D,
        // the destination may be above the NoI plane, so the route table may
        // intentionally enter a lower chiplet first and continue through NoL.
        if (m_router->role() == "interposer" &&
            startsWith(outport_dirn, "Up")) {
            if (!has_direct_destination_up)
                return false;
            const char *suffix = outport_dirn.c_str() + 2;
            char *end = nullptr;
            long target_id = std::strtol(suffix, &end, 10);
            if (end == suffix || *end != '\0' || target_id < 0 ||
                target_id >= network->getNumRouters() ||
                network->getRouter(static_cast<int>(target_id))->role() !=
                    dst_role)
                return true;
        }
        return false;
    };

    // Identify the minimum weight among the candidate output links
    for (int link = 0; link < m_routing_table[vnet].size(); link++) {
        if (msg_destination.intersectionIsNotEmpty(
            m_routing_table[vnet][link])) {
            auto direction = m_outports_idx2dirn.at(link);
            if (forbiddenHierarchicalUturn(direction))
                continue;

        if (m_weight_table[link] <= min_weight)
            min_weight = m_weight_table[link];
        }
    }

    // Collect all candidate output links with this minimum weight
    for (int link = 0; link < m_routing_table[vnet].size(); link++) {
        if (msg_destination.intersectionIsNotEmpty(
            m_routing_table[vnet][link])) {
            auto direction = m_outports_idx2dirn.at(link);
            if (forbiddenHierarchicalUturn(direction))
                continue;

            if (m_weight_table[link] == min_weight) {
                num_candidates++;
                output_link_candidates.push_back(link);
            }
        }
    }

    if (output_link_candidates.size() == 0) {
        if (std::getenv("OMELET_ROUTE_DEBUG") != nullptr) {
            auto *network = m_router->get_net_ptr();
            const auto &src_role = network->getRouter(src_router)->role();
            const auto &dst_role = network->getRouter(dest_router)->role();
            std::cerr << "OMELET_ROUTE_DEBUG no-route"
                      << " router=" << m_router->get_id()
                      << " role=" << m_router->role()
                      << " src=" << src_router
                      << " src_role=" << src_role
                      << " dst=" << dest_router
                      << " dst_role=" << dst_role
                      << " in=" << inport_dirn << std::endl;
            for (int link = 0; link < m_routing_table[vnet].size();
                 ++link) {
                if (msg_destination.intersectionIsNotEmpty(
                        m_routing_table[vnet][link])) {
                    std::cerr << "  candidate link=" << link
                              << " dir=" << m_outports_idx2dirn.at(link)
                              << " weight=" << m_weight_table[link]
                              << " filtered="
                              << forbiddenHierarchicalUturn(
                                     m_outports_idx2dirn.at(link))
                              << std::endl;
                }
            }
        }
        fatal("Fatal Error:: No Route exists from this Router.");
        exit(0);
    }

    // Online Omelet workloads use stable shortest-path tie-breaking. Keep
    // ordinary Garnet's randomized choice unless explicitly enabled.
    int candidate = 0;
    const bool deterministic_online =
        std::getenv("OMELET_DETERMINISTIC_ROUTES") != nullptr;
    if (!(m_router->get_net_ptr())->isVNetOrdered(vnet) &&
        !deterministic_online)
        candidate = rand() % num_candidates;

    output_link = output_link_candidates.at(candidate);
    return output_link;
}


void
RoutingUnit::addInDirection(PortDirection inport_dirn, int inport_idx)
{
    m_inports_dirn2idx[inport_dirn] = inport_idx;
    m_inports_idx2dirn[inport_idx]  = inport_dirn;
}

void
RoutingUnit::addOutDirection(PortDirection outport_dirn, int outport_idx)
{
    m_outports_dirn2idx[outport_dirn] = outport_idx;
    m_outports_idx2dirn[outport_idx]  = outport_dirn;
}

// outportCompute() is called by the InputUnit
// It calls the routing table by default.
// A template for adaptive topology-specific routing algorithm
// implementations using port directions rather than a static routing
// table is provided here.

int
RoutingUnit::outportCompute(RouteInfo route, int inport,
                            PortDirection inport_dirn)
{
    int outport = -1;

    if (route.dest_router == m_router->get_id()) {

        // Multiple NIs may be connected to this router,
        // all with output port direction = "Local"
        // Get exact outport id from table
        outport = lookupRoutingTable(route.vnet, route.net_dest,
                                     inport_dirn, route.src_router,
                                     route.dest_router);
        return outport;
    }

    // Routing Algorithm set in GarnetNetwork.py
    // Can be over-ridden from command line using --routing-algorithm = 1
    RoutingAlgorithm routing_algorithm =
        (RoutingAlgorithm) m_router->get_net_ptr()->getRoutingAlgorithm();

    switch (routing_algorithm) {
        case TABLE_:  outport =
            lookupRoutingTable(route.vnet, route.net_dest,
                               inport_dirn, route.src_router,
                               route.dest_router); break;
        case XY_:     outport =
            outportComputeXY(route, inport, inport_dirn); break;
        // any custom algorithm
        case CUSTOM_: outport =
            outportComputeCustom(route, inport, inport_dirn); break;
        default: outport =
            lookupRoutingTable(route.vnet, route.net_dest,
                               inport_dirn, route.src_router,
                               route.dest_router); break;
    }

    assert(outport != -1);
    return outport;
}

// XY routing implemented using port directions
// Only for reference purpose in a Mesh
// By default Garnet uses the routing table
int
RoutingUnit::outportComputeXY(RouteInfo route,
                              int inport,
                              PortDirection inport_dirn)
{
    PortDirection outport_dirn = "Unknown";

    [[maybe_unused]] int num_rows = m_router->get_net_ptr()->getNumRows();
    int num_cols = m_router->get_net_ptr()->getNumCols();
    assert(num_rows > 0 && num_cols > 0);

    int my_id = m_router->get_id();
    int my_x = my_id % num_cols;
    int my_y = my_id / num_cols;

    int dest_id = route.dest_router;
    int dest_x = dest_id % num_cols;
    int dest_y = dest_id / num_cols;

    int x_hops = abs(dest_x - my_x);
    int y_hops = abs(dest_y - my_y);

    bool x_dirn = (dest_x >= my_x);
    bool y_dirn = (dest_y >= my_y);

    // already checked that in outportCompute() function
    assert(!(x_hops == 0 && y_hops == 0));

    if (x_hops > 0) {
        if (x_dirn) {
            assert(inport_dirn == "Local" || inport_dirn == "West");
            outport_dirn = "East";
        } else {
            assert(inport_dirn == "Local" || inport_dirn == "East");
            outport_dirn = "West";
        }
    } else if (y_hops > 0) {
        if (y_dirn) {
            // "Local" or "South" or "West" or "East"
            assert(inport_dirn != "North");
            outport_dirn = "North";
        } else {
            // "Local" or "North" or "West" or "East"
            assert(inport_dirn != "South");
            outport_dirn = "South";
        }
    } else {
        // x_hops == 0 and y_hops == 0
        // this is not possible
        // already checked that in outportCompute() function
        panic("x_hops == y_hops == 0");
    }

    return m_outports_dirn2idx[outport_dirn];
}

// Template for implementing custom routing algorithm
// using port directions. (Example adaptive)
int
RoutingUnit::outportComputeCustom(RouteInfo route,
                                 int inport,
                                 PortDirection inport_dirn)
{
    panic("%s placeholder executed", __FUNCTION__);
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
