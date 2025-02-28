/***************************************************************************
 *  include/stxxl/bits/stream/sort_stream.h
 *
 *  Part of the STXXL. See http://stxxl.org
 *
 *  Copyright (C) 2002-2005 Roman Dementiev <dementiev@mpi-sb.mpg.de>
 *  Copyright (C) 2006-2008 Johannes Singler <singler@ira.uka.de>
 *  Copyright (C) 2008-2010 Andreas Beckmann <beckmann@cs.uni-frankfurt.de>
 *  Copyright (C) 2013 Timo Bingmann <tb@panthema.net>
 *
 *  Distributed under the Boost Software License, Version 1.0.
 *  (See accompanying file LICENSE_1_0.txt or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 **************************************************************************/

#ifndef STXXL_STREAM_SORT_STREAM_HEADER
#define STXXL_STREAM_SORT_STREAM_HEADER

#include <algorithm>
#include <cassert>
#include <functional>
#include <utility>
#include <vector>

#include <tlx/counting_ptr.hpp>
#include <tlx/define.hpp>
#include <tlx/logger/core.hpp>

#include <foxxll/mng/block_manager.hpp>

#include <stxxl/bits/algo/losertree.h>
#include <stxxl/bits/algo/run_cursor.h>
#include <stxxl/bits/algo/sort_base.h>
#include <stxxl/bits/algo/sort_helper.h>
#include <stxxl/bits/algo/trigger_entry.h>
#include <stxxl/bits/config.h>
#include <stxxl/bits/parallel.h>
#include <stxxl/bits/stream/sorted_runs.h>
#include <stxxl/bits/stream/stream.h>

namespace stxxl {
namespace stream {

//! \addtogroup streampack Stream Package
//! \{

////////////////////////////////////////////////////////////////////////
//     CREATE RUNS                                                    //
////////////////////////////////////////////////////////////////////////

//! Forms sorted runs of data from a stream.
//!
//! \tparam Input type of the input stream
//! \tparam CompareWithMax type of comparison object used for sorting the runs
//! \tparam BlockSize size of blocks used to store the runs (in bytes)
//! \tparam AllocStr functor that defines allocation strategy for the runs
template <
    class Input,
    class CompareWithMax,
    size_t BlockSize = STXXL_DEFAULT_BLOCK_SIZE(typename Input::value_type),
    class AllocStr = foxxll::default_alloc_strategy>
class basic_runs_creator
{
public:
    using input_type = Input;
    using cmp_type = CompareWithMax;
    static const size_t block_size = BlockSize;
    using allocation_strategy_type = AllocStr;

public:
    using value_type = typename Input::value_type;
    using block_type = foxxll::typed_block<BlockSize, value_type>;
    using trigger_entry_type = sort_helper::trigger_entry<block_type>;
    using sorted_runs_data_type = sorted_runs<trigger_entry_type, cmp_type>;
    using run_type = typename sorted_runs_data_type::run_type;
    using sorted_runs_type = tlx::counting_ptr<sorted_runs_data_type>;

    using element_iterator = typename element_iterator_traits<block_type, external_size_type>::element_iterator;

protected:
    //! reference to the input stream
    Input& m_input;
    //! comparator used to sort block groups
    CompareWithMax m_cmp;

private:
    //! stores the result (sorted runs) as smart pointer
    sorted_runs_type m_result;
    //! memory for internal use in blocks
    size_t m_memsize;
    //! true iff result is already computed (used in 'result()' method)
    bool m_result_computed;

    //! Fetch data from input into blocks[first_idx,last_idx).
    size_t fetch(block_type* blocks,
                 size_t first_idx, size_t last_idx)
    {
        element_iterator output = make_element_iterator(blocks, first_idx);
        size_t curr_idx = first_idx;
        while (!m_input.empty() && curr_idx != last_idx) {
            *output = *m_input;
            ++m_input;
            ++output;
            ++curr_idx;
        }
        return curr_idx;
    }

    //! fill the rest of the block with max values
    void fill_with_max_value(block_type* blocks, size_t num_blocks,
                             size_t first_idx)
    {
        size_t last_idx = num_blocks * block_type::size;
        if (first_idx < last_idx) {
            element_iterator curr = make_element_iterator(blocks, first_idx);
            while (first_idx != last_idx) {
                *curr = m_cmp.max_value();
                ++curr;
                ++first_idx;
            }
        }
    }

    //! Sort a specific run, contained in a sequences of blocks.
    void sort_run(block_type* run, size_t elements)
    {
        check_sort_settings();
        potentially_parallel::sort(make_element_iterator(run, 0),
                                   make_element_iterator(run, elements),
                                   m_cmp);
    }

    void compute_result();

public:
    //! Create the object.
    //! \param input input stream
    //! \param cmp comparator object
    //! \param memory_to_use memory amount that is allowed to used by the
    //! sorter in bytes
    basic_runs_creator(Input& input, CompareWithMax cmp,
                       size_t memory_to_use)
        : m_input(input),
          m_cmp(cmp),
          m_result(new sorted_runs_data_type),
          m_memsize(memory_to_use / BlockSize / sort_memory_usage_factor()),
          m_result_computed(false)
    {
        sort_helper::verify_sentinel_strict_weak_ordering(cmp);
        if (!(2 * BlockSize * sort_memory_usage_factor() <= memory_to_use)) {
            throw foxxll::bad_parameter(
                      "stxxl::runs_creator<>:runs_creator(): "
                      "INSUFFICIENT MEMORY provided, "
                      "please increase parameter 'memory_to_use'");
        }
        assert(m_memsize > 0);
    }

    //! non-copyable: delete copy-constructor
    basic_runs_creator(const basic_runs_creator&) = delete;
    //! non-copyable: delete assignment operator
    basic_runs_creator& operator = (const basic_runs_creator&) = delete;

