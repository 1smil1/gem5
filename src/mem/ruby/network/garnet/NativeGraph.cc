#include "mem/ruby/network/garnet/NativeGraph.hh"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "base/cast.hh"
#include "base/logging.hh"
#include "mem/ruby/network/garnet/GarnetLink.hh"
#include "mem/ruby/network/garnet/GarnetNetwork.hh"
#include "mem/ruby/network/garnet/NetworkInterface.hh"
#include "mem/ruby/network/garnet/Router.hh"
#include "params/GarnetExtLink.hh"
#include "params/GarnetIntLink.hh"
#include "params/GarnetNetworkInterface.hh"
#include "params/GarnetRouter.hh"

namespace gem5
{
namespace ruby
{
namespace garnet
{
namespace
{
using Fields = std::vector<std::string_view>;

Fields split(std::string_view text, char delimiter)
{
    Fields fields;
    for (;;) {
        auto end = text.find(delimiter);
        fields.push_back(text.substr(0, end));
        if (end == std::string_view::npos)
            return fields;
        text.remove_prefix(end + 1);
    }
}

int64_t number(std::string_view value)
{
    int64_t result;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    fatal_if(parsed.ec != std::errc() || parsed.ptr != value.data() + value.size(),
             "Invalid native topology integer: %s", std::string(value));
    return result;
}

template<class T>
T integer(std::string_view value)
{
    const auto result = number(value);
    bool invalid;
    if constexpr (std::is_unsigned_v<T>) {
        invalid = result < 0 || static_cast<uint64_t>(result) > std::numeric_limits<T>::max();
    } else {
        invalid = result < std::numeric_limits<T>::min() || result > std::numeric_limits<T>::max();
    }
    fatal_if(invalid, "Native topology integer out of range: %s", std::string(value));
    return static_cast<T>(result);
}

bool flag(std::string_view value)
{
    const auto result = number(value);
    fatal_if(result != 0 && result != 1, "Native topology flag must be 0 or 1");
    return result != 0;
}

Cycles cycles(std::string_view value)
{
    return Cycles(integer<uint64_t>(value));
}

std::vector<int> vnets(std::string_view value)
{
    std::vector<int> result;
    if (!value.empty())
        for (auto field : split(value, ','))
            result.push_back(integer<int>(field));
    return result;
}

std::string childName(const std::string &parent, const char *field,
                      size_t index, size_t count)
{
    if (count == 1) return parent + "." + field;
    auto suffix = std::to_string(index);
    suffix.insert(0, std::to_string(count - 1).size() - suffix.size(), '0');
    return parent + "." + field + suffix;
}
} // anonymous namespace

NativeGraph::NativeGraph(const Params &p) : SimObject(p), configuration(p) {}
NativeGraph::~NativeGraph() = default;

const GarnetNetworkParams &
NativeGraph::prepare(const GarnetNetworkParams &source)
{
    fatal_if(networkParams, "Native topology prepared twice");
    fatal_if(!source.routers.empty() || !source.int_links.empty() ||
             !source.ext_links.empty() || !source.netifs.empty(),
             "Native graph requires empty Python topology vectors");
    fatal_if(configuration.router_anchors.empty() || !configuration.ni_anchor ||
             !configuration.power_template, "Native graph requires real anchors and power template");
    const auto lines = split(configuration.records, '\n');
    const auto header = split(lines.front(), '\t');
    fatal_if(header.size() != 5 || header[0] != "GARNET_GRAPH" || header[1] != "1",
             "Unsupported native topology schema");
    const auto routerCount = integer<uint32_t>(header[2]);
    const auto internalCount = integer<uint32_t>(header[3]);
    const auto externalCount = integer<uint32_t>(header[4]);
    fatal_if(!routerCount || !externalCount || externalCount != configuration.endpoints.size(),
             "Invalid native topology counts");
    networkParams = std::make_unique<GarnetNetworkParams>(source);
    auto &p = *networkParams;
    p.routers.reserve(routerCount);
    p.int_links.reserve(internalCount);
    p.ext_links.reserve(externalCount);
    p.netifs.reserve(externalCount);

    auto clock = [&](std::string_view index) {
        const auto i = integer<uint32_t>(index);
        fatal_if(i >= configuration.clocks.size() || !configuration.clocks[i],
                 "Invalid native topology clock index");
        return configuration.clocks[i];
    };
    auto router = [&](std::string_view index) {
        const auto i = integer<uint32_t>(index);
        fatal_if(i >= p.routers.size(), "Invalid native topology router index");
        return p.routers[i];
    };
    auto own = [&](auto parameters, auto object) {
        auto *ptr = object.get();
        childParams.push_back(std::move(parameters));
        children.push_back(std::move(object));
        return ptr;
    };
    const auto &routerPrototype = static_cast<const GarnetRouterParams &>(
        configuration.router_anchors.front()->SimObject::params());

    // Records are ordered by kind and by the original vector index. This
    // guarantees that all router pointers exist before any link is created.
    for (size_t line = 1; line < lines.size(); ++line) {
        const auto f = split(lines[line], '\t');
        fatal_if(f.empty(), "Empty native topology record");
        if (f[0] == "R") {
            fatal_if(f.size() != 11 || p.routers.size() >= routerCount ||
                     !p.int_links.empty() || !p.ext_links.empty(), "Invalid native router record order");
            const auto index = p.routers.size();
            fatal_if(integer<uint32_t>(f[1]) != index, "Native router IDs must match vector indices");
            const auto anchor = integer<int>(f[7]);
            if (anchor >= 0) {
                fatal_if(size_t(anchor) >= configuration.router_anchors.size(), "Invalid router anchor index");
                auto *node = configuration.router_anchors[anchor];
                fatal_if(node->name() != p.name + "." + std::string(f[10]),
                         "Native router anchor name mismatch");
                p.routers.push_back(node);
            } else {
                fatal_if(anchor != -1, "Invalid router anchor sentinel");
                auto params = std::make_unique<GarnetRouterParams>(routerPrototype);
                params->name = p.name + "." + std::string(f[10]);
                params->router_id = integer<int>(f[1]);
                params->latency = cycles(f[2]);
                params->width = integer<uint32_t>(f[3]);
                params->vcs_per_vnet = integer<uint32_t>(f[4]);
                params->virt_nets = integer<uint32_t>(f[5]);
                params->clk_domain = clock(f[6]);
                params->eventq_index = integer<uint32_t>(f[8]);
                params->role = std::string(f[9]);
                auto node = std::make_unique<Router>(*params);
                p.routers.push_back(own(std::move(params), std::move(node)));
            }
        } else if (f[0] == "I") {
            fatal_if(f.size() != 27 || p.routers.size() != routerCount ||
                     p.int_links.size() >= internalCount || !p.ext_links.empty(),
                     "Invalid native internal link record");
            const auto anchor = integer<int>(f[26]);
            if (anchor >= 0) {
                fatal_if(size_t(anchor) >= configuration.int_anchors.size(), "Invalid internal anchor index");
                auto *link = configuration.int_anchors[anchor];
                fatal_if(link->name() != childName(p.name, "int_links", p.int_links.size(), internalCount),
                         "Native internal link anchor name mismatch");
                p.int_links.push_back(link);
                continue;
            }
            fatal_if(anchor != -1, "Invalid internal anchor sentinel");
            auto params = std::make_unique<GarnetIntLinkParams>();
            params->name = childName(p.name, "int_links", p.int_links.size(), internalCount);
            params->link_id = integer<int>(f[1]);
            params->src_node = router(f[2]);
            params->dst_node = router(f[3]);
            params->latency = cycles(f[4]);
            params->serdes_latency = cycles(f[5]);
            params->is_off_chip = flag(f[6]);
            params->weight = integer<int>(f[7]);
            params->bandwidth_factor = integer<int>(f[8]);
            params->width = integer<uint32_t>(f[9]);
            params->src_cdc = flag(f[10]);
            params->dst_cdc = flag(f[11]);
            params->src_serdes = flag(f[12]);
            params->dst_serdes = flag(f[13]);
            params->onchip_component_latency = cycles(f[14]);
            params->offchip_component_latency = cycles(f[15]);
            params->serdes_component_latency = cycles(f[16]);
            params->native_bridge_serdes_latency = cycles(f[17]);
            params->native_bridge_cdc_latency = cycles(f[18]);
            params->native_link_clock = clock(f[19]);
            params->eventq_index = integer<uint32_t>(f[20]);
            params->supported_vnets = vnets(f[21]);
            params->src_outport = std::string(f[22]);
            params->dst_inport = std::string(f[23]);
            params->native_children = true;
            params->native_power_template = configuration.power_template;
            params->native_vcs = integer<int>(f[24]);
            params->native_vnets = integer<int>(f[25]);
            params->network_link = nullptr;
            params->credit_link = nullptr;
            params->src_net_bridge = params->dst_net_bridge = nullptr;
            params->src_cred_bridge = params->dst_cred_bridge = nullptr;
            auto node = std::make_unique<GarnetIntLink>(*params);
            p.int_links.push_back(own(std::move(params), std::move(node)));
        } else if (f[0] == "E") {
            fatal_if(f.size() != 23 || p.routers.size() != routerCount ||
                     p.int_links.size() != internalCount || p.ext_links.size() >= externalCount,
                     "Invalid native external link record");
            const auto index = p.ext_links.size();
            const auto endpoint = integer<uint32_t>(f[2]);
            fatal_if(endpoint != index || !configuration.endpoints[index], "Invalid native endpoint index");
            const auto anchor = integer<int>(f[20]);
            if (anchor >= 0) {
                fatal_if(size_t(anchor) >= configuration.ext_anchors.size(), "Invalid external anchor index");
                auto *link = configuration.ext_anchors[anchor];
                fatal_if(link->name() != childName(p.name, "ext_links", index, externalCount),
                         "Native external link anchor name mismatch");
                p.ext_links.push_back(link);
            } else {
                fatal_if(anchor != -1, "Invalid external anchor sentinel");
                auto params = std::make_unique<GarnetExtLinkParams>();
                params->name = childName(p.name, "ext_links", index, externalCount);
                params->link_id = integer<int>(f[1]);
                params->ext_node = configuration.endpoints[index];
                params->int_node = router(f[3]);
                params->latency = cycles(f[4]);
                params->serdes_latency = cycles(f[5]);
                params->is_off_chip = flag(f[6]);
                params->weight = integer<int>(f[7]);
                params->bandwidth_factor = integer<int>(f[8]);
                params->width = integer<uint32_t>(f[9]);
                params->ext_cdc = flag(f[10]);
                params->int_cdc = flag(f[11]);
                params->ext_serdes = flag(f[12]);
                params->int_serdes = flag(f[13]);
                params->native_link_clock = clock(f[14]);
                params->eventq_index = integer<uint32_t>(f[15]);
                params->supported_vnets = vnets(f[16]);
                params->onchip_component_latency = cycles(f[17]);
                params->offchip_component_latency = cycles(f[18]);
                params->serdes_component_latency = cycles(f[19]);
                params->native_children = true;
                params->native_power_template = configuration.power_template;
                params->native_vcs = integer<int>(f[21]);
                params->native_vnets = integer<int>(f[22]);
                auto node = std::make_unique<GarnetExtLink>(*params);
                p.ext_links.push_back(own(std::move(params), std::move(node)));
            }
        } else {
            fatal("Unknown native topology record kind: %s", std::string(f[0]));
        }
    }
    fatal_if(p.routers.size() != routerCount || p.int_links.size() != internalCount ||
             p.ext_links.size() != externalCount, "Native topology record count mismatch");
    const auto &niPrototype = static_cast<const GarnetNetworkInterfaceParams &>(
        configuration.ni_anchor->SimObject::params());
    for (size_t i = 0; i < externalCount; ++i) {
        if (i == 0) {
            fatal_if(configuration.ni_anchor->name() != childName(p.name, "netifs", 0, externalCount),
                     "Native network interface anchor name mismatch");
            p.netifs.push_back(configuration.ni_anchor);
        } else {
            auto params = std::make_unique<GarnetNetworkInterfaceParams>(niPrototype);
            params->name = childName(p.name, "netifs", i, externalCount);
            params->id = i;
            auto node = std::make_unique<NetworkInterface>(*params);
            p.netifs.push_back(own(std::move(params), std::move(node)));
        }
    }
    // Original vector object names are zero padded. Sorting their full paths
    // therefore retains descendants() field order and numeric vector order.
    std::unordered_map<std::string, SimObject *> unique;
    const auto prefix = p.name + ".";
    for (auto *node : configuration.lifecycle_anchors) {
        fatal_if(!node || node->name().compare(0, prefix.size(), prefix) != 0,
                 "Native lifecycle anchor outside network subtree");
        fatal_if(!unique.emplace(node->name(), node).second, "Duplicate native lifecycle anchor");
    }
    for (const auto &node : children)
        fatal_if(!unique.emplace(node->name(), node.get()).second, "Duplicate native physical object");
    for (const auto &entry : unique) lifecycle.push_back(entry.second);
    std::sort(lifecycle.begin(), lifecycle.end(), [](auto *a, auto *b) {
        return a->name() < b->name();
    });
    return p;
}

void NativeGraph::bindGroups(GarnetNetwork *network)
{
    const auto prefix = network->name() + ".";
    std::unordered_map<std::string, SimObject *> parents;
    std::unordered_set<SimObject *> owned;
    for (const auto &child : children) owned.insert(child.get());
    const auto &p = *networkParams;
    const auto bindRoots = [&](const auto &nodes, const char *field) {
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto *node = nodes[i];
            parents.emplace(node->name(), node);
            // Python already binds its anchors, using the same vector labels.
            if (owned.count(node)) {
                const auto label = std::string(field) + (nodes.size() == 1 ? "" : std::to_string(i));
                network->addStatGroup(label.c_str(), node);
            }
        }
    };
    bindRoots(p.ext_links, "ext_links");
    bindRoots(p.int_links, "int_links");
    bindRoots(p.netifs, "netifs");
    for (auto &node : children) {
        fatal_if(node->name().compare(0, prefix.size(), prefix) != 0, "Native topology name outside network");
        const auto end = node->name().rfind('.');
        const auto parent = node->name().substr(0, end);
        if (parent == network->name()) continue;
        const auto found = parents.find(parent);
        fatal_if(found == parents.end(), "Missing native router owner: %s", parent);
        const auto label = node->name().substr(end + 1);
        found->second->addStatGroup(label.c_str(), node.get());
    }
}
void NativeGraph::initChildren() { for (auto *node : lifecycle) node->init(); }
void NativeGraph::initStateChildren() { for (auto *node : lifecycle) node->initState(); }
void NativeGraph::probePointChildren() { for (auto *node : lifecycle) node->regProbePoints(); }
void NativeGraph::probeListenerChildren() { for (auto *node : lifecycle) node->regProbeListeners(); }
void NativeGraph::startupChildren() { for (auto *node : lifecycle) node->startup(); }
SimObject *NativeGraph::findChild(const std::string &name) const
{
    for (auto *node : lifecycle)
        if (node->name() == name) return node;
    fatal("Native topology child not found: %s", name);
}
} // namespace garnet
} // namespace ruby
} // namespace gem5
