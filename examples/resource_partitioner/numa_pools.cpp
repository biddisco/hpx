//  Copyright (c) 2019 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// This example creates a resource partitioner, a custom thread pool, and adds
// processing units from a single NUMA domain to the custom thread pool. It is
// intended for inclusion in the documentation.

//[body
#include <hpx/hpx_init.hpp>
#include <hpx/runtime/resource/partitioner.hpp>

#include <iostream>

int hpx_main(int argc, char* argv[])
{
    return hpx::finalize();
}

int main(int argc, char* argv[])
{
    hpx::resource::partitioner rp(argc, argv);

    std::vector<std::string> pools;

    // how many numa domains are there?
    // we must be careful because some machines have numa domains that have
    // no CPU resources bound, and so iterate over the topology and build a list
    // of numa nodes we can use for computation
    int index = 0;
    for (const auto &numa : rp.numa_domains()) {
        if (numa.cores().size()>0) {
            std::string pool_name = "pool" + std::to_string(index++);
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