    //! Returns the sorted runs object.
    //! \return Sorted runs object. The result is computed lazily, i.e. on the first call
    //! \remark Returned object is intended to be used by \c runs_merger object as input
    sorted_runs_type & result()
    {
        if (!m_result_computed)
        {
            compute_result();
            m_result_computed = true;
#ifdef STXXL_PRINT_STAT_AFTER_RF
            TLX_LOG1 << *stats::get_instance();
#endif //STXXL_PRINT_STAT_AFTER_RF
        }
        return m_result;
    }
};

//! Finish the results, i. e. create all runs.
//!
//! This is the main routine of this class.
template <class Input, class CompareType, size_t BlockSize, class AllocStr>
void basic_runs_creator<Input, CompareType, BlockSize, AllocStr>::compute_result()
{
    constexpr bool debug = false;
    using request_ptr = foxxll::request_ptr;

    size_t i = 0;
    size_t m2 = m_memsize / 2;
    const size_t el_in_run = m2 * block_type::size;     // # el in a run
    TLX_LOG << "basic_runs_creator::compute_result m2=" << m2;
    size_t blocks1_length = 0, blocks2_length = 0;
    block_type* Blocks1 = nullptr;

#ifndef STXXL_SMALL_INPUT_PSORT_OPT
    Blocks1 = new block_type[m2 * 2];
#else
    // push input element into small_run vector in result until it is full
    while (!input.empty() && blocks1_length != block_type::size)
    {
        m_result->small_run.push_back(*input);
        ++input;
        ++blocks1_length;
    }

    if (blocks1_length == block_type::size && !input.empty())
    {
        Blocks1 = new block_type[m2 * 2];
        std::copy(m_result->small_run.begin(), m_result->small_run.end(),
                  Blocks1[0].begin());
        m_result->small_run.clear();
    }
    else
    {
        TLX_LOG << "basic_runs_creator: Small input optimization, input length: " << blocks1_length;
        m_result->elements = blocks1_length;
        check_sort_settings();
        potentially_parallel::sort(m_result->small_run.begin(), m_result->small_run.end(), cmp);
        return;
    }
#endif //STXXL_SMALL_INPUT_PSORT_OPT

    // the first block may be there already, now fetch until memsize is filled.
    blocks1_length = fetch(Blocks1, blocks1_length, el_in_run);

    // sort first run
    sort_run(Blocks1, blocks1_length);

    if (blocks1_length <= block_type::size && m_input.empty())
    {
        // small input, do not flush it on the disk(s)
        TLX_LOG << "basic_runs_creator: Small input optimization, input length: " << blocks1_length;
        assert(m_result->small_run.empty());
        m_result->small_run.assign(Blocks1[0].begin(), Blocks1[0].begin() + blocks1_length);
        m_result->elements = blocks1_length;
        delete[] Blocks1;
        return;
    }

    block_type* Blocks2 = Blocks1 + m2;
    foxxll::block_manager* bm = foxxll::block_manager::get_instance();
    request_ptr* write_reqs = new request_ptr[m2];
    run_type run;

    size_t cur_run_size = foxxll::div_ceil(blocks1_length, block_type::size);      // in blocks
    run.resize(cur_run_size);
    bm->new_blocks(AllocStr(), make_bid_iterator(run.begin()), make_bid_iterator(run.end()));

    foxxll::disk_queues::get_instance()->set_priority_op(foxxll::request_queue::WRITE);

    // fill the rest of the last block with max values
    fill_with_max_value(Blocks1, cur_run_size, blocks1_length);

    for (i = 0; i < cur_run_size; ++i)
    {
        run[i].value = Blocks1[i][0];
        write_reqs[i] = Blocks1[i].write(run[i].bid);
    }
    m_result->runs.push_back(run);
    m_result->runs_sizes.push_back(blocks1_length);
    m_result->elements += blocks1_length;

    if (m_input.empty())
    {
        // return
        wait_all(write_reqs, write_reqs + cur_run_size);
        delete[] write_reqs;
        delete[] Blocks1;
        return;
    }

    TLX_LOG << "Filling the second part of the allocated blocks";
    blocks2_length = fetch(Blocks2, 0, el_in_run);

    if (m_input.empty())
    {
        // optimization if the whole set fits into both halves
        // (re)sort internally and return
        blocks2_length += el_in_run;
        sort_run(Blocks1, blocks2_length);      // sort first an second run together
        wait_all(write_reqs, write_reqs + cur_run_size);
        bm->delete_blocks(make_bid_iterator(run.begin()), make_bid_iterator(run.end()));

        cur_run_size = foxxll::div_ceil(blocks2_length, block_type::size);
        run.resize(cur_run_size);
        bm->new_blocks(AllocStr(), make_bid_iterator(run.begin()), make_bid_iterator(run.end()));

        // fill the rest of the last block with max values
        fill_with_max_value(Blocks1, cur_run_size, blocks2_length);

        assert(cur_run_size > m2);

        for (i = 0; i < m2; ++i)
        {
            run[i].value = Blocks1[i][0];
            write_reqs[i]->wait();
            write_reqs[i] = Blocks1[i].write(run[i].bid);
        }

        request_ptr* write_reqs1 = new request_ptr[cur_run_size - m2];

        for ( ; i < cur_run_size; ++i)
        {
            run[i].value = Blocks1[i][0];
            write_reqs1[i - m2] = Blocks1[i].write(run[i].bid);
        }

        m_result->runs[0] = run;
        m_result->runs_sizes[0] = blocks2_length;
        m_result->elements = blocks2_length;

        wait_all(write_reqs, write_reqs + m2);
        delete[] write_reqs;
        wait_all(write_reqs1, write_reqs1 + cur_run_size - m2);
        delete[] write_reqs1;

        delete[] Blocks1;

        return;
    }

    // more than 2 runs can be filled, i. e. the general case

    sort_run(Blocks2, blocks2_length);

    cur_run_size = foxxll::div_ceil(blocks2_length, block_type::size);      // in blocks
    run.resize(cur_run_size);
    bm->new_blocks(AllocStr(), make_bid_iterator(run.begin()), make_bid_iterator(run.end()));

    for (i = 0; i < cur_run_size; ++i)
    {
        run[i].value = Blocks2[i][0];
        write_reqs[i]->wait();
        write_reqs[i] = Blocks2[i].write(run[i].bid);
    }
    assert((blocks2_length % el_in_run) == 0);

    m_result->add_run(run, blocks2_length);

    while (!m_input.empty())
    {
        blocks1_length = fetch(Blocks1, 0, el_in_run);
        sort_run(Blocks1, blocks1_length);
        cur_run_size = foxxll::div_ceil(blocks1_length, block_type::size);      // in blocks
        run.resize(cur_run_size);
        bm->new_blocks(AllocStr(), make_bid_iterator(run.begin()), make_bid_iterator(run.end()));

        // fill the rest of the last block with max values (occurs only on the last run)
        fill_with_max_value(Blocks1, cur_run_size, blocks1_length);

        for (i = 0; i < cur_run_size; ++i)
        {
            run[i].value = Blocks1[i][0];
            write_reqs[i]->wait();
            write_reqs[i] = Blocks1[i].write(run[i].bid);
        }
        m_result->add_run(run, blocks1_length);

        std::swap(Blocks1, Blocks2);
        std::swap(blocks1_length, blocks2_length);
    }

    wait_all(write_reqs, write_reqs + m2);
    delete[] write_reqs;
    delete[] ((Blocks1 < Blocks2) ? Blocks1 : Blocks2);
}

//! Forms sorted runs of data from a stream.
//!
//! \tparam Input type of the input stream
//! \tparam CompareType type of omparison object used for sorting the runs
//! \tparam BlockSize size of blocks used to store the runs
//! \tparam AllocStr functor that defines allocation strategy for the runs
template <
    class Input,
    class CompareType,
    size_t BlockSize = STXXL_DEFAULT_BLOCK_SIZE(typename Input::value_type),
    class AllocStr = foxxll::default_alloc_strategy
    >
class runs_creator : public basic_runs_creator<Input, CompareType, BlockSize, AllocStr>
{
private:
    using base = basic_runs_creator<Input, CompareType, BlockSize, AllocStr>;

public:
    using cmp_type = typename base::cmp_type;
    using value_type = typename base::value_type;
    using block_type = typename base::block_type;
    using sorted_runs_data_type = typename base::sorted_runs_data_type;
    using sorted_runs_type = typename base::sorted_runs_type;

public:
    //! Creates the object.
    //! \param input input stream
    //! \param cmp comparator object
    //! \param memory_to_use memory amount that is allowed to used by the
    //! sorter in bytes
    runs_creator(Input& input, CompareType cmp, size_t memory_to_use)
        : base(input, cmp, memory_to_use)
    { }
};

//! Input strategy for \c runs_creator class.
//!
//! This strategy together with \c runs_creator class
//! allows to create sorted runs
//! data structure usable for \c runs_merger
//! pushing elements into the sorter
//! (using runs_creator::push())
template <class ValueType>
struct use_push
{
    using value_type = ValueType;
};

//! Forms sorted runs of elements passed in push() method.
//!
//! A specialization of \c runs_creator that
//! allows to create sorted runs
//! data structure usable for \c runs_merger from
//! elements passed in sorted push() method. <BR>
//! \tparam ValueType type of values (parameter for \c use_push strategy)
//! \tparam CompareType type of comparison object used for sorting the runs
//! \tparam BlockSize size of blocks used to store the runs
//! \tparam AllocStr functor that defines allocation strategy for the runs
template <
    class ValueType,
    class CompareType,
    size_t BlockSize,
    class AllocStr
    >
class runs_creator<
        use_push<ValueType>,
        CompareType,
        BlockSize,
        AllocStr
        >
{
    static constexpr bool debug = false;

public:
    using cmp_type = CompareType;
    using value_type = ValueType;
    using block_type = foxxll::typed_block<BlockSize, value_type>;
    using trigger_entry_type = sort_helper::trigger_entry<block_type>;
    using sorted_runs_data_type = sorted_runs<trigger_entry_type, cmp_type>;
    using sorted_runs_type = tlx::counting_ptr<sorted_runs_data_type>;
    using request_ptr = foxxll::request_ptr;
    using result_type = sorted_runs_type;

    using element_iterator = typename element_iterator_traits<block_type, external_size_type>::element_iterator;

private:
    //! comparator object to sort runs
    CompareType m_cmp;

    using run_type = typename sorted_runs_data_type::run_type;

    //! stores the result (sorted runs) in a reference counted object
    sorted_runs_type m_result;

    //! memory size in bytes to use
    const size_t m_memory_to_use;

    //! memory size in numberr of blocks for internal use
    const size_t m_memsize;

    //! m_memsize / 2
    const size_t m_m2;

    //! true after the result() method was called for the first time
    bool m_result_computed;

    //! total number of elements in a run
    const size_t m_el_in_run;

    //! current number of elements in the run m_blocks1
    size_t m_cur_el;

    //! accumulation buffer of size m_m2 blocks, half the available memory size
    block_type* m_blocks1;

    //! accumulation buffer that is currently being written to disk
    block_type* m_blocks2;

    //! reference to write requests transporting the last accumulation buffer
    //! to disk
    request_ptr* m_write_reqs;

    //! run object containing block ids of the run being written to disk
    run_type run;

protected:
    //!  fill the rest of the block with max values
    void fill_with_max_value(block_type* blocks, size_t num_blocks,
                             size_t first_idx)
    {
        size_t last_idx = num_blocks * block_type::size;
        if (first_idx < last_idx) {
            element_iterator curr = make_element_iterator(blocks, first_idx);
            while (first_idx != last_idx) {
                *curr = m_cmp.max_value();
                ++curr;
                ++first_idx;
            }
        }
    }

    //! Sort a specific run, contained in a sequences of blocks.
    void sort_run(block_type* run, size_t elements)
    {
        check_sort_settings();
        potentially_parallel::sort(make_element_iterator(run, 0),
                                   make_element_iterator(run, elements),
                                   m_cmp);
    }

    void compute_result()
    {
        if (m_cur_el == 0)
            return;

        sort_run(m_blocks1, m_cur_el);

        if (m_cur_el <= block_type::size && m_result->elements == 0)
        {
            // small input, do not flush it on the disk(s)
            TLX_LOG << "runs_creator(use_push): Small input optimization, input length: " << m_cur_el;
            m_result->small_run.assign(m_blocks1[0].begin(), m_blocks1[0].begin() + m_cur_el);
            m_result->elements = m_cur_el;
            return;
        }

        const size_t cur_run_size = foxxll::div_ceil(m_cur_el, block_type::size);         // in blocks
        run.resize(cur_run_size);
        foxxll::block_manager* bm = foxxll::block_manager::get_instance();
        bm->new_blocks(AllocStr(), make_bid_iterator(run.begin()), make_bid_iterator(run.end()));

        foxxll::disk_queues::get_instance()->set_priority_op(foxxll::request_queue::WRITE);

        // fill the rest of the last block with max values
        fill_with_max_value(m_blocks1, cur_run_size, m_cur_el);

        size_t i = 0;
        for ( ; i < cur_run_size; ++i)
        {
            run[i].value = m_blocks1[i][0];
            if (m_write_reqs[i].get())
                m_write_reqs[i]->wait();

            m_write_reqs[i] = m_blocks1[i].write(run[i].bid);
        }
        m_result->add_run(run, m_cur_el);

        for (i = 0; i < m_m2; ++i)
        {
            if (m_write_reqs[i].get())
                m_write_reqs[i]->wait();
        }
    }

public:
    //! Creates the object.
    //! \param cmp comparator object
    //! \param memory_to_use memory amount that is allowed to used by the sorter in bytes
    runs_creator(CompareType cmp, size_t memory_to_use)
        : m_cmp(cmp),
          m_memory_to_use(memory_to_use),
          m_memsize(memory_to_use / BlockSize / sort_memory_usage_factor()),
          m_m2(m_memsize / 2),
          m_el_in_run(m_m2 * block_type::size),
          m_blocks1(nullptr), m_blocks2(nullptr),
          m_write_reqs(nullptr)
    {
        sort_helper::verify_sentinel_strict_weak_ordering(m_cmp);
        if (!(2 * BlockSize * sort_memory_usage_factor() <= m_memory_to_use)) {
            throw foxxll::bad_parameter(
                      "stxxl::runs_creator<>:runs_creator(): "
                      "INSUFFICIENT MEMORY provided, "
                      "please increase parameter 'memory_to_use'");
        }
        assert(m_m2 > 0);

        allocate();
    }

    //! non-copyable: delete copy-constructor
    runs_creator(const runs_creator&) = delete;
    //! non-copyable: delete assignment operator
    runs_creator& operator = (const runs_creator&) = delete;

    ~runs_creator()
    {
        m_result_computed = 1;
        deallocate();
    }

    //! Clear current state and remove all items.
    void clear()
    {
        if (!m_result)
            m_result = sorted_runs_type(new sorted_runs_data_type);
        else
            m_result->clear();

        m_result_computed = false;
        m_cur_el = 0;

        for (size_t i = 0; i < m_m2; ++i)
        {
            if (m_write_reqs[i].get())
                m_write_reqs[i]->cancel();
        }
    }

    //! Allocates input buffers and clears result.
    void allocate()
    {
        if (!m_blocks1)
        {
            m_blocks1 = new block_type[m_m2 * 2];
            m_blocks2 = m_blocks1 + m_m2;

            m_write_reqs = new request_ptr[m_m2];
        }

        clear();
    }

    //! Deallocates input buffers but not the current result.
    void deallocate()
    {
        result();       // finishes result

        if (m_blocks1)
        {
            delete[] ((m_blocks1 < m_blocks2) ? m_blocks1 : m_blocks2);
            m_blocks1 = m_blocks2 = nullptr;

            delete[] m_write_reqs;
            m_write_reqs = nullptr;
        }
    }

    //! Adds new element to the sorter.
    //! \param val value to be added
    void push(const value_type& val)
    {
        assert(m_result_computed == false);
        if (TLX_LIKELY(m_cur_el < m_el_in_run))
        {
            m_blocks1[m_cur_el / block_type::size][m_cur_el % block_type::size] = val;
            ++m_cur_el;
            return;
        }

        assert(m_el_in_run == m_cur_el);
        m_cur_el = 0;

        // sort and store m_blocks1
        sort_run(m_blocks1, m_el_in_run);

        const size_t cur_run_blocks = foxxll::div_ceil(m_el_in_run, block_type::size);        // in blocks
        run.resize(cur_run_blocks);
        foxxll::block_manager* bm = foxxll::block_manager::get_instance();
        bm->new_blocks(AllocStr(), make_bid_iterator(run.begin()), make_bid_iterator(run.end()));

        foxxll::disk_queues::get_instance()->set_priority_op(foxxll::request_queue::WRITE);

        for (size_t i = 0; i < cur_run_blocks; ++i)
        {
            run[i].value = m_blocks1[i][0];
            if (m_write_reqs[i].get())
                m_write_reqs[i]->wait();

            m_write_reqs[i] = m_blocks1[i].write(run[i].bid);
        }

        m_result->add_run(run, m_el_in_run);

        std::swap(m_blocks1, m_blocks2);

        push(val);
    }

    //! Returns the sorted runs object.
    //! \return Sorted runs object.
    //! \remark Returned object is intended to be used by \c runs_merger object as input
    sorted_runs_type & result()
    {
        if (!m_result_computed)
        {
            compute_result();
            m_result_computed = true;
#ifdef STXXL_PRINT_STAT_AFTER_RF
            TLX_LOG1 << *stats::get_instance();
#endif //STXXL_PRINT_STAT_AFTER_RF
        }
        return m_result;
    }

    //! number of items currently inserted.
    external_size_type size() const
    {
        return m_result->elements + m_cur_el;
    }

    //! return comparator object.
    const cmp_type & cmp() const
    {
        return m_cmp;
    }

    //! return memory size used (in bytes).
    size_t memory_used() const
    {
        return m_memory_to_use;
    }

    //! return number of elements in a single sort run
    size_t num_els_in_run() const {
      return m_el_in_run;
    }
};

//! Input strategy for \c runs_creator class.
//!
//! This strategy together with \c runs_creator class
//! allows to create sorted runs
//! data structure usable for \c runs_merger from
//! sequences of elements in sorted order
template <class ValueType>
struct from_sorted_sequences
{
    using value_type = ValueType;
};

//! Forms sorted runs of data taking elements in sorted order (element by element).
//!
//! A specialization of \c runs_creator that
//! allows to create sorted runs
//! data structure usable for \c runs_merger from
//! sequences of elements in sorted order. <BR>
//! \tparam ValueType type of values (parameter for \c from_sorted_sequences strategy)
//! \tparam CompareType type of comparison object used for sorting the runs
//! \tparam BlockSize size of blocks used to store the runs
//! \tparam AllocStr functor that defines allocation strategy for the runs
template <
    class ValueType,
    class CompareType,
    size_t BlockSize,
    class AllocStr
    >
class runs_creator<
        from_sorted_sequences<ValueType>,
        CompareType,
        BlockSize,
        AllocStr
        >
{
public:
    using value_type = ValueType;
    using block_type = foxxll::typed_block<BlockSize, value_type>;
    using trigger_entry_type = sort_helper::trigger_entry<block_type>;
    using alloc_strategy_type = AllocStr;

public:
    using cmp_type = CompareType;
    using sorted_runs_data_type = sorted_runs<trigger_entry_type, cmp_type>;
    using sorted_runs_type = tlx::counting_ptr<sorted_runs_data_type>;
    using result_type = sorted_runs_type;

private:
    using run_type = typename sorted_runs_data_type::run_type;

    CompareType cmp;

    //! stores the result (sorted runs)
    sorted_runs_type result_;
    //! memory for internal use in blocks
    size_t m_;
    foxxll::buffered_writer<block_type> writer;
    block_type* cur_block;
    size_t offset;
    size_t iblock;
    size_t irun;
    //! needs to be reset after each run
    alloc_strategy_type alloc_strategy;

public:
    //! Creates the object.
    //! \param c comparator object
    //! \param memory_to_use memory amount that is allowed to used by the sorter in bytes.
    //! Recommended value: 2 * block_size * D
    runs_creator(CompareType c, size_t memory_to_use)
        : cmp(c),
          result_(new sorted_runs_data_type),
          m_(memory_to_use / BlockSize / sort_memory_usage_factor()),
          writer(m_, m_ / 2),
          cur_block(writer.get_free_block()),
          offset(0),
          iblock(0),
          irun(0)
    {
        sort_helper::verify_sentinel_strict_weak_ordering(cmp);
        assert(m_ > 0);
        if (!(2 * BlockSize * sort_memory_usage_factor() <= memory_to_use)) {
            throw foxxll::bad_parameter(
                      "stxxl::runs_creator<>:runs_creator(): "
                      "INSUFFICIENT MEMORY provided, "
                      "please increase parameter 'memory_to_use'");
        }
    }

    //! non-copyable: delete copy-constructor
    runs_creator(const runs_creator&) = delete;
    //! non-copyable: delete assignment operator
    runs_creator& operator = (const runs_creator&) = delete;

    //! Adds new element to the current run.
    //! \param val value to be added to the current run
    void push(const value_type& val)
    {
        assert(offset < block_type::size);

        (*cur_block)[offset] = val;
        ++offset;

        if (offset == block_type::size)
        {
            // write current block

            foxxll::block_manager* bm = foxxll::block_manager::get_instance();
            // allocate space for the block
            result_->runs.resize(irun + 1);
            result_->runs[irun].resize(iblock + 1);
            bm->new_blocks(
                alloc_strategy,
                make_bid_iterator(result_->runs[irun].begin() + iblock),
                make_bid_iterator(result_->runs[irun].end()),
                iblock);

            result_->runs[irun][iblock].value = (*cur_block)[0];             // init trigger
            cur_block = writer.write(cur_block, result_->runs[irun][iblock].bid);
            ++iblock;

            offset = 0;
        }

        ++result_->elements;
    }

    //! Finishes current run and begins new one.
    void finish()
    {
        if (offset == 0 && iblock == 0)     // current run is empty
            return;

        result_->runs_sizes.resize(irun + 1);
        result_->runs_sizes.back() = iblock * block_type::size + offset;

        if (offset)        // if current block is partially filled
        {
            while (offset != block_type::size)
            {
                (*cur_block)[offset] = cmp.max_value();
                ++offset;
            }
            offset = 0;

            foxxll::block_manager* bm = foxxll::block_manager::get_instance();
            // allocate space for the block
            result_->runs.resize(irun + 1);
            result_->runs[irun].resize(iblock + 1);
            bm->new_blocks(
                alloc_strategy,
                make_bid_iterator(result_->runs[irun].begin() + iblock),
                make_bid_iterator(result_->runs[irun].end()),
                iblock);

            result_->runs[irun][iblock].value = (*cur_block)[0];             // init trigger
            cur_block = writer.write(cur_block, result_->runs[irun][iblock].bid);
        }
        else
        { }

        alloc_strategy = alloc_strategy_type();      // reinitialize block allocator for the next run
        iblock = 0;
        ++irun;
    }

    //! Returns the sorted runs object.
    //! \return Sorted runs object
    //! \remark Returned object is intended to be used by \c runs_merger object as input
    sorted_runs_type & result()
    {
        finish();
        writer.flush();

        return result_;
    }
};

//! Checker for the sorted runs object created by the \c runs_creator .
//! \param sruns sorted runs object
//! \param cmp comparison object used for checking the order of elements in runs
//! \return \c true if runs are sorted, \c false otherwise
template <class RunsType, class CompareType>
bool check_sorted_runs(const RunsType& sruns, CompareType cmp)
{
    constexpr bool debug = false;

    sort_helper::verify_sentinel_strict_weak_ordering(cmp);
    using block_type = typename RunsType::element_type::block_type;
    TLX_LOG << "Elements: " << sruns->elements;
    size_t nruns = sruns->runs.size();
    TLX_LOG << "Runs: " << nruns;
    size_t irun = 0;
    for (irun = 0; irun < nruns; ++irun)
    {
        const size_t nblocks = sruns->runs[irun].size();
        block_type* blocks = new block_type[nblocks];
        foxxll::request_ptr* reqs = new foxxll::request_ptr[nblocks];
        for (size_t j = 0; j < nblocks; ++j)
        {
            reqs[j] = blocks[j].read(sruns->runs[irun][j].bid);
        }
        wait_all(reqs, reqs + nblocks);
        delete[] reqs;

        for (size_t j = 0; j < nblocks; ++j)
        {
            if (cmp(blocks[j][0], sruns->runs[irun][j].value) ||
                cmp(sruns->runs[irun][j].value, blocks[j][0]))     //!=
            {
                TLX_LOG1 << "check_sorted_runs  wrong trigger in the run";
                delete[] blocks;
                return false;
            }
        }
        if (!stxxl::is_sorted(
                make_element_iterator(blocks, 0),
                make_element_iterator(blocks, sruns->runs_sizes[irun]),
                cmp))
        {
            TLX_LOG1 << "check_sorted_runs  wrong order in the run";
            delete[] blocks;
            return false;
        }

        delete[] blocks;
    }

    TLX_LOG1 << "Checking runs finished successfully";

    return true;
}

////////////////////////////////////////////////////////////////////////
//     MERGE RUNS                                                     //
////////////////////////////////////////////////////////////////////////

//! Merges sorted runs.
//!
//! \tparam RunsType type of the sorted runs, available as \c runs_creator::sorted_runs_type ,
//! \tparam CompareWithMinMax type of comparison object used for merging
//! \tparam AllocStr allocation strategy used to allocate the blocks for
//! storing intermediate results if several merge passes are required
template <class RunsType,
          class CompareWithMinMax,
          class AllocStr = foxxll::default_alloc_strategy>
class basic_runs_merger
{
    static constexpr bool debug = false;

public:
    using sorted_runs_type = RunsType;
    using value_cmp = CompareWithMinMax;
    using alloc_strategy = AllocStr;

