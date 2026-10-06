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

/*
 * The topology here is configurable; it can be a hierachical (default
 * one) or a 2D torus or a 2D torus with half switches killed. I think
 * all input port has a one-input-one-output switch connected just to
 * control and bandwidth, since we don't control bandwidth on input
 * ports.  Basically, the class has a vector of nodes and edges. First
 * 2*m_nodes elements in the node vector are input and output
 * ports. Edges are represented in two vectors of src and dest
 * nodes. All edges have latency.
 */

#ifndef __MEM_RUBY_NETWORK_TOPOLOGY_HH__
#define __MEM_RUBY_NETWORK_TOPOLOGY_HH__

#include <cstddef>
#include <iostream>
#include <utility>
#include <vector>

#include "mem/ruby/common/TypeDefines.hh"
#include "mem/ruby/network/BasicLink.hh"
#include "mem/ruby/protocol/LinkDirection.hh"

namespace gem5
{

namespace ruby
{

class NetDest;
class Network;

// Dense 3D int matrix. Used by the legacy Floyd-Warshall implementation
// (extend_shortest_path). The active Dijkstra path in shortest_path() does
// not allocate this storage. Kept for API compatibility and to avoid
// silently changing route results.
typedef std::vector<std::vector<std::vector<int>>> Matrix;

// Sparse CSR-style weight matrix: per (vnet, src) list of (dst, weight).
// Only edges with finite positive weight are stored; absent entries
// default to INFINITE_LATENCY (defined in Topology.cc). Hot path
// (Dijkstra) walks adj[vnet][src] directly without a dense lookup.
// Equivalence with the prior dense Matrix: when there is a direct link,
// adj[vnet][src] contains (dst, weight); the absence of an entry means
// the same INFINITE_LATENCY as before. Dijkstra relax rules unchanged.
struct WeightMatrix
{
    int n = 0;       // num_switches
    int vnets = 0;   // num virtual networks
    // adj[vnet][src] = vector of (dst, weight) pairs.
    std::vector<std::vector<std::vector<std::pair<int, int>>>> adj;

    void init(int num_vnets, int num_switches);
    // Insert or overwrite the weight for (vnet, src, dst). No vnet_done
    // tracking here; the caller in createLinks already enforces the
    // uniqueness constraint for links between the same source-destination
    // pair.
    void set(int vnet, int src, int dst, int weight);
    // Linear scan over adj[vnet][src]; small (mesh-degree) in practice.
    int get(int vnet, int src, int dst) const;
};

// Per-source distance row, length num_switches. Workers move completed
// rows into DistMatrix; all source rows remain available for route marking.
struct DistVector
{
    int n = 0;
    std::vector<int> d;

    DistVector() = default;
    explicit DistVector(int n_, int fill) : n(n_), d(n_, fill) {}

    int &operator[](int i) { return d[i]; }
    int operator[](int i) const { return d[i]; }
};

// Per-(vnet, src) distance storage returned by shortest_path(). Total
// memory is O(vnets * num_switches^2) ints. Sparse weights do not make
// this all-pairs distance storage sparse.
struct DistMatrix
{
    int n = 0;
    int vnets = 0;
    std::vector<std::vector<DistVector>> rows;

    void init(int num_vnets, int num_switches);
    DistVector &operator()(int vnet, int src) { return rows[vnet][src]; }
    const DistVector &operator()(int vnet, int src) const
    { return rows[vnet][src]; }
};

struct LinkEntry
{
    BasicLink *link;
    PortDirection src_outport_dirn;
    PortDirection dst_inport_dirn;
};

typedef std::map<std::pair<SwitchID, SwitchID>,
             std::vector<LinkEntry>> LinkMap;

class Topology
{
  public:
    Topology(uint32_t num_nodes, uint32_t num_routers, uint32_t num_vnets,
             const std::vector<BasicExtLink *> &ext_links,
             const std::vector<BasicIntLink *> &int_links);

    uint32_t numSwitches() const { return m_number_of_switches; }
    void createLinks(Network *net);
    void print(std::ostream& out) const { out << "[Topology]"; }

  private:
    void addLink(SwitchID src, SwitchID dest, BasicLink* link,
                 PortDirection src_outport_dirn = "",
                 PortDirection dest_inport_dirn = "");
    void makeLink(Network *net, SwitchID src, SwitchID dest,
                  std::vector<NetDest>& routing_table_entry);

    // Helper functions based on chapter 29 of Cormen et al.
    // Deprecated: the active routing path is shortest_path() (Dijkstra).
    // Kept declared/defined to preserve API; not invoked in the hot path.
    void extend_shortest_path(Matrix &current_dist, Matrix &latencies,
                              Matrix &inter_switches);

    DistMatrix shortest_path(const WeightMatrix &weights);

    bool link_is_shortest_path_to_node(SwitchID src, SwitchID next,
            SwitchID final, const WeightMatrix &weights,
            const DistMatrix &dist, int vnet);

    NetDest shortest_path_to_node(SwitchID src, SwitchID next,
                                  const WeightMatrix &weights,
                                  const DistMatrix &dist, int vnet);

    const uint32_t m_nodes;
    const uint32_t m_number_of_switches;
    int m_vnets;

    std::vector<BasicExtLink*> m_ext_link_vector;
    std::vector<BasicIntLink*> m_int_link_vector;

    LinkMap m_link_map;
};

inline std::ostream&
operator<<(std::ostream& out, const Topology& obj)
{
    obj.print(out);
    out << std::flush;
    return out;
}

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_NETWORK_TOPOLOGY_HH__
