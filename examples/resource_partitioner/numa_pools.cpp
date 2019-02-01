//  Copyright (c) 2019 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx.hpp>
#include <hpx/async.hpp>
#include <hpx/hpx_init.hpp>
#include <hpx/parallel/executors.hpp>
#include <hpx/parallel/execution.hpp>
#include <hpx/runtime/resource/partitioner.hpp>
#include <hpx/runtime/threads/topology.hpp>
#include <hpx/runtime/threads/executors.hpp>
#include <hpx/runtime/threads/executors/current_executor.hpp>
#include <hpx/runtime/threads/executors/pool_executor.hpp>
#include <hpx/parallel/util/numa_binding_allocator.hpp>
//
#include <tests/unit/topology/allocator_binder_executor.hpp>
//
#include <iostream>

void test(const std::string pool)
{
    using namespace hpx::threads::executors;
    hpx::threads::scheduled_executor exec = current_executor();
}

int hpx_main(int argc, char* argv[])
{
    // get a reference to the singleton resource partitioner
    auto &rp = hpx::resource::get_partitioner();
    int index = 0;
    for (const auto &numa : rp.numa_domains()) {
        // For each numa domain, create an executor so that we can
        // spawn tasks on that domain only.
        std::string pool_name = "pool_" + std::to_string(index++);
        hpx::threads::executors::pool_executor pool_exec(pool_name);

        // Create an allocator binder object from the executor
        std::shared_ptr<executor_numa_binder<double>> binder =
                std::make_shared<executor_numa_binder<double>>(pool_exec);

        // create an allocator using our binder. Any memory allocated
        // from this will be bound to the same numa domain as the executor
        hpx::compute::host::numa_binding_allocator<double> exec_allocator =
            hpx::compute::host::numa_binding_allocator<double>(binder,
                hpx::threads::hpx_hwloc_membind_policy::membind_user, 0);

        // launch a task on this executor
        hpx::async(pool_exec, &test, pool_name);
    }
    return hpx::finalize();
}

int main(int argc, char* argv[])
{
    // create resource partitioner at startup
    hpx::resource::partitioner rp(argc, argv);

    // the default pool will be on the first numa_node
    rp.set_default_pool_name("pool_" + std::to_string(0));
    std::vector<std::string> pools;

    // how many numa domains are there?
    // we must be careful because some machines have numa domains that have
    // no CPU resources bound, and so iterate over the topology and build a list
    // of numa nodes we can use for computation
    int index = 0;
    for (const auto &numa : rp.numa_domains()) {
        if (numa.cores().size()>0) {
            std::string pool_name = "pool_" + std::to_string(index++);

            // create a thread pool for this numa node
            rp.create_thread_pool(pool_name);
            // add all cores/pus on this numa node to the pool
            rp.add_resource(numa, pool_name);
            // store the pool in our list for future reference
            pools.push_back(pool_name);
        }
    }

    hpx::init();
}
//body]
