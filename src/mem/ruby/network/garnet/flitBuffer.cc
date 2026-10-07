/*
 * Copyright (c) 2020 Advanced Micro Devices, Inc.
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


#include "mem/ruby/network/garnet/flitBuffer.hh"

namespace gem5
{

namespace ruby
{

namespace garnet
{

flitBuffer::flitBuffer()
{
    max_size = INFINITE_;
}

flitBuffer::flitBuffer(int maximum_size)
{
    max_size = maximum_size;
}

bool
flitBuffer::isEmpty()
{
    return !m_buffer || m_buffer->empty();
}

bool
flitBuffer::isReady(Tick curTime)
{
    if (!isEmpty()) {
        flit *t_flit = peekTopFlit();
        if (t_flit->get_time() <= curTime)
            return true;
    }
    return false;
}

void
flitBuffer::print(std::ostream& out) const
{
    out << "[flitBuffer: " << getSize() << "] " << std::endl;
}

bool
flitBuffer::isFull()
{
    const auto size = m_buffer ? m_buffer->size() : 0;
    return size >= max_size;
}

void
flitBuffer::setMaxSize(int maximum)
{
    max_size = maximum;
}

bool
flitBuffer::functionalRead(Packet *pkt, WriteMask &mask)
{
    bool read = false;
    if (!m_buffer)
        return read;
    for (auto *flt : *m_buffer) {
        if (flt->functionalRead(pkt, mask)) {
            read = true;
        }
    }

    return read;
}

uint32_t
flitBuffer::functionalWrite(Packet *pkt)
{
    uint32_t num_functional_writes = 0;
    if (!m_buffer)
        return num_functional_writes;
    for (auto *flt : *m_buffer) {
        if (flt->functionalWrite(pkt)) {
            num_functional_writes++;
        }
    }

    return num_functional_writes;
}

} // namespace garnet
} // namespace ruby
} // namespace gem5