    using sorted_runs_data_type = typename sorted_runs_type::element_type;
    using size_type = typename sorted_runs_data_type::size_type;
    using run_type = typename sorted_runs_data_type::run_type;
    using block_type = typename sorted_runs_data_type::block_type;
    using out_block_type = block_type;
    using trigger_entry_type = typename run_type::value_type;
    using prefetcher_type = foxxll::block_prefetcher<block_type, typename run_type::iterator>;
    using run_cursor_type = run_cursor2<block_type, prefetcher_type>;
    using run_cursor2_cmp_type = sort_helper::run_cursor2_cmp<block_type, prefetcher_type, value_cmp>;
    using loser_tree_type = loser_tree<run_cursor_type, run_cursor2_cmp_type>;
    using diff_type = int64_t;
    using sequence = std::pair<typename block_type::iterator, typename block_type::iterator>;
    using seqs_size_type = typename std::vector<sequence>::size_type;

public:
    //! Standard stream typedef.
    using value_type = typename sorted_runs_data_type::value_type;

private:
    //! comparator object to sort runs
    value_cmp m_cmp;

    //! memory size in bytes to use
    size_t m_memory_to_use;

    //! smart pointer to sorted_runs object
    sorted_runs_type m_sruns;

    //! items remaining in input
    size_type m_elements_remaining;

