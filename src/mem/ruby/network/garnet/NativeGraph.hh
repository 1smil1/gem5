#ifndef __MEM_RUBY_GARNET_NATIVE_GRAPH_HH__
#define __MEM_RUBY_GARNET_NATIVE_GRAPH_HH__

#include <memory>
#include <vector>

#include "params/GarnetNativeGraph.hh"
#include "sim/sim_object.hh"

namespace gem5
{
struct GarnetNetworkParams;
namespace ruby
{
namespace garnet
{
class GarnetNetwork;

// Own both the physical objects and the Params referenced by SimObject.
// No temporary Params may escape prepare(). Python clock owners stay alive.
class NativeGraph : public SimObject
{
  public:
    using Params = GarnetNativeGraphParams;
    explicit NativeGraph(const Params &p);
    ~NativeGraph() override;
    const GarnetNetworkParams &prepare(const GarnetNetworkParams &network);
    void bindGroups(GarnetNetwork *network);
    void initChildren();
    void initStateChildren();
    void probePointChildren();
    void probeListenerChildren();
    void startupChildren();
    SimObject *findChild(const std::string &name) const;

  private:
    const Params &configuration;
    std::unique_ptr<GarnetNetworkParams> networkParams;
    std::vector<std::unique_ptr<SimObjectParams>> childParams;
    std::vector<std::unique_ptr<SimObject>> children;
    std::vector<SimObject *> lifecycle;
};
} // namespace garnet
} // namespace ruby
} // namespace gem5
#endif
