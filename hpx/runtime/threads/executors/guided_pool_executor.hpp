//  Copyright (c)      2017 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_RUNTIME_THREADS_GUIDED_POOL_EXECUTOR
#define HPX_RUNTIME_THREADS_GUIDED_POOL_EXECUTOR

#include <hpx/async.hpp>
#include <hpx/runtime/threads/executors/pool_executor.hpp>
#include <hpx/runtime/threads/detail/thread_pool_base.hpp>
#include <hpx/util/thread_description.hpp>
#include <hpx/util/thread_specific_ptr.hpp>
#include <hpx/lcos/dataflow.hpp>
#include <hpx/util/invoke.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <iostream>

#include <hpx/config/warnings_prefix.hpp>

/*
hkaiser	jbjnr: if an executor implements then_execute, it will be invoked here:
https://github.com/STEllAR-GROUP/hpx/blob/master/hpx/parallel/executors/execution.hpp#L458
if it doesn't implement it, th efunctionality is emulated here
(by creating a wrapper which is invoked by post?async_execute:
https://github.com/STEllAR-GROUP/hpx/blob/master/hpx/parallel/executors/execution.hpp#L434-L445

then_execute should receive your original arguments,
but it also has to handle the actual 'then' aspect,
i.e. delay things until the 'predessessor' future has become ready
otoh, this shouldn't be hard as you have to call dataflow anyways
*/
//
#include <typeinfo>

#ifdef __GNUG__
# include <cstdlib>
# include <cxxabi.h>
#endif

// ------------------------------------------------------------------
// helper to demangle type names
// ------------------------------------------------------------------
#ifdef __GNUG__
std::string demangle(const char* name)
{
    // some arbitrary value to eliminate the compiler warning
    int status = -4;
    std::unique_ptr<char, void(*)(void*)> res {
        abi::__cxa_demangle(name, NULL, NULL, &status),
                std::free
    };
    return (status==0) ? res.get() : name ;
}
#else
// does nothing if not g++
std::string demangle(const char* name) {
    return name;
}
#endif

// --------------------------------------------------------------------
// print type information
// --------------------------------------------------------------------
inline std::string print_type() { return ""; }

template <class T>
inline std::string print_type()
{
    return demangle(typeid(T).name());
}

template<typename T, typename... Args>
inline std::string print_type(T&& head, Args&&... tail)
{
    std::string temp = print_type<T>();
    std::cout << "\t" << temp << std::endl;
    return print_type(std::forward<Args>(tail)...);
}

// --------------------------------------------------------------------
// pool_numa_hint
// --------------------------------------------------------------------
namespace hpx { namespace threads { namespace executors
{
    struct bitmap_storage
    {
        struct tls_tag {};
        static hpx::util::thread_specific_ptr<hwloc_bitmap_ptr, tls_tag> bitmap_storage_;
    };

    // --------------------------------------------------------------------
    // Template type for a numa domain scheduling hint
    template <typename... Args>
    struct HPX_EXPORT pool_numa_hint {};

    // Template type for a core scheduling hint
    template <typename... Args>
    struct HPX_EXPORT pool_core_hint {};

    // --------------------------------------------------------------------
    // helper : numa domain scheduling and then execution
    template <typename Executor, typename NumaFunction>
    struct pre_execution_async_domain_schedule
    {
        Executor     executor_;
        NumaFunction numa_function_;
        //
        template <typename F, typename ... Ts>
        auto operator()(F && f, Ts &&... ts) const
        {
            int domain = numa_function_(ts...);
            std::cout << "The numa domain is " << domain << "\n";

            std::cout << "Function : \n";
            print_type(f);
            std::cout << "Arguments : \n";
            print_type(ts...);

            // now we must forward the task on to the correct dispatch function
            typedef typename util::detail::invoke_deferred_result<F, Ts...>::type
                result_type;
            std::cout << "Result type : \n";
//            print_type<result_type>();

            lcos::local::futures_factory<result_type()> p(
                const_cast<Executor&>(executor_),
                util::deferred_call(std::forward<F>(f), std::forward<Ts>(ts)...));

            p.apply(
                launch::async,
                threads::thread_priority_default,
                threads::thread_stacksize_default,
                threads::thread_schedule_hint(domain));

            return p.get_future();
        }
    };

