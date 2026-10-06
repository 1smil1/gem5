/*
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

// modified by Dan Gibson on 05/20/05 to accomidate FASTER
// >32 set lengths, using an array of ints w/ 32 bits/int
//
// Modified 2026-10-06: replaced compile-time `std::bitset<NUMBER_BITS_PER_SET>`
// with a runtime-sized `RubyDynamicBitset` (vector<uint64_t>). NetDest::resize
// can now pass MachineType_base_count values (e.g. 54752) without recompiling
// the binary. NUMBER_BITS_PER_SET is no longer referenced.

#ifndef __MEM_RUBY_COMMON_SET_HH__
#define __MEM_RUBY_COMMON_SET_HH__

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "base/logging.hh"
#include "mem/ruby/common/TypeDefines.hh"

namespace gem5
{

namespace ruby
{

// Minimal runtime-sized bitset used by Set. API mirrors the subset of
// std::bitset / boost::dynamic_bitset that Set needs; missing members fail at
// compile time on the caller, which is intentional so no behaviour drift
// creeps in via silent extension.
class RubyDynamicBitset
{
  public:
    RubyDynamicBitset() : m_size(0) {}

    explicit RubyDynamicBitset(std::size_t nbits) : m_size(nbits)
    {
        m_words.resize(wordsFor(nbits), 0);
    }

    std::size_t size() const { return m_size; }

    bool test(std::size_t pos) const
    {
        checkBounds(pos);
        return (m_words[wordIdx(pos)] >> bitIdx(pos)) & 1ULL;
    }

    // std::bitset-compatible proxy: `bits[i]` returns the bit at i.
    // Returns a proxy so write-syntax keeps working; reads return bool.
    class Proxy
    {
      public:
        Proxy(RubyDynamicBitset &bs, std::size_t pos)
            : m_bs(bs), m_pos(pos) {}
        Proxy &operator=(bool v)
        {
            m_bs.set(m_pos, v);
            return *this;
        }
        Proxy &operator=(const Proxy &other)
        {
            m_bs.set(m_pos, (bool)other);
            return *this;
        }
        operator bool() const { return m_bs.test(m_pos); }

      private:
        RubyDynamicBitset &m_bs;
        std::size_t m_pos;
    };
    Proxy operator[](std::size_t pos)
    {
        checkBounds(pos);
        return Proxy(*this, pos);
    }
    bool operator[](std::size_t pos) const
    {
        checkBounds(pos);
        return test(pos);
    }

    void set(std::size_t pos, bool value = true)
    {
        checkBounds(pos);
        std::uint64_t mask = 1ULL << bitIdx(pos);
        if (value)
            m_words[wordIdx(pos)] |= mask;
        else
            m_words[wordIdx(pos)] &= ~mask;
    }

    void set()
    {
        for (auto &w : m_words) w = ~0ULL;
    }

    void reset()
    {
        for (auto &w : m_words) w = 0ULL;
    }

    void reset(std::size_t pos)
    {
        checkBounds(pos);
        m_words[wordIdx(pos)] &= ~(1ULL << bitIdx(pos));
    }

    bool none() const
    {
        for (auto w : m_words) if (w) return false;
        return true;
    }

    std::size_t count() const
    {
        std::size_t total = 0;
        for (auto w : m_words) total += popcount(w);
        return total;
    }

    bool operator==(const RubyDynamicBitset &other) const
    {
        return m_size == other.m_size && m_words == other.m_words;
    }
    bool operator!=(const RubyDynamicBitset &other) const
    {
        return !(*this == other);
    }

    RubyDynamicBitset operator|(const RubyDynamicBitset &other) const
    {
        assert(m_size == other.m_size);
        RubyDynamicBitset r(m_size);
        for (std::size_t i = 0; i < m_words.size(); ++i)
            r.m_words[i] = m_words[i] | other.m_words[i];
        return r;
    }

    RubyDynamicBitset operator&(const RubyDynamicBitset &other) const
    {
        assert(m_size == other.m_size);
        RubyDynamicBitset r(m_size);
        for (std::size_t i = 0; i < m_words.size(); ++i)
            r.m_words[i] = m_words[i] & other.m_words[i];
        return r;
    }

    RubyDynamicBitset operator~() const
    {
        RubyDynamicBitset r(m_size);
        for (std::size_t i = 0; i < m_words.size(); ++i)
            r.m_words[i] = ~m_words[i];
        return r;
    }

    RubyDynamicBitset &operator|=(const RubyDynamicBitset &other)
    {
        assert(m_size == other.m_size);
        for (std::size_t i = 0; i < m_words.size(); ++i)
            m_words[i] |= other.m_words[i];
        return *this;
    }

    RubyDynamicBitset &operator&=(const RubyDynamicBitset &other)
    {
        assert(m_size == other.m_size);
        for (std::size_t i = 0; i < m_words.size(); ++i)
            m_words[i] &= other.m_words[i];
        return *this;
    }

    void resize(std::size_t nbits)
    {
        m_size = nbits;
        m_words.assign(wordsFor(nbits), 0);
    }

    friend std::ostream &operator<<(std::ostream &out,
                                    const RubyDynamicBitset &bs)
    {
        out << '[';
        for (std::size_t i = 0; i < bs.m_size; ++i) {
            if (i) out << ' ';
            out << (bs.test(i) ? '1' : '0');
        }
        out << ']';
        return out;
    }

  private:
    static constexpr std::size_t kBitsPerWord = 64;

    static std::size_t wordsFor(std::size_t nbits)
    {
        return (nbits + kBitsPerWord - 1) / kBitsPerWord;
    }

    static std::size_t wordIdx(std::size_t pos) { return pos / kBitsPerWord; }
    static std::size_t bitIdx(std::size_t pos) { return pos % kBitsPerWord; }

    void checkBounds(std::size_t pos) const
    {
        assert(pos < m_size);
    }

    static std::size_t popcount(std::uint64_t w)
    {
        // No <bit> header dependency; use compiler builtin when available.
#if defined(__GNUC__) || defined(__clang__)
        return static_cast<std::size_t>(__builtin_popcountll(w));
#else
        std::size_t c = 0;
        while (w) { c += w & 1ULL; w >>= 1; }
        return c;
#endif
    }

    std::size_t m_size;
    std::vector<std::uint64_t> m_words;
};

class Set
{
  private:
    // Number of bits in use in this set.
    int m_nSize;
    RubyDynamicBitset bits;

  public:
    Set() : m_nSize(0) {}

    Set(int size) : m_nSize(size), bits(static_cast<std::size_t>(size)) {}

    Set(const Set& obj) : m_nSize(obj.m_nSize), bits(obj.bits) {}
    ~Set() {}

    Set& operator=(const Set& obj)
    {
        m_nSize = obj.m_nSize;
        bits = obj.bits;
        return *this;
    }

    void
    add(NodeID index)
    {
        bits.set(index);
    }

    /*
     * This function should set all the bits in the current set that are
     * already set in the parameter set
     */
    void
    addSet(const Set& obj)
    {
        assert(m_nSize == obj.m_nSize);
        bits |= obj.bits;
    }

    /*
     * This function clears bits that are =1 in the parameter set
     */
    void
    remove(NodeID index)
    {
        bits.reset(index);
    }

    /*
     * This function clears bits that are =1 in the parameter set
     */
    void
    removeSet(const Set& obj)
    {
        assert(m_nSize == obj.m_nSize);
        bits &= (~obj.bits);
    }

    void clear() { bits.reset(); }

    /*
     * this function sets all bits in the set
     */
    void broadcast()
    {
        bits.resize(static_cast<std::size_t>(m_nSize));
        bits.set();
    }

    /*
     * This function returns the population count of 1's in the set
     */
    int count() const { return static_cast<int>(bits.count()); }

    /*
     * This function checks for set equality
     */
    bool
    isEqual(const Set& obj) const
    {
        assert(m_nSize == obj.m_nSize);
        return bits == obj.bits;
    }

    // return the logical OR of this set and orSet
    Set
    OR(const Set& obj) const
    {
        assert(m_nSize == obj.m_nSize);
        Set r(m_nSize);
        r.bits = bits | obj.bits;
        return r;
    };

    // return the logical AND of this set and andSet
    Set
    AND(const Set& obj) const
    {
        assert(m_nSize == obj.m_nSize);
        Set r(m_nSize);
        r.bits = bits & obj.bits;
        return r;
    }

    // Returns true if the intersection of the two sets is empty
    bool
    intersectionIsEmpty(const Set& obj) const
    {
        RubyDynamicBitset r = bits & obj.bits;
        return r.none();
    }

    /*
     * Returns false if a bit is set in the parameter set that is NOT set
     * in this set
     */
    bool
    isSuperset(const Set& test) const
    {
        assert(m_nSize == test.m_nSize);
        RubyDynamicBitset r = bits | test.bits;
        return (r == bits);
    }

    bool isSubset(const Set& test) const { return test.isSuperset(*this); }

    bool isElement(NodeID element) const { return bits.test(element); }

    /*
     * this function returns true iff all bits in use are set
     */
    bool
    isBroadcast() const
    {
        return (bits.count() == static_cast<std::size_t>(m_nSize));
    }

    bool isEmpty() const { return bits.none(); }

    NodeID smallestElement() const
    {
        for (int i = 0; i < m_nSize; ++i) {
            if (bits.test(i)) {
                return i;
            }
        }
        panic("No smallest element of an empty set.");
    }

    bool elementAt(int index) const { return bits[index]; }

    int getSize() const { return m_nSize; }

    void
    setSize(int size)
    {
        m_nSize = size;
        bits.resize(static_cast<std::size_t>(size));
    }

    void print(std::ostream& out) const
    {
        out << "[Set (" << m_nSize << "): " << bits << "]";
    }
};

inline std::ostream&
operator<<(std::ostream& out, const Set& obj)
{
    obj.print(out);
    out << std::flush;
    return out;
}

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_COMMON_SET_HH__
