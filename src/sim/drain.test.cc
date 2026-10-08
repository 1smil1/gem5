#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "sim/drain.hh"

namespace gem5
{
namespace
{

class DrainProbe : public Drainable
{
  public:
    DrainProbe(int id, std::vector<int> &calls) : id(id), calls(calls) {}
    ~DrainProbe() override = default;

  protected:
    DrainState drain() override
    {
        calls.push_back(id);
        return DrainState::Drained;
    }
    void drainResume() override { calls.push_back(-id); }

  private:
    int id;
    std::vector<int> &calls;
};

TEST(DrainRegistration, RemovalReregistrationAndCallbackOrder)
{
    auto &manager = DrainManager::instance();
    std::vector<int> calls;
    DrainProbe first(1, calls);
    auto middle = std::make_unique<DrainProbe>(2, calls);
    DrainProbe last(3, calls);
    middle.reset();
    manager.unregisterDrainable(&first);
    manager.registerDrainable(&first);
    ASSERT_TRUE(manager.tryDrain());
    EXPECT_EQ(calls, (std::vector<int>{3, 1}));
    manager.resume();
    EXPECT_EQ(calls, (std::vector<int>{3, 1, -3, -1}));
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
TEST(DrainRegistrationDeathTest, RejectDuplicate)
{
    std::vector<int> calls;
    DrainProbe probe(1, calls);
    EXPECT_DEATH(DrainManager::instance().registerDrainable(&probe),
                 "!obj->_registered");
}
#endif

} // anonymous namespace
} // namespace gem5
