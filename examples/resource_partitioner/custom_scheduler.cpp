//  Copyright (c) 2017 John Biddiscombe
//  Copyright (c) 2017 Shoshana Jakobovits
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#define SHARED_PRIORITY_SCHEDULER_DEBUG

#include <hpx/hpx.hpp>
#include <hpx/hpx_init.hpp>
//
#include <hpx/parallel/algorithms/for_loop.hpp>
#include <hpx/parallel/execution.hpp>
//
#include <hpx/runtime/resource/partitioner.hpp>
#include <hpx/runtime/threads/cpu_mask.hpp>
#include <hpx/runtime/threads/detail/scheduled_thread_pool_impl.hpp>
#include <hpx/runtime/threads/executors/pool_executor.hpp>
//
#include <hpx/include/iostreams.hpp>
#include <hpx/include/runtime.hpp>
//
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>
//
#include "shared_priority_queue_scheduler.hpp"

// ------------------------------------------------------------------------
static bool use_scheduler = false;
static std::size_t threads_per_queue_h = 1;
static std::size_t threads_per_queue_n = 1;
static std::size_t threads_per_queue_l = 1;

// ------------------------------------------------------------------------
// this is our custom scheduler type
using custom_sched =
    hpx::threads::policies::example::shared_priority_queue_scheduler<>;
using hpx::threads::policies::scheduler_mode;

// ------------------------------------------------------------------------
// dummy function we will call using async
void do_stuff(std::size_t n, bool printout)
{
    if (printout)
        hpx::cout << "[do stuff] " << n << "\n";
    for (std::size_t i(0); i < n; ++i)
    {
        double f = std::sin(2 * M_PI * i / n);
        if (printout)
            hpx::cout << "sin(" << i << ") = " << f << ", ";
    }
    if (printout)
        hpx::cout << "\n";
}

// ------------------------------------------------------------------------
// this is called on an hpx thread after the runtime starts up
int hpx_main(boost::program_options::variables_map& vm)
{
    if (vm.count("use-scheduler"))
        use_scheduler = true;
    //
    std::cout << "[hpx_main] starting ..."
              << "use_scheduler " << use_scheduler << "\n";

    std::size_t num_threads = hpx::get_num_worker_threads();
    hpx::cout << "HPX using threads = " << num_threads << std::endl;

    // create an executor with high priority for important tasks
    hpx::threads::executors::default_executor high_priority_executor(
        hpx::threads::thread_priority_critical);
    hpx::threads::executors::default_executor normal_priority_executor;


    // print partition characteristics
    std::cout << "\n\n[hpx_main] print resource_partitioner characteristics : "
              << "\n";
    hpx::resource::get_partitioner().print_init_pool_data(std::cout);

    // print partition characteristics
    std::cout << "\n\n[hpx_main] print thread-manager pools : "
              << "\n";
    hpx::threads::get_thread_manager().print_pools(std::cout);

    return hpx::finalize();
}

// ------------------------------------------------------------------------
// the normal int main function that is called at startup and runs on an OS thread
// the user must call hpx::init to start the hpx runtime which will execute hpx_main
// on an hpx thread
int main(int argc, char* argv[])
{
    boost::program_options::options_description desc_cmdline("Test options");
    desc_cmdline.add_options()
        ( "use-scheduler,s", "Enable custom priority scheduler")
        ( "hp_tpq,h",
          boost::program_options::value<std::size_t>()->default_value(1),
          "Number of threads per task queue HP")
        ( "np_tpq,n",
          boost::program_options::value<std::size_t>()->default_value(1),
          "Number of threads per task queue NP")
        ( "lp_tpq,l",
          boost::program_options::value<std::size_t>()->default_value(64),
          "Number of threads per task queue LP")
    ;

    // HPX uses a boost program options variable map, but we need it before
    // hpx-main, so we will create another one here and throw it away after use
    boost::program_options::variables_map vm;
    boost::program_options::store(
        boost::program_options::command_line_parser(argc, argv)
            .allow_unregistered()
            .options(desc_cmdline)
            .run(),
        vm);

    if (vm.count("use-scheduler"))
    {
        use_scheduler = true;
    }

    threads_per_queue_h = vm["hp_tpq"].as<std::size_t>();
    threads_per_queue_n = vm["np_tpq"].as<std::size_t>();
    threads_per_queue_l = vm["lp_tpq"].as<std::size_t>();

    // Create the resource partitioner
    hpx::resource::partitioner rp(desc_cmdline, argc, argv);

    //    auto &topo = rp.get_topology();
    std::cout << "[main] obtained reference to the resource_partitioner\n";

    // create a thread pool and supply a lambda that returns a new pool with
    // the a user supplied scheduler attached
    rp.create_thread_pool("default",
        [](hpx::threads::policies::callback_notifier& notifier,
            std::size_t num_threads, std::size_t thread_offset,
            std::size_t pool_index, std::string const& pool_name)
        -> std::unique_ptr<hpx::threads::thread_pool_base>
        {
            std::cout << "User defined scheduler creation callback " << std::endl;
            std::unique_ptr<custom_sched> scheduler(new custom_sched(
                num_threads,
                // HP, NP, LP : cores per queue
                {threads_per_queue_h, threads_per_queue_n, threads_per_queue_l},
#if SHARED_PRIORITY_QUEUE_SCHEDULER_API==2
                true,       // NUMA stealing
                true,       // Core Stealing
                custom_sched::work_assignment_policy::assign_work_round_robin,
#endif
                "shared-priority-scheduler"));

            auto mode = scheduler_mode(scheduler_mode::do_background_work |
                scheduler_mode::delay_exit);

            std::unique_ptr<hpx::threads::thread_pool_base> pool(
                new hpx::threads::detail::scheduled_thread_pool<
                        custom_sched
                    >(std::move(scheduler), notifier,
                        pool_index, pool_name, mode, thread_offset));
            return pool;
        });


    return hpx::init();
}