    // --------------------------------------------------------------------
    // helper : numa domain scheduling and then execution
    template <typename Executor, typename NumaFunction>
    struct pre_execution_then_domain_schedule
    {
        Executor     executor_;
        NumaFunction numa_function_;
        //
        template <typename F, typename P, typename ... Ts>
        auto operator()(F && f, P && predecessor, Ts &&... ts) const
        {
            int domain = 2 ; // numa_function_(predecessor, ts...);
            std::cout << "The numa domain is " << domain << "\n";

            std::cout << "Function : \n";
            print_type(f);
            std::cout << "Predecessor : \n";
            print_type(predecessor);
            std::cout << "Arguments : \n";
            print_type(ts...);

            // now we must forward the task on to the correct dispatch function
            typedef typename util::detail::invoke_deferred_result<F, Ts...>::type
                result_type;
            std::cout << "Result type : \n";
            print_type<result_type>(result_type());

            lcos::local::futures_factory<result_type()> p(
                const_cast<Executor&>(executor_),
                util::deferred_call(std::forward<F>(f), std::forward<P>(predecessor), std::forward<Ts>(ts)...));

            p.apply(
                launch::async,
                threads::thread_priority_default,
                threads::thread_stacksize_default,
                threads::thread_schedule_hint(domain));

            return p.get_future();
        }
    };

    // --------------------------------------------------------------------
    struct HPX_EXPORT guided_pool_executor_base {
    public:
        guided_pool_executor_base(const std::string& pool_name)
            : pool_executor_(pool_name)
        {}

        guided_pool_executor_base(const std::string& pool_name,
                             thread_stacksize stacksize)
            : pool_executor_(pool_name, stacksize)
        {}

        guided_pool_executor_base(const std::string& pool_name,
                             thread_priority priority,
                             thread_stacksize stacksize = thread_stacksize_default)
            : pool_executor_(pool_name, priority, stacksize)
        {}

    protected:
        pool_executor pool_executor_;
    };


    // --------------------------------------------------------------------
    template <typename... Args>
    struct HPX_EXPORT guided_pool_executor {};

    // --------------------------------------------------------------------
    // this is a guided pool executor templated over a function type
    // the function type should be the one used for async calls
    template <typename R, typename...Args>
    struct HPX_EXPORT guided_pool_executor<pool_numa_hint<R(*)(Args...)>>
        : guided_pool_executor_base
    {
    public:
        typedef guided_pool_executor<pool_numa_hint<R(*)(Args...)>> executor_type;
        using guided_pool_executor_base::guided_pool_executor_base;

        template <typename F, typename ... Ts>
        hpx::future<
            typename hpx::util::detail::invoke_deferred_result<F, Ts...>::type>
        async_execute(F && f, Ts &&... ts)
        {
            std::cout << "async_execute pool_numa_hint<R(*)(Args...)> : Function : \n";
            print_type(f);
            std::cout << "async_execute pool_numa_hint<R(*)(Args...)> : Arguments : \n";
            print_type(ts...);

std::cout << "async_execute pool_numa_hint<R(*)(Args...)> : Result : \n";
typedef hpx::future<typename hpx::util::detail::invoke_deferred_result<F, Ts...>::type> res_type;
print_type<res_type>();
std::cout << "async_execute pool_numa_hint<R(*)(Args...)> : R : \n";
print_type<R>();


            // hold onto the function until all futures have become ready
            // by using a dataflow operation, then call the scheduling hint
            // before passing the task onwards to the real executor
            return hpx::dataflow(
                util::unwrapping(
                    pre_execution_async_domain_schedule<pool_executor,
                        pool_numa_hint<R(*)(Args...)>>{
                            pool_executor_, hint_
                        }
                ),
                std::forward<F>(f), std::forward<Ts>(ts)...);
        };

    private:
        pool_numa_hint<R(*)(Args...)> hint_;
    };

