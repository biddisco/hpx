//  Copyright (c) 2015 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_RUNTIME_PARCELSET_RMA_OBJECT_HPP
#define HPX_RUNTIME_PARCELSET_RMA_OBJECT_HPP

#include <hpx/runtime.hpp>
#include <hpx/runtime/parcelset/parcelhandler.hpp>
#include <hpx/runtime/parcelset/parcelport.hpp>
#include <hpx/runtime/parcelset/rma/memory_region.hpp>
#include <hpx/runtime/parcelset/rma/memory_pool.hpp>
//
#include <memory>
#include <vector>
#include <type_traits>
#include <hpx/serialization/array.hpp>
#include <hpx/serialization/serialize.hpp>
#include <hpx/serialization/traits/is_bitwise_serializable.hpp>
#include <hpx/serialization/traits/is_rma_eligible.hpp>
#include <hpx/runtime/parcelset/rma/rma_vector.hpp>

using namespace hpx::parcelset;

namespace hpx {
namespace parcelset {
namespace rma
{

    template <typename T>
    using rma_vector = rma::rmavector<T, hpx::parcelset::rma::allocator<T>>;

    // ---------------------------------------------------------------------------
    // rma object definition
    template <typename T>
    struct rma_object : std::enable_shared_from_this<rma_object<T>>
    {
        // we do not allow arbitrary classes to be declared as RMA capable
        static_assert(
            hpx::traits::is_rma_elegible<T>::value,
            "type must be is_rma_eligible to support rma"
        );

        // clean up
        ~rma_object<T>() {
            pp_->deallocate_region(region_);
        }

        // placeholder for future work
        void put(T *other) {
            LOG_TRACE_MSG("Executing a put on rma channel");
        }

        std::shared_ptr<rma_object<T>> getptr() {
            // use "this->" to resolve dependent base lookup problem
            return this->shared_from_this();
        }

        operator T&() {
            return *obj_;
        }

        operator const T&() {
            return *obj_;
        }

        T* operator->() {
            return obj_;
        }

        const T* operator->() const {
            return obj_;
        }

        T& get() {
            return *obj_;
        }

        bool operator ==(const rma::rma_object<T> & other) const {
            return (*other.obj_)==(*obj_);
        }

        template <typename Archive>
        void serialize(Archive & ar, unsigned)
        {
            ar & (*obj_);
        }

        template <typename U>
        friend std::ostream& operator<<(std::ostream& os, rma::rma_object<U> const& c);

    public:
        // make_rma_object must have access to our private constructor
        template <typename T2, typename... Args>
        friend rma_object<T2>
            make_rma_object_impl(T2*, Args&&... args);

        // default empty constructor
        rma_object<T>() : region_(nullptr) {}

    private:
        // Create an rma object with a memory region
        rma_object<T>(T *obj, memory_region *region, parcelport *pp)
            : obj_(obj), region_(region), pp_(pp) {}

        // internal memory region managed by this class
        T             *obj_;
        memory_region *region_;
        parcelport    *pp_;
    };

    // ---------------------------------------------------------------------------
    // default rma object creator function.
    //
    // @TODO: currently we use the default parcelport to obtain an rma capable
    // interface provider, however we should extend this to allow an rma object
    // creation using a locality/vector of localities to ensure that the object
    // is capable of being delivered using a parcelport for that locality.

    template <typename T, typename... Args>
    rma_object<T> make_rma_object_impl(T*, Args&&... args)
    {
        parcelset::parcelhandler &ph =
            hpx::get_runtime().get_parcel_handler();
        auto pp = ph.get_default_parcelport();

        // get a memory region big enough to hold an object of type T
        memory_region *region = pp->allocate_region(sizeof(T));
        // construct a T in the memory held by the region
        void *address = region->get_address();
        T *obj = new (address) T(std::forward<Args>(args)...);
        // construct an rma object using the region
        return rma_object<T>(obj, region, pp.get());
    }

    // ---------------------------------------------------------------------------
    // main entry point for making rma objects
    template <typename T, typename... Args>
    rma_object<T> make_rma_object(Args&&... args)
    {
        return make_rma_object_impl(static_cast<T*>(nullptr), args...);
    }
}}}

#endif
