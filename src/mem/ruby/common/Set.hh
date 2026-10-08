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

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
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

    explicit RubyDynamicBitset(std::size_t nbits) : m_size(nbits) {}

    std::size_t size() const { return m_size; }

    bool test(std::size_t pos) const
    {
        checkBounds(pos);
        if (hasSingleton())
            return m_index == pos;
        return (wordAt(wordIdx(pos)) >> bitIdx(pos)) & 1ULL;
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
        if (!m_words) {
            if (!value) {
                if (m_index == pos)
                    m_index = noBit;
                return;
            }
            if (m_index == noBit || m_index == pos) {
                m_index = pos;
                return;
            }
        }
        const auto index = wordIdx(pos);
        if (!value && (index < m_index || index >= m_index + words().size()))
            return;
        std::uint64_t mask = 1ULL << bitIdx(pos);
        auto &data = writableRange(index, index + 1);
        if (value)
            data[index - m_index] |= mask;
        else
            data[index - m_index] &= ~mask;
    }

    void set()
    {
        for (auto &w : writableWords()) w = ~0ULL;
    }

    void reset()
    {
        m_words.reset();
        m_index = noBit;
    }

    void reset(std::size_t pos)
    {
        set(pos, false);
    }

    bool none() const
    {
        if (hasSingleton())
            return false;
        for (auto w : words()) if (w) return false;
        return true;
    }

    std::size_t count() const
    {
        if (hasSingleton())
            return 1;
        std::size_t total = 0;
        for (auto w : words()) total += popcount(w);
        return total;
    }

    bool operator==(const RubyDynamicBitset &other) const
    {
        if (m_size != other.m_size)
            return false;
        if (!m_words && !other.m_words)
            return m_index == other.m_index;
        if (!m_words)
            return m_index == noBit ? other.none() :
                other.count() == 1 && other.test(m_index);
        if (!other.m_words)
            return other == *this;
        if (m_words == other.m_words && m_index == other.m_index)
            return true;
        for (std::size_t i = 0; i < wordsFor(m_size); ++i)
            if (wordAt(i) != other.wordAt(i))
                return false;
        return true;
    }
    bool operator!=(const RubyDynamicBitset &other) const
    {
        return !(*this == other);
    }

    RubyDynamicBitset operator|(const RubyDynamicBitset &other) const
    {
        assert(m_size == other.m_size);
        RubyDynamicBitset r(*this);
        r |= other;
        return r;
    }

    RubyDynamicBitset operator&(const RubyDynamicBitset &other) const
    {
        assert(m_size == other.m_size);
        RubyDynamicBitset r(*this);
        r &= other;
        return r;
    }

    RubyDynamicBitset operator~() const
    {
        RubyDynamicBitset r(m_size);
        auto &result = r.writableWords();
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = ~wordAt(i);
        return r;
    }

    RubyDynamicBitset &operator|=(const RubyDynamicBitset &other)
    {
        assert(m_size == other.m_size);
        if (this == &other)
            return *this;
        if (other.hasSingleton()) {
            set(other.m_index);
            return *this;
        }
        if (!other.m_words)
            return *this;
        if (!m_words && m_index == noBit) {
            m_words = other.m_words;
            m_index = other.m_index;
            return *this;
        }
        auto &data = writableWords();
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i] |= other.wordAt(i);
        return *this;
    }

    RubyDynamicBitset &operator&=(const RubyDynamicBitset &other)
    {
        assert(m_size == other.m_size);
        if (this == &other)
            return *this;
        if (other.hasSingleton()) {
            const auto pos = other.m_index;
            const bool retain = test(pos);
            reset();
            if (retain)
                m_index = pos;
            return *this;
        }
        if (!other.m_words) {
            reset();
            return *this;
        }
        if (hasSingleton()) {
            if (!other.test(m_index))
                reset();
            return *this;
        }
        if (!m_words)
            return *this;
        auto &data = writableWords();
        for (std::size_t i = 0; i < data.size(); ++i)
            data[i] &= other.wordAt(i);
        return *this;
    }

    void resize(std::size_t nbits)
    {
        m_size = nbits;
        reset();
    }

    bool intersects(const RubyDynamicBitset &other) const
    {
        assert(m_size == other.m_size);
        if (hasSingleton())
            return other.test(m_index);
        if (other.hasSingleton())
            return test(other.m_index);
        if (!m_words || !other.m_words)
            return false;
        const auto begin = std::max(m_index, other.m_index);
        const auto end = std::min(m_index + words().size(),
                                  other.m_index + other.words().size());
        for (std::size_t i = begin; i < end; ++i)
            if (wordAt(i) & other.wordAt(i))
                return true;
        return false;
    }

    template<class Callback>
    void forEachSetBit(Callback callback) const
    {
        if (hasSingleton()) {
            callback(m_index);
            return;
        }
        for (std::size_t i = 0; i < words().size(); ++i) {
            auto word = words()[i];
            while (word) {
                const auto pos = (i + m_index) * kBitsPerWord + trailingZeros(word);
                if (pos >= m_size)
                    break;
                callback(pos);
                word &= word - 1;
            }
        }
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
    using Words = std::vector<std::uint64_t>;
    static constexpr std::size_t noBit = std::numeric_limits<std::size_t>::max();

    std::uint64_t wordAt(std::size_t index) const
    {
        if (hasSingleton())
            return wordIdx(m_index) == index ? 1ULL << bitIdx(m_index) : 0;
        return m_words && index >= m_index && index - m_index < words().size()
            ? words()[index - m_index] : 0;
    }

    const Words &words() const
    {
        static const Words empty;
        return m_words ? *m_words : empty;
    }

    bool hasSingleton() const { return !m_words && m_index != noBit; }

    Words &writableRange(std::size_t begin, std::size_t end)
    {
        if (!m_words) {
            const auto singleton = m_index;
            if (singleton != noBit) {
                begin = std::min(begin, wordIdx(singleton));
                end = std::max(end, wordIdx(singleton) + 1);
            }
            auto data = std::make_shared<Words>(end - begin, 0);
            if (singleton != noBit)
                (*data)[wordIdx(singleton) - begin] = 1ULL << bitIdx(singleton);
            m_words = std::move(data);
            m_index = begin;
        } else {
            if (!m_words.unique())
                m_words = std::make_shared<Words>(*m_words);
            const auto first = std::min(begin, m_index);
            const auto length = std::max(end, m_index + m_words->size()) - first;
            if (length > m_words->capacity())
                m_words->reserve(std::min(wordsFor(m_size) - first,
                    std::max(length, m_words->capacity() * 2)));
            if (begin < m_index) {
                m_words->insert(m_words->begin(), m_index - begin, 0);
                m_index = begin;
            }
            if (end > m_index + m_words->size())
                m_words->resize(end - m_index, 0);
        }
        return *m_words;
    }

    Words &writableWords() { return writableRange(0, wordsFor(m_size)); }

    static constexpr std::size_t kBitsPerWord = 64;

    static std::size_t wordsFor(std::size_t nbits)
    {
        return (nbits + kBitsPerWord - 1) / kBitsPerWord;
    }

    static std::size_t wordIdx(std::size_t pos) { return pos / kBitsPerWord; }
    static std::size_t bitIdx(std::size_t pos) { return pos % kBitsPerWord; }

    static std::size_t trailingZeros(std::uint64_t word)
    {
#if defined(__GNUC__) || defined(__clang__)
        return static_cast<std::size_t>(__builtin_ctzll(word));
#else
        std::size_t count = 0;
        while (!(word & 1ULL)) { word >>= 1; ++count; }
        return count;
#endif
    }

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
    // Inline bit index when backing is null; first stored word otherwise.
    std::size_t m_index = noBit;
    // A null backing array represents an empty or inline singleton set.
    // Copies retain value semantics: reads share immutable storage; every
    // mutator obtains an independent backing array before changing a word.
    std::shared_ptr<Words> m_words;
};

