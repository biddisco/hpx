//  Copyright (c) 2015 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_PARALLEL_ALGORITHM_PREFIX_COPY_IF_JAN_2016)
#define HPX_PARALLEL_ALGORITHM_PREFIX_COPY_IF_JAN_2016
//
#include <hpx/parallel/executors.hpp>
//
#include <hpx/parallel/algorithms/sort.hpp>
#include <hpx/parallel/algorithms/prefix_scan.hpp>
#include <hpx/parallel/algorithms/inclusive_scan.hpp>
#include <hpx/parallel/util/zip_iterator.hpp>
#include <hpx/util/transform_iterator.hpp>
#include <hpx/util/tuple.hpp>
//
#ifdef EXTRA_DEBUG
# define debug_reduce_by_key(a) std::cout << a
#else
# define debug_reduce_by_key(a)
#endif

namespace hpx { namespace parallel { HPX_INLINE_NAMESPACE(v1)
{
    ///////////////////////////////////////////////////////////////////////////
    // reduce_by_key
    namespace detail
    {
        /// \cond NOINTERNAL

        /// \endcond
    }

    //-----------------------------------------------------------------------------
    /// Sorts the elements in the range [first, last) in ascending order. The
    /// order of equal elements is not guaranteed to be preserved. The function
    /// uses the given comparison function object comp (defaults to using
    /// operator<()).
    ///
    /// \note   Complexity: O(Nlog(N)), where N = std::distance(first, last)
    ///                     comparisons.
    ///
    /// A sequence is sorted with respect to a comparator \a comp and a
    /// projection \a proj if for every iterator i pointing to the sequence and
    /// every non-negative integer n such that i + n is a valid iterator
    /// pointing to an element of the sequence, and
    /// INVOKE(comp, INVOKE(proj, *(i + n)), INVOKE(proj, *i)) == false.
    ///
    /// \tparam ExPolicy    The type of the execution policy to use (deduced).
    ///                     It describes the manner in which the execution
    ///                     of the algorithm may be parallelized and the manner
    ///                     in which it applies user-provided function objects.
    /// \tparam Iter        The type of the source iterators used (deduced).
    ///                     This iterator type must meet the requirements of a
    ///                     random access iterator.
    /// \tparam Comp        The type of the function/function object to use
    ///                     (deduced).
    /// \tparam Proj        The type of an optional projection function. This
    ///                     defaults to \a util::projection_identity
    ///
    /// \param policy       The execution policy to use for the scheduling of
    ///                     the iterations.
    /// \param first        Refers to the beginning of the sequence of elements
    ///                     the algorithm will be applied to.
    /// \param last         Refers to the end of the sequence of elements the
    ///                     algorithm will be applied to.
    /// \param comp         comp is a callable object. The return value of the
    ///                     INVOKE operation applied to an object of type Comp,
    ///                     when contextually converted to bool, yields true if
    ///                     the first argument of the call is less than the
    ///                     second, and false otherwise. It is assumed that comp
    ///                     will not apply any non-constant function through the
    ///                     dereferenced iterator.
    /// \param proj         Specifies the function (or function object) which
    ///                     will be invoked for each pair of elements as a
    ///                     projection operation before the actual predicate
    ///                     \a comp is invoked.
    ///
    /// \a comp has to induce a strict weak ordering on the values.
    ///
    /// The application of function objects in parallel algorithm
    /// invoked with an execution policy object of type
    /// \a sequential_execution_policy execute in sequential order in the
    /// calling thread.
    ///
    /// The application of function objects in parallel algorithm
    /// invoked with an execution policy object of type
    /// \a parallel_execution_policy or \a parallel_task_execution_policy are
    /// permitted to execute in an unordered fashion in unspecified
    /// threads, and indeterminately sequenced within each thread.
    ///
    /// \returns  The \a sort algorithm returns a
    ///           \a hpx::future<Iter> if the execution policy is of
    ///           type
    ///           \a sequential_task_execution_policy or
    ///           \a parallel_task_execution_policy and returns \a Iter
    ///           otherwise.
    ///           It returns \a last.
    //-----------------------------------------------------------------------------

