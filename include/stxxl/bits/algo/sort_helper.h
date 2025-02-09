/***************************************************************************
 *  include/stxxl/bits/algo/sort_helper.h
 *
 *  Part of the STXXL. See http://stxxl.org
 *
 *  Copyright (C) 2002-2003 Roman Dementiev <dementiev@mpi-sb.mpg.de>
 *  Copyright (C) 2009, 2010 Andreas Beckmann <beckmann@cs.uni-frankfurt.de>
 *
 *  Distributed under the Boost Software License, Version 1.0.
 *  (See accompanying file LICENSE_1_0.txt or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 **************************************************************************/

#ifndef STXXL_ALGO_SORT_HELPER_HEADER
#define STXXL_ALGO_SORT_HELPER_HEADER

#include <algorithm>
#include <functional>

#include <tlx/define.hpp>
#include <tlx/logger/core.hpp>
#include <tlx/unused.hpp>

#include <stxxl/bits/algo/run_cursor.h>

namespace stxxl {

//! \internal
namespace sort_helper {

template <typename StrictWeakOrderingWithMinMax>
inline void verify_sentinel_strict_weak_ordering(StrictWeakOrderingWithMinMax cmp)
{
    assert(!cmp(cmp.min_value(), cmp.min_value()));
    assert(cmp(cmp.min_value(), cmp.max_value()));
    assert(!cmp(cmp.max_value(), cmp.min_value()));
    assert(!cmp(cmp.max_value(), cmp.max_value()));
    tlx::unused(cmp);
}

template <typename BlockType, typename ValueType = typename BlockType::value_type>
struct trigger_entry
{
    using block_type = BlockType;
    using bid_type = typename block_type::bid_type;
    using value_type = ValueType;

    bid_type bid;
    value_type value;

    operator bid_type ()
    {
        return bid;
    }
};

template <typename TriggerEntryType, typename ValueCmp>
struct trigger_entry_cmp
{
    using trigger_entry_type = TriggerEntryType;
    ValueCmp cmp;
    explicit trigger_entry_cmp(ValueCmp c) : cmp(c) { }
    trigger_entry_cmp(const trigger_entry_cmp& a) : cmp(a.cmp) { }
    bool operator () (const trigger_entry_type& a, const trigger_entry_type& b) const
    {
        return cmp(a.value, b.value);
    }
};

template <typename BlockType,
          typename PrefetcherType,
          typename ValueCmp>
struct run_cursor2_cmp
{
    using block_type = BlockType;
    using prefetcher_type = PrefetcherType;
    using value_cmp = ValueCmp;

    using cursor_type = run_cursor2<block_type, prefetcher_type>;
    value_cmp cmp;

    explicit run_cursor2_cmp(value_cmp c) : cmp(c) { }
    run_cursor2_cmp(const run_cursor2_cmp& a) : cmp(a.cmp) { }
    inline bool operator () (const cursor_type& a, const cursor_type& b) const
    {
        if (TLX_UNLIKELY(b.empty()))
            return true;
        // sentinel emulation
        if (TLX_UNLIKELY(a.empty()))
            return false;
        // sentinel emulation

        return (cmp(a.current(), b.current()));
    }
};

// this function is used by parallel mergers
template <typename SequenceVector, typename ValueType, typename Comparator>
inline size_t
count_elements_less_equal(const SequenceVector& seqs,
                          const ValueType& bound, Comparator cmp)
{
    using seqs_size_type = typename SequenceVector::size_type;
    using iterator = typename SequenceVector::value_type::first_type;
    size_t count = 0;

    for (seqs_size_type i = 0; i < seqs.size(); ++i)
    {
        iterator position = std::upper_bound(seqs[i].first, seqs[i].second, bound, cmp);
        TLX_LOG0 << "less equal than " << position - seqs[i].first;
        count += position - seqs[i].first;
    }
    TLX_LOG0 << "finished loop";
    return count;
}

// this function is used by parallel mergers
template <typename SequenceVector, typename BufferPtrVector, typename Prefetcher>
inline void
refill_or_remove_empty_sequences(SequenceVector& seqs,
                                 BufferPtrVector& buffers,
                                 Prefetcher& prefetcher)
{
    using seqs_size_type = typename SequenceVector::size_type;

    for (seqs_size_type i = 0; i < seqs.size(); ++i)
    {
        if (seqs[i].first == seqs[i].second)                    // run empty
        {
            if (prefetcher.block_consumed(buffers[i]))
            {
                seqs[i].first = buffers[i]->begin();            // reset iterator
                seqs[i].second = buffers[i]->end();
                TLX_LOG0 << "block ran empty " << i;
            }
            else
            {
                seqs.erase(seqs.begin() + i);                   // remove this sequence
                buffers.erase(buffers.begin() + i);
                TLX_LOG0 << "seq removed " << i;
                --i;                                            // don't skip the next sequence
            }
        }
    }
}

} // namespace sort_helper
} // namespace stxxl

#endif // !STXXL_ALGO_SORT_HELPER_HEADER