    // --------------------------------------------------------------------
    // this is a guided pool executor templated over args only
    // the args should be the same as those that would be called
    // for an async function or continuation. This makes it possible to
    // guide a lambda rather than a full function.
    template <typename...Args>
    struct HPX_EXPORT guided_pool_executor<pool_numa_hint<Args...>>
        : guided_pool_executor_base
    {
    public:
        typedef guided_pool_executor<pool_numa_hint<Args...>> executor_type;
        using guided_pool_executor_base::guided_pool_executor_base;

        template <typename F, typename ... Ts>
        hpx::future<
            typename hpx::util::detail::invoke_deferred_result<F, Ts...>::type>
        async_execute(F && f, Ts &&... ts) const
        {
            std::cout << "async_execute pool_numa_hint<Args...> : Function : \n";
            print_type(f);
            std::cout << "async_execute pool_numa_hint<Args...> : Arguments : \n";
            print_type(ts...);

            // hold onto the function until all futures have become ready
            // by using a dataflow operation, then call the scheduling hint
            // before passing the task onwards to the real executor
            return hpx::dataflow(
                util::unwrapping(
                    pre_execution_async_domain_schedule<pool_executor,
                        pool_numa_hint<Args...>> {
                            pool_executor_, hint_
                        }
                ),
                std::forward<F>(f), std::forward<Ts>(ts)...);
        };

        ///////////////////////////////////////////////////////////////////////
        template <typename F, template <typename> typename Future, typename X, typename ... Ts>
        auto
        then_execute(F && f, Future<X> & predecessor, Ts &&... ts)
        ->  hpx::future<typename hpx::util::detail::invoke_deferred_result<
            F, Future<X>, Ts...>::type>
        {
            typedef typename hpx::util::detail::invoke_deferred_result<
                    F, Future<X>, Ts...>::type result_type;

            std::cout << "then_execute pool_numa_hint<Args...> : Function : \n";
            print_type(f);
            std::cout << "then_execute pool_numa_hint<Args...> : Predecessor : \n";
            print_type(predecessor);
            std::cout << "then_execute pool_numa_hint<Args...> : Future type : \n";
            print_type(X());
            std::cout << "then_execute pool_numa_hint<Args...> : Arguments : \n";
            print_type(ts...);
            std::cout << "then_execute pool_numa_hint<Args...> : result_type : \n";
            print_type(result_type());

            // swap the order of these forwarded parameters because dataflow is
            // overloaded for a function type first and if std::forward<F>(f) is
            // the first arg, then compilation breaks.
            hpx::dataflow(
                [&f](Future<X> && predecessor, Ts &&... ts) -> X {
                    X pred = predecessor.get();
                    std::cout << "then_execute dataflow : Received a value "
                              << pred << std::endl;
                    return pred;

////                    pre_execution_then_domain_schedule<pool_executor,
////                        pool_numa_hint<Args...>> {
////                            pool_executor_, hint_
////                        }
                },
                std::move(predecessor), std::forward<Ts>(ts)...);

            return hpx::make_ready_future(result_type());
        }
/*
        ///////////////////////////////////////////////////////////////////////
        // then_execute dispatch point
        template <typename Executor, typename F, typename Future, typename ... Ts>
        auto
        then_execute(Executor && exec, F && f, Future& predecessor, Ts &&... ts)
        ->  hpx::future<int>
        {

std::cout << "then_execute pool_numa_hint<Args...> : Function : \n";
print_type(f);
std::cout << "then_execute pool_numa_hint<Args...> : Predecessor : \n";
print_type(predecessor);
std::cout << "then_execute pool_numa_hint<Args...> : Arguments : \n";
print_type(ts...);


            return hpx::make_ready_future<int>(0);
        }
*/
//        ///////////////////////////////////////////////////////////////////////
//        // then_execute dispatch point
//        template <typename Executor, typename F, typename Future, typename ... Ts>
//        auto
//        then_execute(Executor && exec, F && f, Future& predecessor, Ts &&... ts)
//        ->  decltype(hpx::parallel::execution::detail::then_execute_fn_helper<
//                  typename std::decay<Executor>::type
//              >::call(std::forward<Executor>(exec), std::forward<F>(f),
//                  predecessor, std::forward<Ts>(ts)...
//          ))
//        {
///*
//std::cout << "then_execute pool_numa_hint<Args...> : Function : \n";
//print_type(f);
//std::cout << "then_execute pool_numa_hint<Args...> : Predecessor : \n";
//print_type(predecessor);
//std::cout << "then_execute pool_numa_hint<Args...> : Arguments : \n";
//print_type(ts...);
//*/

//            return hpx::make_ready_future<int>(0);
//        }

    private:
        pool_numa_hint<Args...> hint_;
    };

}}}

namespace hpx { namespace parallel { namespace execution
{
    template <typename Executor>
    struct executor_execution_category<
        threads::executors::guided_pool_executor<Executor> >
    {
        typedef parallel::execution::parallel_execution_tag type;
    };

    template <typename Executor>
    struct is_one_way_executor<
            threads::executors::guided_pool_executor<Executor> >
      : std::true_type
    {};

    template <typename Executor>
    struct is_two_way_executor<
            threads::executors::guided_pool_executor<Executor> >
      : std::true_type
    {};

    template <typename Executor>
    struct is_bulk_one_way_executor<
            threads::executors::guided_pool_executor<Executor> >
      : std::false_type
    {};

    template <typename Executor>
    struct is_bulk_two_way_executor<
            threads::executors::guided_pool_executor<Executor> >
      : std::false_type
    {};

}}}

#include <hpx/config/warnings_suffix.hpp>

#endif /*HPX_RUNTIME_THREADS_GUIDED_POOL_EXECUTOR*/