    template <typename ExPolicy, typename InIter, typename OutIter, typename F,
        typename Proj = util::projection_identity,
    HPX_CONCEPT_REQUIRES_(
        is_execution_policy<ExPolicy>::value &&
        traits::detail::is_iterator<InIter>::value &&
        traits::detail::is_iterator<OutIter>::value &&
        traits::is_projected<Proj, InIter>::value &&
        traits::is_indirect_callable<
            F, traits::projected<Proj, InIter>
        >::value)
    >
    typename util::detail::algorithm_result<
        ExPolicy, std::pair<InIter, OutIter>
    >::type
    prefix_copy_if(ExPolicy&& policy, InIter first, InIter last, OutIter dest, F && op,
        Proj && proj = Proj())
    {
        typedef typename std::iterator_traits<InIter>::iterator_category
            input_iterator_category;
        typedef typename std::iterator_traits<OutIter>::iterator_category
            output_iterator_category;

        static_assert(
            (boost::is_base_of<
                std::input_iterator_tag, input_iterator_category>::value),
            "Required at least input iterator.");

        static_assert(
            (boost::mpl::or_<
                boost::is_base_of<
                    std::forward_iterator_tag, output_iterator_category>,
                boost::is_same<
                    std::output_iterator_tag, output_iterator_category>
            >::value),
            "Requires at least output iterator.");

        typedef typename boost::mpl::or_<
            is_sequential_execution_policy<ExPolicy>,
            boost::is_same<std::input_iterator_tag, input_iterator_category>,
            boost::is_same<std::output_iterator_tag, output_iterator_category>
        >::type is_seq;

        typedef typename std::iterator_traits<InIter>::value_type value_type;

        typedef typename detail::remove_asynchronous<
                    typename std::decay< ExPolicy >::type >::type sync_policy_type;

        sync_policy_type sync_policy = sync_policy_type().on(policy.executor()).with(policy.parameters());

        typedef hpx::util::zip_iterator<InIter, bool*> zip_iterator;
        std::size_t N = std::distance(first,last);
        boost::shared_array<bool> flags(new bool[N]);
        value_type init = 0;
        //
        zip_iterator s_begin = hpx::util::make_zip_iterator(first, flags.get());
        zip_iterator s_end   = hpx::util::make_zip_iterator(last,  flags.get()+N);
        OutIter out_iter = dest;
        //
        auto result = detail::parallel_scan_struct_lambda< std::pair<InIter, OutIter> >().call(
            std::forward < ExPolicy > (policy),
            is_seq(),
            s_begin,
            s_end,
            dest,
            init,
            // stage 1 : initial pass of each section of the input
            [&op](zip_iterator first, std::size_t count, value_type init) {
                std::size_t offset = 0;
                for (/* */; count-- != 0; ++first) {
                    bool temp = op(hpx::util::get<0>(*first));
                    if ((hpx::util::get<1>(*first) = temp)) offset++;
                }
                return offset;
            },
            // stage 2 operator to use to combine intermediate results
            std::plus<std::size_t>(),
            // stage 3 lambda to apply results to each section
            [out_iter](zip_iterator first, std::size_t count, OutIter dest, std::size_t offset) mutable {
                std::advance(out_iter, offset);
                for (/* */; count-- != 0; ++first) {
                    if (hpx::util::get<1>(*first)) {
                        *out_iter++ = hpx::util::get<0>(*first);
                        std::cout << "writing " << hpx::util::get<0>(*first) << "\n";
                    }
                }
                return out_iter;
            },
            // stage 4 : generate a return value
            [last](OutIter dest) mutable ->  std::pair<InIter, OutIter> {
                //std::advance(out_iter, offset);
                return std::make_pair(last, dest);
            }
        );

        return std::make_pair<InIter, OutIter>(std::move(last), std::move(result.second));


/*


        const uint64_t numberOfKeys = std::distance(key_first, key_last);

        if (numberOfKeys <= 1)
        { // we only have a single key/value so that is our output
            *keys_output = *key_first;
            *values_output = *values_first;
            return result::get(std::make_pair(keys_output,values_output));
        }

        using namespace hpx::parallel::v1::detail;
        using namespace hpx::util;
        //we need to determine based on the keys what is the keystate for
        //each key. The states are start, middle, end of a series and the special
        //state start and end of a series
        std::vector< ReduceKeySeriesStates > keystate;
        using KeyStateIterType = std::vector< ReduceKeySeriesStates >::iterator;
        using reducebykey_iter = detail::reduce_stencil_iterator<RanIter, reduce_stencil_transformer>;
        using element_type = typename std::iterator_traits<RanIter>::reference;
        using zip_ref = typename zip_iterator<reducebykey_iter, KeyStateIterType>::reference;
        keystate.assign(numberOfKeys, ReduceKeySeriesStates());
        {
            reduce_stencil_transformer r_s_t;
            reducebykey_iter reduce_begin = make_reduce_stencil_iterator(key_first, r_s_t);
            reducebykey_iter reduce_end   = make_reduce_stencil_iterator(key_last, r_s_t);

            if (numberOfKeys==2) {
                // for two entries, one is a start, the other an end,
                // if they are different, then they are both start/end
                element_type left  = *key_first;
                element_type right = *std::next(key_first);
                keystate[0] = ReduceKeySeriesStates(true, !comp(left,right));
                keystate[1] = ReduceKeySeriesStates(!comp(left,right), true);
            }
            else {
                // do the first and last elements by hand to simplify the iterator
                // traversal as there is no prev/next for first/last
                element_type elem0 = *key_first;
                element_type elem1 = *std::next(key_first);
                keystate[0] = ReduceKeySeriesStates(true, elem0!=elem1);
                // middle elements
                ReduceStencilGeneration <reduce_stencil_transformer, RanIter, KeyStateIterType, Compare> kernel;
                hpx::parallel::for_each(
                        sync_policy,
                        make_zip_iterator(reduce_begin + 1, keystate.begin() + 1),
                        make_zip_iterator(reduce_end - 1, keystate.end() - 1),
                        [&kernel, &comp](zip_ref ref) {
                            kernel.operator()(get<0>(ref), get<1>(ref), comp);
                        }
                );
                // Last element
                element_type elemN = *std::prev(key_last);
                element_type elemn = *std::prev(std::prev(key_last));
                keystate.back() = ReduceKeySeriesStates(elemN!=elemn, true);
            }
        }
        {
            typedef zip_iterator<RanIter2, std::vector< ReduceKeySeriesStates >::iterator> zip_iterator;
            typedef std::vector< ReduceKeySeriesStates >::iterator rki;
            typedef typename zip_iterator::value_type zip_type;
            typedef typename std::iterator_traits<RanIter2>::value_type value_type;

            zip_iterator states_begin = make_zip_iterator(
                    values_first, std::begin(keystate));
            zip_iterator states_end = make_zip_iterator(
                    values_first + numberOfKeys, std::end(keystate));
            zip_iterator states_out_begin = make_zip_iterator(
                    values_output, std::begin(keystate));
            //
            zip_type initial = tuple<float, ReduceKeySeriesStates>(0.0, ReduceKeySeriesStates(true, false));
            //
            hpx::parallel::prefix_scan_inclusive(
                    sync_policy,
                    states_begin,
                    states_end,
                    states_out_begin,
                    initial,
                    // B is the current entry, A is the one passed in from 'previous'
                    [](zip_type a, zip_type b) {
                        value_type            a_val   = get<0>(a);
                        ReduceKeySeriesStates a_state = get<1>(a);
                        value_type            b_val   = get<0>(b);
                        ReduceKeySeriesStates b_state = get<1>(b);
                        debug_reduce_by_key(
                                "{ " << a_val << "+" << b_val << " },\t" << a_state << b_state);
                        // if carrying a start flag, then copy - don't add
                        if (b_state.fStart) {
                            debug_reduce_by_key(" = " << b_val << std::endl);
                            return make_tuple(
                                    b_val,
                                    ReduceKeySeriesStates(a_state.fStart || b_state.fStart, b_state.fEnd));
                        }
                        // normal add of previous + this
                        else {
                            debug_reduce_by_key(" = " << a_val + b_val << std::endl);
                            return make_tuple(
                                    a_val + b_val,
                                    ReduceKeySeriesStates(a_state.fStart || b_state.fStart, b_state.fEnd));
                        }
                    }
            );

            // now copy the values and keys for each element that
            // is marked by an 'END' state to the final output
            using zip_iterator2 = hpx::util::zip_iterator<
                    RanIter, OutIter2,
                    std::vector< ReduceKeySeriesStates >::iterator>;
            using zip2_ref = typename zip_iterator2::reference;

            // @TODO : fix this to write keys to output array instead of input
            auto return_val = make_pair_result(
                std::move(hpx::parallel::copy_if(
            hpx::parallel::seq,
                    make_zip_iterator(
                        key_first, values_output, std::begin(keystate)),
                    make_zip_iterator(
                        key_last, values_output + numberOfKeys, std::end(keystate)),
                    make_zip_iterator(
                        key_first, values_output, std::begin(keystate)),
                    // copies to dest only when 'end' state is true
                    [](zip2_ref it) {
                        return get< 2 >(it).fEnd;
                    }
                )), key_first, values_output);

            return result::get(std::move(return_val));
        }
    }
    */

    }
        }
}}

#endif
