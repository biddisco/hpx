//  Copyright (c) 2017 Hartmut Kaiser
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// Simple test verifying basic resource partitioner
// pool and executor

#include <hpx/hpx_init.hpp>

#include <hpx/async.hpp>
#include <hpx/include/parallel_execution.hpp>
#include <hpx/include/resource_partitioner.hpp>
#include <hpx/include/threads.hpp>
#include <hpx/lcos/when_all.hpp>
#include <hpx/runtime/threads/executors/pool_executor.hpp>
#include <hpx/testing.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>
#include <iostream>

std::size_t num_pools = 0;

// dummy function we will call using async
void dummy_task(std::size_t n)
{
    // no other work can take place on this thread whilst it sleeps
    std::this_thread::sleep_for(std::chrono::milliseconds(n));
    //
    for (std::size_t i(0); i < n; ++i)
    {
    }
}

inline std::size_t st_rand() { return std::size_t(std::rand()); }

int hpx_main(int argc, char* argv[])
{
    HPX_TEST_EQ(std::size_t(0), hpx::resource::get_pool_index("default"));
    HPX_TEST_EQ(std::size_t(0), hpx::resource::get_pool_index("pool-0"));

    // print partition characteristics
    hpx::threads::get_thread_manager().print_pools(std::cout);

    auto const sched = hpx::threads::get_self_id()->get_scheduler_base();
    if (std::string("core-shared_priority_queue_scheduler") == sched->get_description()) {
        sched->add_remove_scheduler_mode(
            // add these flags
            hpx::threads::policies::scheduler_mode(
                hpx::threads::policies::enable_stealing |
                hpx::threads::policies::enable_stealing_numa |
                hpx::threads::policies::assign_work_thread_parent |
                hpx::threads::policies::steal_after_local),
            // remove these flags
            hpx::threads::policies::scheduler_mode(
                hpx::threads::policies::assign_work_round_robin |
                hpx::threads::policies::steal_high_priority_first)
        );
        sched->update_scheduler_mode(
            hpx::threads::policies::enable_stealing, false);
        sched->update_scheduler_mode(
            hpx::threads::policies::enable_stealing_numa, false);
    }

    // setup executors for different task priorities on the pools
    std::vector<hpx::threads::scheduled_executor> HP_executors;
    std::vector<hpx::threads::scheduled_executor> NP_executors;
    for (std::size_t i=0; i<num_pools; ++i) {
        std::string pool_name = "pool-"+std::to_string(i);
        HP_executors.push_back(
                hpx::threads::executors::pool_executor(pool_name,
                    hpx::threads::thread_priority_high)
                );
        NP_executors.push_back(
                hpx::threads::executors::pool_executor(pool_name,
                    hpx::threads::thread_priority_default)
                );
    }

    // randomly create tasks that run on a random pool
    // attach continuations to them that run on different
    // random pools
    const int loops = 10000;
    //
    std::cout << "1: Starting HP " << loops << std::endl;
    std::atomic<int> counter = loops;
    for (int i=0; i<loops; ++i)
    {
        // high priority
        std::size_t random_pool_1 = st_rand()%num_pools;
        std::size_t random_pool_2 = st_rand()%num_pools;
        auto &exec_1 = HP_executors[random_pool_1];
        auto &exec_2 = HP_executors[random_pool_2];
        auto f1 = hpx::async(exec_1, &dummy_task, 0);
        auto f2 = f1.then(exec_2, [=,&counter](auto &&){
            dummy_task(0);
            --counter;
        });
    }
    do { hpx::this_thread::yield(); } while (counter>0);

    std::cout << "2: Starting NP " << loops << std::endl;
    counter = loops;
    for (int i=0; i<loops; ++i)
    {
        // normal priority
        std::size_t random_pool_1 = st_rand()%num_pools;
        std::size_t random_pool_2 = st_rand()%num_pools;
        auto &exec_3 = NP_executors[random_pool_1];
        auto &exec_4 = NP_executors[random_pool_2];
        auto f3 = hpx::async(exec_3, &dummy_task, 0);
        auto f4 = f3.then(exec_4, [=,&counter](auto &&){
            dummy_task(0);
            --counter;
        });
    }
    do { hpx::this_thread::yield(); } while (counter>0);

    std::cout << "3: Starting HP->NP " << loops << std::endl;
    counter = loops;
    for (int i=0; i<loops; ++i)
    {
        // mixed priority, HP->NP
        std::size_t random_pool_1 = st_rand()%num_pools;
        std::size_t random_pool_2 = st_rand()%num_pools;
        auto &exec_5 = HP_executors[random_pool_1];
        auto &exec_6 = NP_executors[random_pool_2];
        auto f5 = hpx::async(exec_5, &dummy_task, 0);
        auto f6 = f5.then(exec_6, [=,&counter](auto &&){
            dummy_task(0);
            --counter;
        });
    }
    do { hpx::this_thread::yield(); } while (counter>0);

    std::cout << "4: Starting NP->HP " << loops << std::endl;
    counter = loops;
    for (int i=0; i<loops; ++i)
    {
        // mixed priority, NP->HP
        std::size_t random_pool_1 = st_rand()%num_pools;
        std::size_t random_pool_2 = st_rand()%num_pools;
        auto &exec_7 = NP_executors[random_pool_1];
        auto &exec_8 = HP_executors[random_pool_2];
        auto f7 = hpx::async(exec_7, &dummy_task, 0);
        auto f8 = f7.then(exec_8, [=,&counter](auto &&){
            dummy_task(0);
            --counter;
        });
    }
    do { hpx::this_thread::yield(); } while (counter>0);

    std::cout << "5: Starting suspending " << loops << std::endl;
    counter = loops;
    for (int i=0; i<loops; ++i)
    {
        // tasks that depend on each other and need to suspend
        std::size_t random_pool_1 = st_rand()%num_pools;
        std::size_t random_pool_2 = st_rand()%num_pools;
        auto &exec_7 = NP_executors[random_pool_1];
        auto &exec_8 = HP_executors[random_pool_2];
        // random delay up to 5 miliseconds
        std::size_t delay = st_rand()%5;
        auto f7 = hpx::async(exec_7, &dummy_task, delay);
        auto f8 = hpx::async(exec_8, [f7(std::move(f7)),&counter]() mutable {
            // if f7 is not ready then f8 will suspend itself on get
            f7.get();
            dummy_task(0);
            --counter;
        });

    }
    do { hpx::this_thread::yield(); } while (counter>0);

    return hpx::finalize();
}

