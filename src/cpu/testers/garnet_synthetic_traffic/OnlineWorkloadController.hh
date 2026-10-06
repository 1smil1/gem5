#ifndef __CPU_GARNET_SYNTHETIC_TRAFFIC_ONLINE_WORKLOAD_CONTROLLER_HH__
#define __CPU_GARNET_SYNTHETIC_TRAFFIC_ONLINE_WORKLOAD_CONTROLLER_HH__

#include <cstdint>
#include <string>
#include <vector>

namespace gem5
{

class GarnetSyntheticTraffic;

class OnlineWorkloadController
{
  public:
    struct PacketTemplate {
        uint64_t id = 0;
        int src = -1;
        int dst = -1;
        uint64_t bytes = 0;
    };

    OnlineWorkloadController(const std::vector<GarnetSyntheticTraffic *> &endpoints,
                             bool debug);

    std::string run(const std::string &manifest,
                    const std::string &position,
                    const std::string &catalog,
                    const std::string &nonuniform_grid,
                    const std::string &endpoint_mode,
                    int k_node,
                    int k_chip,
                    double frequency_ghz,
                    const std::string &result_path);

  private:
    struct Group {
        int id = -1;
        std::string path;
        int sharedPolicy = 0;
    };
    struct Phase {
        int id = -1;
        int input = -1;
        int layer = -1;
        uint64_t computeCycles = 0;
        std::vector<int> deps;
        std::vector<int> resources;
        int group = -1;
        int remainingDeps = 0;
        int status = 0; // waiting, computing, communicating, done
        uint64_t computeStart = 0;
        uint64_t computeEnd = 0;
        uint64_t commStart = 0;
        uint64_t commEnd = 0;
    };
    struct Run {
        int id = -1;
        int phase = -1;
        int group = -1;
        int input = -1;
        uint64_t injectTick = 0;
        uint64_t finishTick = 0;
        uint64_t packets = 0;
        uint64_t physicalPackets = 0;
    };
    struct CatalogLayer {
        int block = -1;
        std::vector<int> virtualIds;
        std::vector<int> canonical;
        std::vector<std::vector<int>> candidates;
    };

    const std::vector<GarnetSyntheticTraffic *> &endpoints;
    bool debug;
};

} // namespace gem5

#endif