    //! memory buffer for merging from external streams
    out_block_type* m_buffer_block;

    //! pointer into current memory buffer: this is either m_buffer_block or the small_runs vector
    const value_type* m_current_ptr;

    //! pointer into current memory buffer: end after range of current values
    const value_type* m_current_end;

    //! sequence of block needed for merging
    run_type m_consume_seq;

    //! precalculated order of blocks in which they are prefetched
    size_t* m_prefetch_seq;

    //! prefetcher object
    prefetcher_type* m_prefetcher;

    //! loser tree used for native merging
    loser_tree_type* m_losers;

#if STXXL_PARALLEL_MULTIWAY_MERGE
    std::vector<sequence>* seqs;
    std::vector<block_type*>* buffers;
    diff_type num_currently_mergeable;
#endif

#if STXXL_CHECK_ORDER_IN_SORTS
    //! previous element to ensure the current output ordering
    value_type m_last_element;
#endif //STXXL_CHECK_ORDER_IN_SORTS

    ////////////////////////////////////////////////////////////////////

    void merge_recursively();

    void deallocate_prefetcher()
    {
        if (m_prefetcher)
        {
            delete m_losers;
#if STXXL_PARALLEL_MULTIWAY_MERGE
            delete seqs;
            delete buffers;
#endif
            delete m_prefetcher;
            delete[] m_prefetch_seq;
            m_prefetcher = nullptr;
        }
    }

