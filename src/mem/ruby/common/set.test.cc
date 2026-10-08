#include <algorithm>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "mem/ruby/common/Set.hh"

using gem5::ruby::RubyDynamicBitset;
using gem5::ruby::Set;

namespace {
void check(const RubyDynamicBitset &bits, const std::vector<bool> &reference,
           std::size_t logical_size)
{
    EXPECT_EQ(bits.size(), logical_size);
    const auto count = std::count(reference.begin(), reference.end(), true);
    EXPECT_EQ(bits.count(), count);
    EXPECT_EQ(bits.none(), count == 0);
    std::vector<std::size_t> actual, expected;
    bits.forEachSetBit([&](std::size_t pos) { actual.push_back(pos); });
    for (std::size_t pos = 0; pos < logical_size; ++pos) {
        ASSERT_EQ(bits.test(pos), reference[pos]) << pos;
        if (reference[pos])
            expected.push_back(pos);
    }
    EXPECT_EQ(actual, expected);
}
} // namespace

TEST(RubySetTest, WrapperSizeAndCopySemantics)
{
    std::cout << "Ruby Set bytes=" << sizeof(Set) << '\n';
    Set source(54752);
    source.add(54751);
    Set copy = source;
    copy.remove(54751);
    EXPECT_EQ(copy.getSize(), 54752);
    EXPECT_TRUE(copy.isEmpty());
    EXPECT_TRUE(source.isElement(54751));
    copy = source;
    copy.add(0);
    EXPECT_EQ(copy.OR(source).count(), 2);
    EXPECT_EQ(copy.AND(source).count(), 1);
    EXPECT_FALSE(source.isElement(0));
    copy.removeSet(source);
    EXPECT_EQ(copy.smallestElement(), 0);
    copy.setSize(64);
    EXPECT_EQ(copy.getSize(), 64);
    EXPECT_TRUE(copy.isEmpty());
    copy.broadcast();
    EXPECT_TRUE(copy.isBroadcast());
    copy.clear();
    EXPECT_EQ(copy.getSize(), 64);
    EXPECT_TRUE(copy.isEmpty());
}

TEST(RubySetTest, SingletonAndDenseValueSemantics)
{
    std::minstd_rand random(20261008);
    for (std::size_t size : {0, 1, 63, 64, 65, 1296, 54752}) {
        RubyDynamicBitset bits(size);
        // Preserve the existing backing-word padding semantics of set()/~.
        std::vector<bool> reference((size + 63) / 64 * 64, false);
        for (int step = 0; step < 80; ++step) {
            const auto pos = size ? random() % size : 0;
            switch (step % 8) {
              case 0:
                if (size) { bits.set(pos); reference[pos] = true; }
                break;
              case 1:
                if (size) { bits.reset(pos); reference[pos] = false; }
                break;
              case 2: {
                auto copy = bits;
                auto expected = reference;
                if (size) {
                    copy[pos] = !expected[pos];
                    expected[pos] = !expected[pos];
                }
                check(copy, expected, size);
                break;
              }
              case 3:
              case 4: {
                RubyDynamicBitset other(size);
                std::vector<bool> expected(reference.size(), false);
                if (size) {
                    other.set(pos);
                    expected[pos] = true;
                    if (step % 3 == 0) {
                        other.set(size - 1);
                        expected[size - 1] = true;
                    }
                }
                bool overlap = false;
                for (std::size_t i = 0; i < reference.size(); ++i)
                    overlap |= reference[i] && expected[i];
                EXPECT_EQ(bits.intersects(other), overlap);
                EXPECT_EQ(other.intersects(bits), overlap);
                if (step % 8 == 3) {
                    bits |= other;
                    for (std::size_t i = 0; i < reference.size(); ++i)
                        reference[i] = reference[i] || expected[i];
                } else {
                    bits &= other;
                    for (std::size_t i = 0; i < reference.size(); ++i)
                        reference[i] = reference[i] && expected[i];
                }
                break;
              }
              case 5:
                bits = ~bits;
                for (std::size_t i = 0; i < reference.size(); ++i)
                    reference[i] = !reference[i];
                break;
              case 6:
                bits.set();
                std::fill(reference.begin(), reference.end(), true);
                break;
              case 7:
                bits.resize(size); // Existing resize clears all bits.
                std::fill(reference.begin(), reference.end(), false);
                break;
            }
            check(bits, reference, size);
            auto copy = bits;
            copy |= copy;
            EXPECT_TRUE(copy == bits);
            copy &= copy;
            EXPECT_TRUE(copy == bits);
        }
    }
}

TEST(RubySetTest, OffsetWordRanges)
{
    std::minstd_rand random(20261009);
    constexpr std::size_t size = 54752;
    RubyDynamicBitset bits(size);
    std::vector<bool> reference((size + 63) / 64 * 64, false);
    for (int step = 0; step < 100; ++step) {
        const auto pos = step % 2 ? random() % size : size - 1 - step;
        bits.set(pos);
        reference[pos] = true;
        check(bits, reference, size);
        auto copy = bits;
        copy.reset(pos);
        auto expected = reference;
        expected[pos] = false;
        check(copy, expected, size);
        RubyDynamicBitset other(size);
        other.set(pos);
        other.set((pos + 65) % size);
        auto intersection = bits & other;
        auto combined = bits | other;
        for (std::size_t i = 0; i < reference.size(); ++i) {
            const bool present = i == pos || i == (pos + 65) % size;
            expected[i] = reference[i] && present;
        }
        check(intersection, expected, size);
        expected = reference;
        expected[pos] = true;
        expected[(pos + 65) % size] = true;
        check(combined, expected, size);
    }
}
