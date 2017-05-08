//  Copyright (c)      2017 Shoshana Jakobovits
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_RESOURCE_PARTITIONER)
#define HPX_RESOURCE_PARTITIONER

#include <hpx/include/runtime.hpp>
#include <hpx/runtime/threads/topology.hpp>

#include <vector>

namespace hpx{

    class resource_partitioner{
    public:

    private:

    };

    resource_partitioner * get_resource_partitioner_ptr()
    {
        resource_partitioner** rp = runtime::runtime_.get();
        return rt ? *rt : nullptr;
    }

}



#endif