    void fill_buffer_block()
    {
        TLX_LOG << "fill_buffer_block";
        if (do_parallel_merge())
        {
#if STXXL_PARALLEL_MULTIWAY_MERGE
// begin of STL-style merging
            diff_type rest = out_block_type::size;              // elements still to merge for this output block

            do                                                  // while rest > 0 and still elements available
            {
                if (num_currently_mergeable < rest)
                {
                    if (!m_prefetcher || m_prefetcher->empty())
                    {
                        // anything remaining is already in memory
                        num_currently_mergeable = m_elements_remaining;
                    }
                    else
                    {
                        num_currently_mergeable = sort_helper::count_elements_less_equal(
                            *seqs, m_consume_seq[m_prefetcher->pos()].value, m_cmp);
                    }
                }

                diff_type output_size = std::min(num_currently_mergeable, rest);         // at most rest elements

                TLX_LOG << "before merge " << output_size;

                potentially_parallel::multiway_merge(
                    (*seqs).begin(), (*seqs).end(),
                    m_buffer_block->end() - rest, output_size, m_cmp);
                // sequence iterators are progressed appropriately

                rest -= output_size;
                num_currently_mergeable -= output_size;

                TLX_LOG << "after merge";

                sort_helper::refill_or_remove_empty_sequences(*seqs, *buffers, *m_prefetcher);
            } while (rest > 0 && (*seqs).size() > 0);

#if STXXL_CHECK_ORDER_IN_SORTS
            if (!stxxl::is_sorted(m_buffer_block->cbegin(), m_buffer_block->cend(), cmp))
            {
                for (value_type* i = m_buffer_block->begin() + 1; i != m_buffer_block->end(); ++i)
                    if (cmp(*i, *(i - 1)))
                    {
                        TLX_LOG << "Error at position " << (i - m_buffer_block->begin());
                    }
                assert(false);
            }
#endif //STXXL_CHECK_ORDER_IN_SORTS

// end of STL-style merging
#else
            FOXXLL_THROW_UNREACHABLE();
#endif //STXXL_PARALLEL_MULTIWAY_MERGE
        }
        else
        {
// begin of native merging procedure
            m_losers->multi_merge(m_buffer_block->elem,
                                  m_buffer_block->elem +
                                  std::min<size_type>(
                                      static_cast<size_type>(out_block_type::size),
                                      m_elements_remaining));
// end of native merging procedure
        }
        TLX_LOG << "current block filled";

        m_current_ptr = m_buffer_block->elem;
        m_current_end = m_buffer_block->elem + std::min<size_type>(
            static_cast<size_type>(out_block_type::size), m_elements_remaining);

        if (m_elements_remaining <= out_block_type::size)
            deallocate_prefetcher();
    }

public:
    //! Creates a runs merger object.
    //! \param c comparison object
    //! \param memory_to_use amount of memory available for the merger in bytes
    basic_runs_merger(value_cmp c, size_t memory_to_use)
        : m_cmp(c),
          m_memory_to_use(memory_to_use),
          m_buffer_block(new out_block_type),
          m_prefetch_seq(nullptr),
          m_prefetcher(nullptr),
          m_losers(nullptr)
#if STXXL_PARALLEL_MULTIWAY_MERGE
          , seqs(nullptr),
          buffers(nullptr),
          num_currently_mergeable(0)
#endif
#if STXXL_CHECK_ORDER_IN_SORTS
          , m_last_element(m_cmp.min_value())
#endif //STXXL_CHECK_ORDER_IN_SORTS
    {
        sort_helper::verify_sentinel_strict_weak_ordering(m_cmp);
    }

