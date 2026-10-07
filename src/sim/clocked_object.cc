/*
 * Copyright (c) 2015-2017, 2019-2020 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#include "sim/clocked_object.hh"

#include "base/logging.hh"
#include "sim/power/power_model.hh"

namespace gem5
{

ClockedObject::ClockedObject(const ClockedObjectParams &p) :
    SimObject(p), Clocked(*p.clk_domain), powerState(p.power_state)
{
    if (p.native_power_state) {
        fatal_if(!p.power_state || typeid(*p.power_state) != typeid(PowerState),
                 "Native power state requires a plain PowerState template");
        fatal_if(!p.power_state->params().leaders.empty(),
                 "Native power state does not support power-domain leaders");
        nativePowerStateParams =
            std::make_unique<PowerStateParams>(p.power_state->params());
        nativePowerStateParams->name = name() + ".power_state";
        nativePowerStateParams->eventq_index = p.eventq_index;
        nativePowerState = std::make_unique<PowerState>(*nativePowerStateParams);
        powerState = nativePowerState.get();
        // PowerState's lifecycle methods are inherited no-ops; its statistics
        // are initialized/reset through this same parent-child Group tree.
        addStatGroup("power_state", powerState);
    }
    // Register the power_model with the object
    // Slightly counter-intuitively, power models need to to register with the
    // clocked object and not the power stated object because the power model
    // needs information from the clock domain, which is an attribute of the
    // clocked object.
    for (auto & power_model: p.power_model)
        power_model->setClockedObject(this);
}

void
ClockedObject::serialize(CheckpointOut &cp) const
{
    powerState->serialize(cp);
}
void
ClockedObject::unserialize(CheckpointIn &cp)
{
    powerState->unserialize(cp);
}

} // namespace gem5
