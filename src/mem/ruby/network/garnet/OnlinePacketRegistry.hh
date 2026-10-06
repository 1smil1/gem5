#ifndef __MEM_RUBY_NETWORK_GARNET_ONLINE_PACKET_REGISTRY_HH__
#define __MEM_RUBY_NETWORK_GARNET_ONLINE_PACKET_REGISTRY_HH__

#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/types.hh"
#include "sim/core.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

// Shared by the online injector and the destination NI in one gem5 process.
class OnlinePacketRegistry
{
  public:
    struct ArrivalInfo
    {
        uint64_t id;
        Tick tick;
    };
    struct PacketInfo
    {
        uint64_t id;
        uint64_t bytes;
        uint64_t slot;
    };

    static Addr
    registerPacket(uint64_t id, uint64_t bytes, unsigned destination,
                   unsigned destinations, unsigned blockBits,
                   uint64_t memoryBytes)
    {
        if (bytes == 0 || destinations == 0 || destination >= destinations ||
            blockBits >= 63 || packets().size() >= std::numeric_limits<int>::max()) {
            throw std::invalid_argument("invalid online packet id/size/destination");
        }
        if (!activeIds().insert(id).second) {
            throw std::invalid_argument("duplicate online packet id");
        }
        onlineActivated() = true;
        uint64_t slot;
        if (freeSlots().empty()) {
            slot = nextSlot()++;
        } else {
            slot = freeSlots().back();
            freeSlots().pop_back();
        }
        uint64_t stride = 1;
        while (stride < destinations) {
            stride <<= 1;
        }
        if (slot > ((std::numeric_limits<Addr>::max() >> blockBits) -
                    destination) / stride) {
            throw std::overflow_error("online packet address space exhausted");
        }
        Addr address = (slot * stride + destination) << blockBits;
        if (address >= memoryBytes) {
            throw std::overflow_error("online packet exceeds gem5 memory range");
        }
        if (!packets().emplace(address, PacketInfo{id, bytes, slot}).second) {
            throw std::logic_error("duplicate online packet address");
        }
        return address;
    }

    static bool
    sizeFor(Addr address, uint64_t &bytes)
    {
        auto it = packets().find(address);
        if (it == packets().end()) {
            if (onlineActivated()) {
                throw std::logic_error("online packet metadata missing at injection");
            }
            return false;
        }
        bytes = it->second.bytes;
        return true;
    }

    static void
    arrived(Addr address)
    {
        auto it = packets().find(address);
        if (it == packets().end()) {
            if (onlineActivated()) {
                throw std::logic_error("unknown or duplicate online arrival");
            }
            return; // Synthetic traffic is outside the online registry.
        }
        arrivalRecords().push_back(ArrivalInfo{it->second.id, curTick()});
        arrivals() << it->second.id << ' ' << curTick() << '\n';
        activeIds().erase(it->second.id);
        freeSlots().push_back(it->second.slot);
        packets().erase(it);
        exitSimLoop("online packet arrival");
    }

    static std::string
    drainArrivals()
    {
        std::string result = arrivals().str();
        arrivals().str("");
        arrivals().clear();
        return result;
    }

    static size_t inFlight() { return packets().size(); }

    static std::vector<ArrivalInfo>
    drainArrivalRecords()
    {
        std::vector<ArrivalInfo> result;
        result.swap(arrivalRecords());
        return result;
    }

  private:
    static std::map<Addr, PacketInfo>& packets()
    {
        static std::map<Addr, PacketInfo> value;
        return value;
    }
    static std::set<uint64_t>& activeIds()
    {
        static std::set<uint64_t> value;
        return value;
    }
    static bool& onlineActivated()
    {
        static bool value = false;
        return value;
    }
    static std::vector<uint64_t>& freeSlots()
    {
        static std::vector<uint64_t> value;
        return value;
    }
    static uint64_t& nextSlot()
    {
        static uint64_t value = 0;
        return value;
    }
    static std::ostringstream& arrivals()
    {
        static std::ostringstream value;
        return value;
    }
    static std::vector<ArrivalInfo>& arrivalRecords()
    {
        static std::vector<ArrivalInfo> value;
        return value;
    }
};

} // namespace gem5

#endif
