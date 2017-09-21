///////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2016 Thomas Heller
//  Copyright (c) 2016 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///////////////////////////////////////////////////////////////////////////////

#ifndef HPX_COMPUTE_HOST_NUMA_ALLOCATOR_HPP
#define HPX_COMPUTE_HOST_NUMA_ALLOCATOR_HPP

#include <hpx/config.hpp>

#include <hpx/compute/host/target.hpp>
#include <hpx/parallel/execution_policy.hpp>
#include <hpx/runtime/threads/topology.hpp>

#include <hpx/runtime/threads/policies/topology.hpp>
#include <hpx/runtime/threads/policies/hwloc_topology_info.hpp>

#include <boost/range/irange.hpp>

#include <cstddef>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace hpx { namespace compute { namespace host
{
    /// The numa_allocator allocates memory using a policy based on
    /// hwloc flags for memory binding.

    /// This allocator can be used to write NUMA aware algorithms:
    ///
    /// typedef hpx::compute::host::numa_allocator<int> allocator_type;
    ///
    template <typename T, threads::hpx_hwloc_membind_policy MemBind >
    struct numa_allocator
    {
        typedef T              value_type;
        typedef T*             pointer;
        typedef const T*       const_pointer;
        typedef T&             reference;
        typedef T const&       const_reference;
        typedef std::size_t    size_type;
        typedef std::ptrdiff_t difference_type;
        //
        template <typename U>
        struct rebind
        {
            typedef numa_allocator<U, MemBind> other;
        };

        typedef std::false_type is_always_equal;
        typedef std::true_type propagate_on_container_move_assignment;

        typedef std::vector<host::target> target_type;

        numa_allocator()
            : flags_(0)
        {}

        numa_allocator(threads::hwloc_bitmap_ptr bitmap, unsigned int flags)
            : bitmap_(bitmap), flags_(flags)
        {
            const threads::hwloc_topology_info *topo =
                dynamic_cast<const threads::hwloc_topology_info *>
                    (&threads::get_topology());

            char strp[256];
            hwloc_bitmap_snprintf((char*)(strp), 256, bitmap_->bmp_);
            std::cout << "Copy allocator 1 with bitmap " << strp << std::endl;
        }

        numa_allocator(numa_allocator const& alloc)
            : bitmap_(alloc.bitmap_)
            , flags_(alloc.flags_)
        {
            const threads::hwloc_topology_info *topo =
                dynamic_cast<const threads::hwloc_topology_info *>
                    (&threads::get_topology());

            char strp[256];
            hwloc_bitmap_snprintf((char*)(strp), 256, bitmap_->bmp_);
            std::cout << "Copy allocator 2 with bitmap " << strp << std::endl;
        }

        numa_allocator(numa_allocator && alloc)
          : bitmap_(std::move(alloc.bitmap_))
          , flags_(alloc.flags_)
        {
            alloc.bitmap_ = nullptr;
            char strp[256];
            hwloc_bitmap_snprintf((char*)(strp), 256, bitmap_->bmp_);
            std::cout << "Move allocator with bitmap " << strp << std::endl;
        }

        template <typename U, threads::hpx_hwloc_membind_policy M>
        numa_allocator(numa_allocator<U, M> const& alloc)
            : bitmap_(alloc.bitmap)
            , flags_(alloc.flags_)
        {
            threads::hwloc_topology_info *topo =
                dynamic_cast<threads::hwloc_topology_info *>
                    (&threads::get_topology());

            char strp[256];
            hwloc_bitmap_snprintf((char*)(strp), 256, bitmap_->bmp_);
            std::cout << "Copy allocator 3 with bitmap " << strp << std::endl;
        }

        template <typename U, threads::hpx_hwloc_membind_policy M>
        numa_allocator(numa_allocator<U, M> && alloc)
            : bitmap_(std::move(alloc.bitmap_))
            , flags_(alloc.flags_)
        {
            alloc.bitmap_ = nullptr;
            char strp[256];
            hwloc_bitmap_snprintf((char*)(strp), 256, bitmap_->bmp_);
            std::cout << "Move allocator 2 with bitmap " << strp << std::endl;
        }

        numa_allocator& operator=(numa_allocator const& rhs)
        {
            bitmap_ = rhs.bitmap_;
            flags_  = rhs.flags_;
            threads::hwloc_topology_info *topo =
                dynamic_cast<threads::hwloc_topology_info *>
                    (&threads::get_topology());

            char strp[256];
            hwloc_bitmap_snprintf((char*)(strp), 256, bitmap_->bmp_);
            std::cout << "Assignment operator with bitmap " << strp << std::endl;
            return *this;
        }
        numa_allocator& operator=(numa_allocator && rhs)
        {
            bitmap_ = std::move(rhs.bitmap_);
            flags_  = rhs.flags_;
            char strp[256];
            hwloc_bitmap_snprintf((char*)(strp), 256, bitmap_->bmp_);
            std::cout << "Move assignment with bitmap " << strp << std::endl;
            rhs.bitmap_ = nullptr;
            return *this;
        }

        // Returns the actual address of x even in presence of overloaded
        // operator&
        pointer address(reference x) const noexcept
        {
            return &x;
        }

        const_pointer address(const_reference x) const noexcept
        {
            return &x;
        }

        // Allocates n * sizeof(T) bytes of uninitialized storage by calling
        // topo.allocate_membind()
        pointer allocate(size_type n)
        {
            char strp[256];
            hwloc_bitmap_snprintf((char*)(strp), 256, bitmap_->bmp_);

            std::cout << "Calling allocate membind with bitmap " << strp
                      << " flags " << flags_
                      << std::endl;

            pointer result = reinterpret_cast<pointer>(
                threads::get_topology().
                    allocate_membind(n * sizeof(T), bitmap_, MemBind, flags_));
            std::cout << "allocate membind returned 0x" << std::hex << result << std::endl;
            return result;
        }

        // Deallocates the storage referenced by the pointer p, which must be a
        // pointer obtained by an earlier call to allocate(). The argument n
        // must be equal to the first argument of the call to allocate() that
        // originally produced p; otherwise, the behavior is undefined.
        void deallocate(pointer p, size_type n)
        {
            std::cout << "Calling deallocate membind for size " << n << std::endl;
            threads::get_topology().deallocate(p, n * sizeof(T));
        }

        // Returns the maximum theoretically possible value of n, for which the
        // call allocate(n, 0) could succeed. In most implementations, this
        // returns std::numeric_limits<size_type>::max() / sizeof(value_type).
        size_type max_size() const noexcept
        {
            return (std::numeric_limits<size_type>::max)();
        }

        // Constructs an object of type T in allocated uninitialized storage
        // pointed to by p, using placement-new
        template <class U, class ...A>
        void construct(U* const p, A&& ...args)
        {
            new (p) U(std::forward<A>(args)...);
        }

        template <class U>
        void destroy(U* const p)
        {
            p->~U();
        }

    protected:
        threads::hwloc_bitmap_ptr bitmap_;
        unsigned int              flags_;
    };

    template <typename T> struct numa_allocator_firsttouch :
        numa_allocator<T, threads::hpx_hwloc_membind_policy::HPX_MEMBIND_FIRSTTOUCH>
    {};

    template <typename T>
    struct numa_allocator_interleave :
        numa_allocator<T, threads::hpx_hwloc_membind_policy::HPX_MEMBIND_INTERLEAVE>
    {
        numa_allocator_interleave(threads::hwloc_bitmap_ptr bitmap) :
            numa_allocator<T, threads::hpx_hwloc_membind_policy::HPX_MEMBIND_INTERLEAVE>(
                bitmap, 0) {}

        // this is provided only to keep the compiler happy - do not use it
        numa_allocator_interleave() :
            numa_allocator<T, threads::hpx_hwloc_membind_policy::HPX_MEMBIND_INTERLEAVE>() {}

    };

}}}

#endif
