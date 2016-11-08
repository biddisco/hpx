//  Copyright (c) 2015 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_UNKNOWN_RDMA_OBJECT_HPP
#define HPX_UNKNOWN_RDMA_OBJECT_HPP

#include <plugins/parcelport/verbs/rdma/memory_region.hpp>
#include <plugins/parcelport/verbs/rdma/rdma_chunk_pool.hpp>
#include <plugins/parcelport/verbs/rdma/rdma_memory_pool.hpp>
//
#include <memory>

namespace hpx {
namespace rdma {

    typedef hpx::parcelset::policies::verbs::rdma_memory_pool memory_pool_type;
    typedef std::shared_ptr<memory_pool_type>                 memory_pool_ptr_type;
    //
    namespace detail {    
        static memory_pool_ptr_type chunk_pool_;
        //
        static void set_memory_pool(memory_pool_ptr_type pool) {
            chunk_pool_ = pool;    
        }
        static memory_pool_ptr_type get_memory_pool() {
            return chunk_pool_;    
        }
    }           

    template <typename T>
    struct rdma_object : std::shared_ptr<T> {
        
        static_assert(std::is_trivially_copyable<typename hpx::util::decay<T>::type>::value,
            "type must be trivially copyable to support RDMA");
            
        void put(T *other) {
            LOG_DEVEL_MSG("Executing a put on rdma channel");
        }
            
    };
    
    template<typename T, typename... Args>
    rdma_object<T> make_rdma_object(Args&&... args) {
        return std::allocate_shared<T>(
            detail::get_memory_pool(), std::forward<Args>(args)...);
    }       
           
}
}

#endif