    //! non-copyable: delete copy-constructor
    basic_runs_merger(const basic_runs_merger&) = delete;
    //! non-copyable: delete assignment operator
    basic_runs_merger& operator = (const basic_runs_merger&) = delete;

    //! Set memory amount to use for the merger in bytes.
    void set_memory_to_use(size_t memory_to_use)
    {
        m_memory_to_use = memory_to_use;
    }

    //! Initialize the runs merger object with a new round of sorted_runs.
    void initialize(const sorted_runs_type& sruns)
    {
        m_sruns = sruns;
        m_elements_remaining = m_sruns->elements;

        if (empty())
            return;

        if (!m_sruns->small_run.empty())
        {
            // we have a small input <= B, that is kept in the main memory
            TLX_LOG << "basic_runs_merger: small input optimization, input length: " << m_elements_remaining;
            assert(m_elements_remaining == size_type(m_sruns->small_run.size()));

            m_current_ptr = &m_sruns->small_run[0];
            m_current_end = m_current_ptr + m_sruns->small_run.size();

            return;
        }

#if STXXL_CHECK_ORDER_IN_SORTS
        assert(check_sorted_runs(m_sruns, m_cmp));
#endif //STXXL_CHECK_ORDER_IN_SORTS

        // *** test whether recursive merging is necessary

        foxxll::disk_queues::get_instance()->set_priority_op(foxxll::request_queue::WRITE);

        size_t disks_number = foxxll::config::get_instance()->disks_number();
        size_t min_prefetch_buffers = 2 * disks_number;
        size_t input_buffers =
            (m_memory_to_use > sizeof(out_block_type)
             ? m_memory_to_use - sizeof(out_block_type)
             : 0) / block_type::raw_size;
        size_t nruns = m_sruns->runs.size();

        if (input_buffers < nruns + min_prefetch_buffers)
        {
            // can not merge runs in one pass. merge recursively:
            TLX_LOG1 <<
                "The implementation of sort requires more than one merge pass, therefore for a better\n"
                "efficiency decrease block size of run storage (a parameter of the run_creator)\n"
                "or increase the amount memory dedicated to the merger.\n"
                "m=" << input_buffers << " nruns=" << nruns << " prefetch_blocks=" << min_prefetch_buffers << "\n"
                "memory_to_use=" << m_memory_to_use << " bytes  block_type::raw_size=" << size_t(block_type::raw_size) << " bytes";

            // check whether we have enough memory to merge recursively
            size_t recursive_merge_buffers = m_memory_to_use / block_type::raw_size;
            if (recursive_merge_buffers < 2 * min_prefetch_buffers + 1 + 2) {
                // recursive merge uses min_prefetch_buffers for input buffering and min_prefetch_buffers output buffering
                // as well as 1 current output block and at least 2 input blocks
                TLX_LOG1 << "There are only m=" << recursive_merge_buffers << " blocks available for recursive merging, but "
                         << min_prefetch_buffers << "+" << min_prefetch_buffers << "+1 are needed read-ahead/write-back/output, and";
                TLX_LOG1 << "the merger requires memory to store at least two input blocks internally. Aborting.";
                throw foxxll::bad_parameter(
                          "basic_runs_merger::sort(): INSUFFICIENT MEMORY provided, please increase parameter 'memory_to_use'");
            }

            merge_recursively();

            nruns = m_sruns->runs.size();
        }

        assert(nruns + min_prefetch_buffers <= input_buffers);

        // *** Allocate prefetcher and merge data structure

        deallocate_prefetcher();

        size_t prefetch_seq_size = 0;
        for (size_t i = 0; i < nruns; ++i)
        {
            prefetch_seq_size += m_sruns->runs[i].size();
        }

        m_consume_seq.resize(prefetch_seq_size);
        m_prefetch_seq = new size_t[prefetch_seq_size];

        typename run_type::iterator copy_start = m_consume_seq.begin();
        for (size_t i = 0; i < nruns; ++i)
        {
            copy_start = std::copy(m_sruns->runs[i].begin(),
                                   m_sruns->runs[i].end(),
                                   copy_start);
        }

        std::stable_sort(m_consume_seq.begin(), m_consume_seq.end(),
                         sort_helper::trigger_entry_cmp<trigger_entry_type, value_cmp>(m_cmp) _STXXL_SORT_TRIGGER_FORCE_SEQUENTIAL);

        const size_t n_prefetch_buffers = std::max(min_prefetch_buffers, input_buffers - nruns);

#if STXXL_SORT_OPTIMAL_PREFETCHING
        // heuristic
        const size_t n_opt_prefetch_buffers = min_prefetch_buffers + (3 * (n_prefetch_buffers - min_prefetch_buffers)) / 10;

        compute_prefetch_schedule(
            m_consume_seq,
            m_prefetch_seq,
            n_opt_prefetch_buffers,
            foxxll::config::get_instance()->max_device_id());
#else
        for (size_t i = 0; i < prefetch_seq_size; ++i)
            m_prefetch_seq[i] = i;
#endif //STXXL_SORT_OPTIMAL_PREFETCHING

        m_prefetcher = new prefetcher_type(
            m_consume_seq.begin(),
            m_consume_seq.end(),
            m_prefetch_seq,
            std::min(nruns + n_prefetch_buffers, prefetch_seq_size));

        if (do_parallel_merge())
        {
#if STXXL_PARALLEL_MULTIWAY_MERGE
// begin of STL-style merging
            seqs = new std::vector<sequence>(nruns);
            buffers = new std::vector<block_type*>(nruns);

            for (size_t i = 0; i < nruns; ++i)                                             //initialize sequences
            {
                (*buffers)[i] = m_prefetcher->pull_block();                                //get first block of each run
                (*seqs)[i] = std::make_pair((*buffers)[i]->begin(), (*buffers)[i]->end()); //this memory location stays the same, only the data is exchanged
            }
// end of STL-style merging
#else
            FOXXLL_THROW_UNREACHABLE();
#endif //STXXL_PARALLEL_MULTIWAY_MERGE
        }
        else
        {
// begin of native merging procedure
            m_losers = new loser_tree_type(m_prefetcher, nruns, run_cursor2_cmp_type(m_cmp));
// end of native merging procedure
        }

        fill_buffer_block();
    }

