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


#include "mem/ruby/network/garnet/GarnetLink.hh"

#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/garnet/CreditLink.hh"
#include "mem/ruby/network/garnet/NetworkBridge.hh"
#include "mem/ruby/network/garnet/NetworkLink.hh"
#include "mem/ruby/network/garnet/Router.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

namespace {

template<class ChildParams, class LinkParams>
std::unique_ptr<ChildParams>
nativeLinkParams(const LinkParams &p, const std::string &child_name)
{
    fatal_if(!p.native_link_clock || !p.native_power_template ||
             p.native_vcs <= 0 || p.native_vnets <= 0,
             "Native link children require clock, power template and VC/vnet counts");
    auto child = std::make_unique<ChildParams>();
    child->name = p.name + "." + child_name;
    child->eventq_index = p.eventq_index;
    child->clk_domain = p.native_link_clock;
    child->native_power_state = true;
    child->power_state = p.native_power_template;
    child->link_id = p.link_id;
    child->link_latency = p.latency;
    child->serdes_latency = p.serdes_latency;
    child->is_off_chip = p.is_off_chip;
    child->vcs_per_vnet = p.native_vcs;
    child->virt_nets = p.native_vnets;
    child->supported_vnets = p.supported_vnets;
    child->width = p.width;
    child->onchip_component_latency = p.onchip_component_latency;
    child->offchip_component_latency = p.offchip_component_latency;
    child->serdes_component_latency = p.serdes_component_latency;
    return child;
}

} // anonymous namespace

GarnetIntLink::GarnetIntLink(const Params &p)
    : BasicIntLink(p)
{
    // Uni-directional

    if (p.native_children) {
        fatal_if(p.network_link || p.credit_link || p.src_net_bridge ||
                 p.src_cred_bridge || p.dst_net_bridge || p.dst_cred_bridge,
                 "Native internal link children require null Python child parameters");
        nativeNetworkParams = nativeLinkParams<NetworkLinkParams>(p, "network_link");
        nativeCreditParams = nativeLinkParams<CreditLinkParams>(p, "credit_link");
        nativeNetwork = std::make_unique<NetworkLink>(*nativeNetworkParams);
        nativeCredit = std::make_unique<CreditLink>(*nativeCreditParams);
        m_network_link = nativeNetwork.get();
        m_credit_link = nativeCredit.get();
        addStatGroup("network_link", m_network_link);
        addStatGroup("credit_link", m_credit_link);
    } else {
        fatal_if(!p.network_link || !p.credit_link,
                 "Internal link requires both network and credit children");
        m_network_link = p.network_link;
        m_credit_link = p.credit_link;
    }

    srcCdcEn = p.src_cdc;
    dstCdcEn = p.dst_cdc;

    srcSerdesEn = p.src_serdes;
    dstSerdesEn = p.dst_serdes;

    srcBridgeEn = false;
    dstBridgeEn = false;

    if (srcCdcEn || srcSerdesEn) {
        srcBridgeEn = true;
        if (!p.native_children) {
            srcNetBridge = p.src_net_bridge;
            srcCredBridge = p.src_cred_bridge;
        }
    }
    if (dstCdcEn || dstSerdesEn) {
        dstBridgeEn = true;
        if (!p.native_children) {
            dstNetBridge = p.dst_net_bridge;
            dstCredBridge = p.dst_cred_bridge;
        }
    }

    if (p.native_children) {
        auto makeBridge = [&](int index, const char *name, BasicRouter *node,
                              NetworkLink *link, enums::CDCType direction) {
            auto *router = dynamic_cast<Router *>(node);
            fatal_if(!router, "Native Garnet bridge requires a Garnet router");
            auto &params = nativeBridgeParams[index];
            params = nativeLinkParams<NetworkBridgeParams>(p, name);
            params->width = router->getBitWidth();
            params->link = link;
            params->vtype = direction;
            params->serdes_latency = p.native_bridge_serdes_latency;
            params->cdc_latency = p.native_bridge_cdc_latency;
            nativeBridges[index] = std::make_unique<NetworkBridge>(*params);
            addStatGroup(name, nativeBridges[index].get());
            return nativeBridges[index].get();
        };
        if (srcBridgeEn) {
            srcNetBridge = makeBridge(0, "src_net_bridge", p.src_node,
                                     m_network_link, enums::OBJECT_LINK);
            srcCredBridge = makeBridge(1, "src_cred_bridge", p.src_node,
                                      m_credit_link, enums::LINK_OBJECT);
        }
        if (dstBridgeEn) {
            dstNetBridge = makeBridge(2, "dst_net_bridge", p.dst_node,
                                     m_network_link, enums::LINK_OBJECT);
            dstCredBridge = makeBridge(3, "dst_cred_bridge", p.dst_node,
                                      m_credit_link, enums::OBJECT_LINK);
        }
    }
}

