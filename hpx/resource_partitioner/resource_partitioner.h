//  Copyright (c)      2017 Shoshana Jakobovits
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_RESOURCE_PARTITIONER)
#define HPX_RESOURCE_PARTITIONER

#include <hpx/include/runtime.hpp>
#include <hpx/runtime/threads/topology.hpp>
#include <hpx/runtime/threads/detail/thread_pool.hpp>

#include <vector>
#include <string>

namespace hpx{

    class initial_thread_pool;

    class resource_partitioner{
    public:
        resource_partitioner() // queries hwloc, sets internal parameters
                :
                {
                        //! do not allow the creation of more than one RP instance
                        //! cf counter for runtime, do the same and add test and exception here
                }

        // create a new thread_pool, add it to the RP and return a pointer to it
        initial_thread_pool* create_thread_pool(std::string name){
            // well ... doesn't have to do that much,
            // just push_back to the internal parameter probably
            initial_thread_pool_.push_back(initial_thread_pool_(name));
        }

        // lots of get_functions
        std::size_t get_number_pools(){
            return thread_pools_.size();
        }

    private:
        // contains the basic characteristics
        std::vector<initial_thread_pool> initial_thread_pool_;

        // actual thread pools of OS-threads
        std::vector<threads::detail::thread_pool> thread_pools_; //! template param?

        // counter for instance numbers

    };

    resource_partitioner * get_resource_partitioner_ptr() const
    {
        // if resource_partitioner has been instanciated already (most cases)
        resource_partitioner** rp = runtime::runtime_.get();
        return rp ? *rp : nullptr;
    }

    // structure used to encapsulate all characteristics of thread_pools
    // as specified by the user in int main()
    struct initial_thread_pool{
    public:
        initial_thread_pool(std::string name)
                :pool_name_(name)
                {}

        // mechanism for adding resources
        void add_resource(std::size_t){
            //! ...
        }

    private:
        std::string pool_name_;

    };



}



#endif