class Set
{
  private:
    RubyDynamicBitset bits;

  public:
    Set() = default;

    Set(int size) : bits(static_cast<std::size_t>(size)) {}

    Set(const Set& obj) = default;
    ~Set() = default;
    Set& operator=(const Set& obj) = default;

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
        assert(getSize() == obj.getSize());
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
        assert(getSize() == obj.getSize());
        bits &= (~obj.bits);
    }

    void clear() { bits.reset(); }

    /*
     * this function sets all bits in the set
     */
    void broadcast()
    {
        bits.resize(static_cast<std::size_t>(getSize()));
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
        assert(getSize() == obj.getSize());
        return bits == obj.bits;
    }

    // return the logical OR of this set and orSet
    Set
    OR(const Set& obj) const
    {
        assert(getSize() == obj.getSize());
        Set r(getSize());
        r.bits = bits | obj.bits;
        return r;
    };

    // return the logical AND of this set and andSet
    Set
    AND(const Set& obj) const
    {
        assert(getSize() == obj.getSize());
        Set r(getSize());
        r.bits = bits & obj.bits;
        return r;
    }

    // Returns true if the intersection of the two sets is empty
    bool
    intersectionIsEmpty(const Set& obj) const
    {
        return !bits.intersects(obj.bits);
    }

    /*
     * Returns false if a bit is set in the parameter set that is NOT set
     * in this set
     */
    bool
    isSuperset(const Set& test) const
    {
        assert(getSize() == test.getSize());
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
        return (bits.count() == static_cast<std::size_t>(getSize()));
    }

    bool isEmpty() const { return bits.none(); }

    NodeID smallestElement() const
    {
        for (int i = 0; i < getSize(); ++i) {
            if (bits.test(i)) {
                return i;
            }
        }
        panic("No smallest element of an empty set.");
    }

    bool elementAt(int index) const { return bits[index]; }

    int getSize() const { return static_cast<int>(bits.size()); }

    template<class Callback>
    void forEachElement(Callback callback) const
    {
        bits.forEachSetBit(callback);
    }

    void
    setSize(int size)
    {
        bits.resize(static_cast<std::size_t>(size));
    }

    void print(std::ostream& out) const
    {
        out << "[Set (" << getSize() << "): " << bits << "]";
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
