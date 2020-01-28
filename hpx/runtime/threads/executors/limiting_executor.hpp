//  Copyright (c) 2017-2018 John Biddiscombe
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_RUNTIME_THREADS_LIMITING_EXECUTOR_HPP
#define HPX_RUNTIME_THREADS_LIMITING_EXECUTOR_HPP

#include <hpx/config.hpp>
#include <hpx/apply.hpp>
#include <hpx/execution/executors/execution_fwd.hpp>
#include <hpx/functional/invoke.hpp>
#include <hpx/functional/invoke_fused.hpp>
#include <hpx/functional/result_of.hpp>
#include <hpx/runtime/threads/executors/default_executor.hpp>
#include <hpx/runtime/threads/executors/pool_executor.hpp>
#include <hpx/runtime/threads/thread_executor.hpp>
#include <hpx/threading_base/thread_pool_base.hpp>
#include <hpx/basic_execution/this_thread.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

#include <hpx/config/warnings_prefix.hpp>

// --------------------------------------------------------------------
//
// --------------------------------------------------------------------
namespace hpx { namespace threads { namespace executors {
    template <typename Executor>
    struct limiting_executor
    {
        // --------------------------------------------------------------------
        // For C++11 compatibility
        template <bool B, typename T = void>
        using enable_if_t = typename std::enable_if<B, T>::type;

        // --------------------------------------------------------------------
        //
        // --------------------------------------------------------------------
        limiting_executor(std::size_t lower, std::size_t upper,
            bool block_on_destruction = true)
          : executor_(Executor())
          , count_(0)
          , lower_threshold_(lower)
          , upper_threshold_(upper)
          , block_(block_on_destruction)
        {
        }

        limiting_executor(const Executor& ex, std::size_t lower,
            std::size_t upper, bool block_on_destruction = true)
          : executor_(ex)
          , count_(0)
          , lower_threshold_(lower)
          , upper_threshold_(upper)
          , block_(block_on_destruction)
        {
        }

        ~limiting_executor()
        {
            if (block_)
            {
                set_and_wait(0, 0);
            }
        }

        void count_up()
        {
            if (++count_ > upper_threshold_)
            {
                hpx::util::yield_while(
                    [&]() { return (count_ > lower_threshold_); }, nullptr,
                    false);
            }
        }

        void count_down()
        {
            --count_;
        }

        // --------------------------------------------------------------------
        // post : for general apply()
        // --------------------------------------------------------------------
        template <typename F, typename... Ts>
        void post(F&& f, Ts&&... ts)
        {
            count_up();
            auto&& args = hpx::util::make_tuple(std::forward<Ts>(ts)...);
            parallel::execution::post(executor_,
                [this, f = std::forward<F>(f),
                    args = std::forward<decltype(args)>(args)]() mutable {
                    hpx::util::invoke_fused(std::move(f), std::move(args));
                    count_down();
                });
        }

        struct on_exit
        {
            on_exit(limiting_executor* this_e)
              : exec_(this_e)
            {
            }
            ~on_exit()
            {
                exec_->count_down();
            }
            limiting_executor* exec_;
        };

        // --------------------------------------------------------------------
        // async execute specialized for simple arguments typical
        // of a normal async call with arbitrary arguments
        // --------------------------------------------------------------------
        template <typename F, typename... Ts>
        future<typename util::detail::invoke_deferred_result<F, Ts...>::type>
        async_execute(F&& f, Ts&&... ts)

        {
            using result_type =
                typename util::detail::invoke_deferred_result<F, Ts...>::type;

            count_up();
            auto&& args = hpx::util::make_tuple(std::forward<Ts>(ts)...);
            lcos::local::futures_factory<result_type()> p(executor_,
                [this, f = std::forward<F>(f),
                    args = std::forward<decltype(args)>(args)]() mutable {
                    on_exit decrementer(this);
                    return /*util::void_guard<result_type>(), */
                        util::invoke_fused(std::move(f), std::move(args));
                });

            p.apply("limiting exec", launch::async,
                threads::thread_priority_default,
                threads::thread_stacksize_default);
            //                executor_.get_priority(),
            //                executor_.get_stacksize());

            return p.get_future();
        }

        // --------------------------------------------------------------------
        // .then() execute specialized for a future<P> predecessor argument
        // note that future<> and shared_future<> are both supported
        // --------------------------------------------------------------------
        template <typename F, typename Future, typename... Ts,
            typename = enable_if_t<traits::is_future<
                typename std::remove_reference<Future>::type>::value>>
        future<typename util::detail::invoke_deferred_result<F, Future,
            Ts...>::type>
        then_execute(F&& f, Ts&&... ts)
        {
            using result_type =
                typename util::detail::invoke_deferred_result<F, Ts...>::type;

            count_up();
            auto&& args = hpx::util::make_tuple(std::forward<Ts>(ts)...);
            lcos::local::futures_factory<result_type()> p(executor_,
                [this, f = std::forward<F>(f),
                    args = std::forward<decltype(args)>(args)]() mutable {
                    on_exit decrementer(this);
                    return util::void_guard<result_type>(),
                           util::invoke_fused(std::move(f), std::move(args));
                });

            p.apply(launch::async, threads::thread_priority_default,
                threads::thread_stacksize_default);

            return p.get_future();
        }

        void set_and_wait(std::size_t lower, std::size_t upper)
        {
            set_threshold(lower, upper);
            wait();
        }

        void set_threshold(std::size_t lower, std::size_t upper)
        {
            lower_threshold_ = lower;
            upper_threshold_ = upper;
        }

        // wait (suspend) until the number of tasks 'in flight' on this executor
        // drops to the lower threashold
        void wait()
        {
            hpx::util::yield_while(
                [&]() { return (count_ > lower_threshold_); });
        }

        // wait (suspend) until all tasks launched on this executor have completed
        void wait_all()
        {
            hpx::util::yield_while([&]() { return (count_ > 0); });
        }

    private:
        // --------------------------------------------------------------------
        Executor executor_;
        std::atomic<std::int64_t> count_;
        std::int64_t lower_threshold_;
        std::int64_t upper_threshold_;
        bool block_;
    };

}}}    // namespace hpx::threads::executors

namespace hpx { namespace parallel { namespace execution {
    template <typename Executor>
    struct executor_execution_category<
        threads::executors::limiting_executor<Executor>>
    {
        typedef parallel::execution::parallel_execution_tag type;
    };

    template <typename Executor>
    struct is_two_way_executor<threads::executors::limiting_executor<Executor>>
      : std::true_type
    {
    };
}}}    // namespace hpx::parallel::execution

#include <hpx/config/warnings_suffix.hpp>

#endif /*HPX_RUNTIME_THREADS_LIMITING_EXECUTOR_HPP*/