int main(int argc, char* argv[])
{
    std::size_t max_threads = std::thread::hardware_concurrency();
    std::vector<std::string> cfg = {
        "hpx.os_threads=" + std::to_string(max_threads)
    };

    // create the resource partitioner
    hpx::resource::partitioner rp(argc, argv, std::move(cfg));

    // before adding pools - set the default pool name to "pool-0"
    rp.set_default_pool_name("pool-0");

    auto seed = std::time(NULL);
    std::srand(seed);
    std::cout << "Random seed " << seed << std::endl;

    // create N pools
    std::string pool_name;
    std::size_t threads_remaining = max_threads;
    std::size_t threads_in_pool = 0;
    // create pools randomly and add a random number of PUs to each pool
    for (const hpx::resource::numa_domain& d : rp.numa_domains())
    {
        for (const hpx::resource::core& c : d.cores())
        {
            for (const hpx::resource::pu& p : c.pus())
            {
                if (threads_in_pool==0) {
                    // pick a random number of threads less than the max
                    threads_in_pool = 1 + st_rand() %
                            ((std::max)(std::size_t(1),max_threads/2));
// { DEBUG ONLY
                    threads_in_pool = max_threads/2;
// } DEBUG ONLY

                    pool_name = "pool-"+std::to_string(num_pools);
                    rp.create_thread_pool(pool_name,
                        hpx::resource::scheduling_policy::shared_priority);
                    num_pools++;
                }
                std::cout << "Added pu " << p.id() << " to " << pool_name << "\n";
                rp.add_resource(p, pool_name);
                threads_in_pool--;
                if (threads_remaining-- == 0) {
                    std::cerr << "This should not happen!" << std::endl;
                }
            }
        }
    }

    // now run the test
    HPX_TEST_EQ(hpx::init(), 0);
    return hpx::util::report_errors();
}
