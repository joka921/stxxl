/***************************************************************************
 *  include/stxxl/bits/containers/sorter.h
 *
 *  Part of the STXXL. See http://stxxl.org
 *
 *  Copyright (C) 2012 Timo Bingmann <tb@panthema.net>
 *
 *  Distributed under the Boost Software License, Version 1.0.
 *  (See accompanying file LICENSE_1_0.txt or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 **************************************************************************/

#ifndef STXXL_CONTAINERS_SORTER_HEADER
#define STXXL_CONTAINERS_SORTER_HEADER

#include <algorithm>

#include <stxxl/bits/deprecated.h>
#include <stxxl/bits/stream/sort_stream.h>

namespace stxxl {

//! \addtogroup stlcont
//! \{

//! External sorter container. \n
//! <b> Introduction </b> to sorter container: see \ref tutorial_sorter tutorial. \n
//! <b> Design and Internals </b> of sorter container: see \ref design_sorter

/**
 * External Sorter: use stream package objects to keep a sorted container.
 *
 * This sorter container combines the two functions of runs_creator and
 * runs_merger from the stream packages into a two-phase container.
 *
 * In the first phase the container is filled with unordered items via push(),
 * which are presorted internally into runs of size M. When the internal memory
 * overflows a runs is written to external memory in blocks of block_size.
 *
 * When sort() is called the container enters the output phase and push() is
 * disallowed. After calling sort() the items can be read in sorted order using
 * operator*() to get the top item, operator++() to advance to the next one and
 * empty() to check for end of stream. This is exactly the stream interface.
 *
 * In the output phase the sorter can be returned to the beginning of the
 * stream using rewind() and everything is read again in sorted order.
 *
 * Using clear() the object can be reset into input state and all items are
 * destroyed.
 *
 * Added in STXXL 1.4
 *
 * \tparam ValueType   type of the contained objects (POD with no references to internal memory)
 * \tparam CompareType type of comparison object used for sorting the runs
 * \tparam BlockSize   size of the external memory block in bytes, default is \c STXXL_DEFAULT_BLOCK_SIZE(ValTp)
 * \tparam AllocStr    parallel disk block allocation strategy, default is \c foxxll::default_alloc_strategy
 */
template <typename ValueType,
          typename CompareType,
          size_t BlockSize = STXXL_DEFAULT_BLOCK_SIZE(ValueType),
          class AllocStrategy = foxxll::default_alloc_strategy>
class sorter
{
public:
    // *** Template Parameters

    using value_type = ValueType;
    using cmp_type = CompareType;
    enum {
        block_size = BlockSize
    };
    using alloc_strategy_type = AllocStrategy;

    // *** Constructed Types

    //! runs creator type with push() method
    using runs_creator_type = stream::runs_creator<stream::use_push<ValueType>, cmp_type,
                                                   block_size, alloc_strategy_type>;

    //! corresponding runs merger type
    using runs_merger_type = stream::runs_merger<typename runs_creator_type::sorted_runs_type,
                                                 cmp_type, alloc_strategy_type>;

    //! size type
    using size_type = typename runs_merger_type::size_type;

protected:
    // *** Object Attributes

    //! current state of sorter
    enum { STATE_INPUT, STATE_OUTPUT } m_state;

    //! runs creator object holding all items
    runs_creator_type m_runs_creator;

    //! runs merger reading items when in STATE_OUTPUT
    runs_merger_type m_runs_merger;

public:
    //! \name Constructors
    //! \{

    //! Constructor allocation memory_to_use bytes in ram for sorted runs.
    sorter(const cmp_type& cmp, size_t memory_to_use)
        : m_state(STATE_INPUT),
          m_runs_creator(cmp, memory_to_use),
          m_runs_merger(cmp, memory_to_use)
    { }

    //! Constructor variant with differently sizes runs_creator and runs_merger
    sorter(const cmp_type& cmp, size_t creator_memory_to_use, size_t merger_memory_to_use)
        : m_state(STATE_INPUT),
          m_runs_creator(cmp, creator_memory_to_use),
          m_runs_merger(cmp, merger_memory_to_use)

    { }