void
GarnetIntLink::init()
{
    if (srcBridgeEn) {
        assert(srcNetBridge && srcCredBridge);
        srcNetBridge->initBridge(srcCredBridge, srcCdcEn, srcSerdesEn);
        srcCredBridge->initBridge(srcNetBridge, srcCdcEn, srcSerdesEn);
    }

    if (dstBridgeEn) {
        assert(dstNetBridge && dstCredBridge);
        dstNetBridge->initBridge(dstCredBridge, dstCdcEn, dstSerdesEn);
        dstCredBridge->initBridge(dstNetBridge, dstCdcEn, dstSerdesEn);
    }
}

void
GarnetIntLink::print(std::ostream& out) const
{
    out << name();
}

GarnetExtLink::GarnetExtLink(const Params &p)
    : BasicExtLink(p)
{
    // Bi-directional

    if (p.native_children) {
        fatal_if(p.ext_cdc || p.int_cdc || p.ext_serdes || p.int_serdes ||
                 !p.network_links.empty() || !p.credit_links.empty(),
                 "Native external link children require unbridged empty child parameters");
        for (int direction = 0; direction < 2; ++direction) {
            const auto network_name = "network_links" + std::to_string(direction);
            const auto credit_name = "credit_links" + std::to_string(direction);
            nativeNetworkParams[direction] = nativeLinkParams<NetworkLinkParams>(p, network_name);
            nativeCreditParams[direction] = nativeLinkParams<CreditLinkParams>(p, credit_name);
            nativeNetworks[direction] = std::make_unique<NetworkLink>(*nativeNetworkParams[direction]);
            nativeCredits[direction] = std::make_unique<CreditLink>(*nativeCreditParams[direction]);
            m_network_links[direction] = nativeNetworks[direction].get();
            m_credit_links[direction] = nativeCredits[direction].get();
            addStatGroup(network_name.c_str(), m_network_links[direction]);
            addStatGroup(credit_name.c_str(), m_credit_links[direction]);
        }
    } else {
        fatal_if(p.network_links.size() != 2 || p.credit_links.size() != 2,
                 "External link requires two network and two credit children");
        for (int direction = 0; direction < 2; ++direction) {
            m_network_links[direction] = p.network_links[direction];
            m_credit_links[direction] = p.credit_links[direction];
        }
    }


    extCdcEn = p.ext_cdc;
    intCdcEn = p.int_cdc;

    extSerdesEn = p.ext_serdes;
    intSerdesEn = p.int_serdes;

    extBridgeEn = false;
    intBridgeEn = false;

    if (extCdcEn || extSerdesEn) {
        extBridgeEn = true;
        extNetBridge[0] = p.ext_net_bridge[0];
        extCredBridge[0] = p.ext_cred_bridge[0];
        extNetBridge[1] = p.ext_net_bridge[1];
        extCredBridge[1] = p.ext_cred_bridge[1];
    }

    if (intCdcEn || intSerdesEn) {
        intBridgeEn = true;
        intNetBridge[0] = p.int_net_bridge[0];
        intNetBridge[1] = p.int_net_bridge[1];
        intCredBridge[0] = p.int_cred_bridge[0];
        intCredBridge[1] = p.int_cred_bridge[1];
    }
}

void
GarnetExtLink::init()
{
    if (extBridgeEn) {
        assert(extNetBridge[0] && extCredBridge[0] &&
           extNetBridge[1] && extCredBridge[1]);
        extNetBridge[0]->initBridge(extCredBridge[0], extCdcEn, extSerdesEn);
        extCredBridge[0]->initBridge(extNetBridge[0], extCdcEn, extSerdesEn);
        extNetBridge[1]->initBridge(extCredBridge[1], extCdcEn, extSerdesEn);
        extCredBridge[1]->initBridge(extNetBridge[1], extCdcEn, extSerdesEn);
    }

    if (intBridgeEn) {
        assert(intNetBridge[0] && intCredBridge[0] &&
           intNetBridge[1] && intCredBridge[1]);
        intNetBridge[0]->initBridge(intCredBridge[0], intCdcEn, intSerdesEn);
        intCredBridge[0]->initBridge(intNetBridge[0], intCdcEn, intSerdesEn);
        intNetBridge[1]->initBridge(intCredBridge[1], intCdcEn, intSerdesEn);
        intCredBridge[1]->initBridge(intNetBridge[1], intCdcEn, intSerdesEn);
    }
}

void
GarnetExtLink::print(std::ostream& out) const
{
    out << name();
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