    //! Deallocate temporary structures freeing memory prior to next initialize().
    void deallocate()
    {
        deallocate_prefetcher();
        m_sruns = nullptr;         // release reference on result object
    }

public:
    //! Standard stream method.
    bool empty() const
    {
        return (m_elements_remaining == 0);
    }

    //! Standard size method.
    size_type size() const
    {
        return m_elements_remaining;
    }

    //! Standard stream method.
    const value_type& operator * () const
    {
        assert(!empty());
        return *m_current_ptr;
    }

    //! Standard stream method.
    const value_type* operator -> () const
    {
        return &(operator * ());
    }

    bool next_output_would_block() const {
      return m_current_ptr + 1 == m_current_end;
    }

    size_t output_block_size() const { return out_block_type::size; }

    //! Standard stream method.
    basic_runs_merger& operator ++ ()       // preincrement operator
    {
        assert(!empty());
        assert(m_current_ptr != m_current_end);

        --m_elements_remaining;
        ++m_current_ptr;

        if (TLX_LIKELY(m_current_ptr == m_current_end && !empty()))
        {
            fill_buffer_block();

#if STXXL_CHECK_ORDER_IN_SORTS
            assert(stxxl::is_sorted(m_buffer_block->cbegin(), m_buffer_block->cbegin() + std::min<size_type>(m_elements_remaining, m_buffer_block->size), m_cmp));
#endif //STXXL_CHECK_ORDER_IN_SORTS
        }

#if STXXL_CHECK_ORDER_IN_SORTS
        if (!empty())
        {
            assert(!m_cmp(operator * (), m_last_element));
            m_last_element = operator * ();
        }
#endif //STXXL_CHECK_ORDER_IN_SORTS

        return *this;
    }

    //! Destructor.
    //! \remark Deallocates blocks of the input sorted runs object
    virtual ~basic_runs_merger()
    {
        deallocate_prefetcher();

        delete m_buffer_block;
    }
};

template <class RunsType, class CompareType, class AllocStr>
void basic_runs_merger<RunsType, CompareType, AllocStr>::merge_recursively()
{
    foxxll::block_manager* bm = foxxll::block_manager::get_instance();
    size_t ndisks = foxxll::config::get_instance()->disks_number();
    size_t nwrite_buffers = 2 * ndisks;
    size_t memory_for_write_buffers = nwrite_buffers * sizeof(block_type);

    // memory consumption of the recursive merger (uses block_type as
    // out_block_type)
    size_t recursive_merger_memory_prefetch_buffers = 2 * ndisks * sizeof(block_type);
    size_t recursive_merger_memory_out_block = sizeof(block_type);
    size_t memory_for_buffers = memory_for_write_buffers
                                + recursive_merger_memory_prefetch_buffers
                                + recursive_merger_memory_out_block;
    // maximum arity in the recursive merger
    size_t max_arity = (m_memory_to_use > memory_for_buffers ? m_memory_to_use - memory_for_buffers : 0) / block_type::raw_size;

    size_t nruns = m_sruns->runs.size();
    const size_t merge_factor = optimal_merge_factor(nruns, max_arity);
    assert(merge_factor > 1);
    assert(merge_factor <= max_arity);

    while (nruns > max_arity)
    {
        size_t new_nruns = foxxll::div_ceil(nruns, merge_factor);
        TLX_LOG1 << "Starting new merge phase: nruns: " << nruns <<
            " opt_merge_factor: " << merge_factor <<
            " max_arity: " << max_arity << " new_nruns: " << new_nruns;

        // construct new sorted_runs data object which will be swapped into
        // m_sruns

        sorted_runs_data_type new_runs;
        new_runs.runs.resize(new_nruns);
        new_runs.runs_sizes.resize(new_nruns);
        new_runs.elements = m_sruns->elements;

        // merge all runs from m_runs into news_runs

        size_t runs_left = nruns;
        size_t cur_out_run = 0;
        size_type elements_left = m_sruns->elements;

        while (runs_left > 0)
        {
            size_t runs2merge = std::min(runs_left, merge_factor);
            TLX_LOG1 << "Merging " << runs2merge << " runs";

            if (runs2merge > 1)     // non-trivial merge
            {
                // count the number of elements in the run
                size_type elements_in_new_run = 0;
                for (size_t i = nruns - runs_left; i < (nruns - runs_left + runs2merge); ++i)
                {
                    elements_in_new_run += m_sruns->runs_sizes[i];
                }
                new_runs.runs_sizes[cur_out_run] = elements_in_new_run;

                // calculate blocks in run
                const size_t blocks_in_new_run = static_cast<size_t>(foxxll::div_ceil(
                                                                         elements_in_new_run, block_type::size));

                // allocate blocks for the new runs
                new_runs.runs[cur_out_run].resize(blocks_in_new_run);
                bm->new_blocks(alloc_strategy(), make_bid_iterator(new_runs.runs[cur_out_run].begin()), make_bid_iterator(new_runs.runs[cur_out_run].end()));

                // Construct temporary sorted_runs object as input into recursive merger.
                // This sorted_runs is copied a subset of the over-large set of runs, which
                // will be deallocated from external memory once the runs are merged.
                sorted_runs_type cur_runs(new sorted_runs_data_type);
                cur_runs->runs.resize(runs2merge);
                cur_runs->runs_sizes.resize(runs2merge);

                std::copy(m_sruns->runs.begin() + nruns - runs_left,
                          m_sruns->runs.begin() + nruns - runs_left + runs2merge,
                          cur_runs->runs.begin());
                std::copy(m_sruns->runs_sizes.begin() + nruns - runs_left,
                          m_sruns->runs_sizes.begin() + nruns - runs_left + runs2merge,
                          cur_runs->runs_sizes.begin());

                cur_runs->elements = elements_in_new_run;
                elements_left -= elements_in_new_run;

                // construct recursive merger

                basic_runs_merger<RunsType, CompareType, AllocStr>
                merger(m_cmp, m_memory_to_use - memory_for_write_buffers);
                merger.initialize(cur_runs);

                {
                    // make sure everything is being destroyed in right time
                    foxxll::buf_ostream<block_type, typename run_type::iterator> out(
                        new_runs.runs[cur_out_run].begin(),
                        nwrite_buffers);

                    size_type cnt = 0;
                    const size_type cnt_max = cur_runs->elements;

                    while (cnt != cnt_max)
                    {
                        *out = *merger;
                        if ((cnt % block_type::size) == 0)     // have to write the trigger value
                            new_runs.runs[cur_out_run][static_cast<size_t>(cnt / size_type(block_type::size))].value = *merger;

                        ++cnt, ++out, ++merger;
                    }
                    assert(merger.empty());

                    while (cnt % block_type::size)
                    {
                        *out = m_cmp.max_value();
                        ++out, ++cnt;
                    }
                }

                // deallocate merged runs by destroying cur_runs
            }
            else     // runs2merge = 1 -> no merging needed
            {
                assert(cur_out_run + 1 == new_runs.runs.size());

                elements_left -= m_sruns->runs_sizes.back();

                // copy block identifiers into new sorted_runs object
                new_runs.runs.back() = m_sruns->runs.back();
                new_runs.runs_sizes.back() = m_sruns->runs_sizes.back();
            }

            runs_left -= runs2merge;
            ++cur_out_run;
        }

        assert(elements_left == 0);

        // clear bid vector of m_sruns to skip deallocation of blocks in
        // destructor
        m_sruns->runs.clear();

        // replaces data in referenced counted object m_sruns end while (nruns
        // > max_arity)
        std::swap(nruns, new_nruns);
        m_sruns->swap(new_runs);
    }
}

//! Merges sorted runs.
//!
//! \tparam RunsType type of the sorted runs, available as \c runs_creator::sorted_runs_type ,
//! \tparam CompareType type of comparison object used for merging
//! \tparam AllocStr allocation strategy used to allocate the blocks for
//! storing intermediate results if several merge passes are required
template <class RunsType,
          class CompareType = typename RunsType::element_type::cmp_type,
          class AllocStr = foxxll::default_alloc_strategy>
class runs_merger : public basic_runs_merger<RunsType, CompareType, AllocStr>
{
protected:
    using base = basic_runs_merger<RunsType, CompareType, AllocStr>;

public:
    using sorted_runs_type = RunsType;
    using value_cmp = typename base::value_cmp;
    using cmp_type = typename base::value_cmp;
    using block_type = typename base::block_type;

public:
    //! Creates a runs merger object.
    //! \param sruns input sorted runs object
    //! \param cmp comparison object
    //! \param memory_to_use amount of memory available for the merger in bytes
    runs_merger(sorted_runs_type& sruns, value_cmp cmp,
                size_t memory_to_use)
        : base(cmp, memory_to_use)
    {
        this->initialize(sruns);
    }