    //! non-copyable: delete copy-constructor
    sorter(const sorter&) = delete;
    //! non-copyable: delete assignment operator
    sorter& operator = (const sorter&) = delete;

    //! \}

    //! \name Modifiers
    //! \{

    //! Remove all items and return to input state.
    void clear()
    {
        if (m_state == STATE_OUTPUT)
            m_runs_merger.deallocate();

        m_runs_creator.allocate();
        m_state = STATE_INPUT;
    }

    //! Push another item (only callable during input state).
    void push(const value_type& val)
    {
        assert(m_state == STATE_INPUT);
        m_runs_creator.push(val);
    }

    //! \}

    //! \name Modus
    //! \{

    //! Finish push input state and deallocate input buffer.
    void finish()
    {
        if (m_state == STATE_OUTPUT)
        {
            m_runs_merger.deallocate();
        }

        m_runs_creator.deallocate();
    }

    //! Deallocate buffers and clear result.
    void finish_clear()
    {
        if (m_state == STATE_OUTPUT)
        {
            m_runs_merger.deallocate();
            m_runs_creator.result()->clear();
        }

        m_runs_creator.deallocate();
    }

    //! \}

    //! \name Modifiers
    //! \{

    //! Switch to output state, rewind() in case the output was already sorted.
    void sort()
    {
        if (m_state == STATE_OUTPUT)
        {
            m_runs_merger.deallocate();
        }

        m_runs_creator.deallocate();
        m_runs_merger.initialize(m_runs_creator.result());
        m_state = STATE_OUTPUT;
    }

    //! Switch to output state, rewind() in case the output was already sorted.
    void sort(size_t merger_memory_to_use)
    {
        m_runs_merger.set_memory_to_use(merger_memory_to_use);
        sort();
    }

    //! \}

    //! \name Modus
    //! \{

    //! Switch to output state, rewind() in case the output was already sorted.
    void sort_reuse()
    {
        assert(m_state == STATE_INPUT);

        m_runs_merger.initialize(m_runs_creator.result());
        m_state = STATE_OUTPUT;
    }

    //! Rewind output stream to beginning.
    void rewind()
    {
        assert(m_state == STATE_OUTPUT);

        m_runs_merger.deallocate();

        m_state = STATE_INPUT;
        return sort();
    }

    //! \}

    //! Change runs_merger memory usage
    void set_merger_memory_to_use(size_t merger_memory_to_use)
    {
        m_runs_merger.set_memory_to_use(merger_memory_to_use);
    }

    //! \}

    //! \name Capacity
    //! \{

    //! Number of items pushed or items remaining to be read.
    size_type size() const
    {
        if (m_state == STATE_INPUT)
            return m_runs_creator.size();
        else
            return m_runs_merger.size();
    }
    //! Standard stream method
    bool empty() const
    {
        assert(m_state == STATE_OUTPUT);
        return m_runs_merger.empty();
    }

    //! Return the number of elements in a single run of the runs_creator
    size_t num_els_in_run() const {
      return m_runs_creator.num_els_in_run();
    }

    //! This number of elements will be merged at one
    size_t num_els_in_output_block() const {
      return m_runs_merger.num_els_in_output_block();
    }
    //! Will the next call to operator++ block, because it needs to merge?
    bool next_call_would_block() const {
      return m_runs_merger.next_call_would_block();
    }

    //! \}

    //! \name Operators
    //! \{

    //! Standard stream method
    const value_type& operator * () const
    {
        assert(m_state == STATE_OUTPUT);
        return *m_runs_merger;
    }

    //! Standard stream method
    const value_type* operator -> () const
    {
        return &(operator * ());
    }

    //! Standard stream method (preincrement operator)
    sorter& operator ++ ()
    {
        assert(m_state == STATE_OUTPUT);
        ++m_runs_merger;
        return *this;
    }

    bool next_output_would_block() const {
      return m_runs_merger.next_output_would_block();
    }

    size_t output_block_size() const {
      return m_runs_merger.output_block_size();
    }

    //! \}
};

//! \}

} // namespace stxxl

#endif // !STXXL_CONTAINERS_SORTER_HEADER
