#ifndef ALLOCATOR_BINDER_EXECUTOR_HPP
#define ALLOCATOR_BINDER_EXECUTOR_HPP

#include <hpx/parallel/util/numa_binding_allocator.hpp>
#include <hpx/runtime/threads/executors/pool_executor.hpp>
//
#include <cstddef>
#include <sstream>
#include <string>

// ------------------------------------------------------------------------
// Example of an allocator binder for a simple executor
// This binds pages according to whatever numa node the executor
// is bound to.
// ------------------------------------------------------------------------
template <typename T>
struct executor_numa_binder : hpx::compute::host::numa_binding_helper<T>
{
    executor_numa_binder(const hpx::threads::executors::pool_executor &exec) :
        hpx::compute::host::numa_binding_helper<T>()
    {
        numa_ = exec.get_numa_domain();
    }

    // return the domain that a given page should be bound to
    virtual std::size_t operator ()(
            const T * const base_ptr,
            const T * const page_ptr,
            const std::size_t pagesize,
            const std::size_t domains) const override
    {
        return numa_;
    }

    // This is for debug purposes
    virtual std::string description() const override
    {
        std::ostringstream temp;
        temp << "Executor "         << std::dec
             << " Numa "            << numa_;
        return temp.str();
    }

    //
    std::size_t numa_;
};

#endif // ALLOCATOR_BINDER_LINEAR_HPP