    //! Creates a runs merger object without initializing a round of sorted_runs.
    //! \param cmp comparison object
    //! \param memory_to_use amount of memory available for the merger in bytes
    runs_merger(value_cmp cmp, size_t memory_to_use)
        : base(cmp, memory_to_use)
    { }
};

////////////////////////////////////////////////////////////////////////
//     SORT                                                           //
////////////////////////////////////////////////////////////////////////

//! Produces sorted stream from input stream.
//!
//! \tparam Input type of the input stream
//! \tparam CompareType type of comparison object used for sorting the runs
//! \tparam BlockSize size of blocks used to store the runs
//! \tparam AllocStr functor that defines allocation strategy for the runs
//! \remark Implemented as the composition of \c runs_creator and \c runs_merger .
template <
    class Input,
    class CompareType,
    size_t BlockSize = STXXL_DEFAULT_BLOCK_SIZE(typename Input::value_type),
    class AllocStr = foxxll::default_alloc_strategy,
    class RunsCreatorType = runs_creator<Input, CompareType, BlockSize, AllocStr>
    >
class sort
{
    using runs_creator_type = RunsCreatorType;
    using sorted_runs_type = typename runs_creator_type::sorted_runs_type;
    using runs_merger_type = runs_merger<sorted_runs_type, CompareType, AllocStr>;

    runs_creator_type creator;
    runs_merger_type merger;

public:
    //! Standard stream typedef.
    using value_type = typename Input::value_type;

    //! Creates the object.
    //! \param in input stream
    //! \param c comparator object
    //! \param memory_to_use memory amount that is allowed to used by the sorter in bytes
    sort(Input& in, CompareType c, size_t memory_to_use)
        : creator(in, c, memory_to_use),
          merger(creator.result(), c, memory_to_use)
    {
        sort_helper::verify_sentinel_strict_weak_ordering(c);
    }

    //! Creates the object.
    //! \param in input stream
    //! \param c comparator object
    //! \param m_memory_to_userc memory amount that is allowed to used by the runs creator in bytes
    //! \param m_memory_to_use memory amount that is allowed to used by the merger in bytes
    sort(Input& in, CompareType c, size_t m_memory_to_userc,
         size_t m_memory_to_use)
        : creator(in, c, m_memory_to_userc),
          merger(creator.result(), c, m_memory_to_use)
    {
        sort_helper::verify_sentinel_strict_weak_ordering(c);
    }

    //! non-copyable: delete copy-constructor
    sort(const sort&) = delete;
    //! non-copyable: delete assignment operator
    sort& operator = (const sort&) = delete;

    //! Standard stream method.
    bool empty() const
    {
        return merger.empty();
    }

    //! Standard stream method.
    const value_type& operator * () const
    {
        assert(!empty());
        return *merger;
    }

    const value_type* operator -> () const
    {
        assert(!empty());
        return merger.operator -> ();
    }

    //! Standard stream method.
    sort& operator ++ ()
    {
        ++merger;
        return *this;
    }
};

//! Computes sorted runs type from value type and block size.
//!
//! \tparam ValueType type of values ins sorted runs
//! \tparam BlockSize size of blocks where sorted runs stored
template <
    class ValueType,
    size_t BlockSize
    >
class compute_sorted_runs_type
{
    using value_type = ValueType;
    using bid_type = foxxll::BID<BlockSize>;
    using trigger_entry_type = sort_helper::trigger_entry<bid_type, value_type>;

public:
    using result = sorted_runs<trigger_entry_type, std::less<value_type> >;
};

//! \}

} // namespace stream

//! \addtogroup stlalgo
//! \{

//! Sorts range of any random access iterators externally.
//!
//! \param begin iterator pointing to the first element of the range
//! \param end iterator pointing to the last+1 element of the range
//! \param cmp comparison object
//! \param MemSize memory to use for sorting (in bytes)
//! \param AS allocation strategy
//!
//! The \c BlockSize template parameter defines the block size to use (in bytes)
//! \warning Slower than External Iterator Sort
template <
    size_t BlockSize,
    class RandomAccessIterator,
    class CmpType,
    class AllocStr
    >
void sort(RandomAccessIterator begin,
          RandomAccessIterator end,
          CmpType cmp,
          size_t MemSize,
          AllocStr AS)
{
    tlx::unused(AS);
    using InputType = typename stream::streamify_traits<RandomAccessIterator>::stream_type;
    InputType Input(begin, end);
    using sorter_type = stream::sort<InputType, CmpType, BlockSize, AllocStr>;
    sorter_type Sort(Input, cmp, MemSize);
    stream::materialize(Sort, begin);
}

//! \}

} // namespace stxxl

#endif // !STXXL_STREAM_SORT_STREAM_HEADER
