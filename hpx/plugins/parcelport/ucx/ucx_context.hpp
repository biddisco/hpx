//  Copyright (c) 2017 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_POLICIES_UCX_CONTEXT_HPP
#define HPX_PARCELSET_POLICIES_UCX_CONTEXT_HPP

#include <hpx/config.hpp>

#if defined(HPX_HAVE_PARCELPORT_UCX)

#include <hpx/lcos/local/spinlock.hpp>
#include <hpx/runtime/parcelset_fwd.hpp>

#define NVALGRIND
extern "C" {
#include <ucs/async/async.h>
#include <ucs/config/global_opts.h>
#include <uct/api/uct.h>
}

#include <string>
#include <mutex>

namespace hpx { namespace parcelset { namespace policies { namespace ucx
{
    class HPX_EXPORT ucx_context
    {
        bool find_ifaces(std::string const& domain);

    public:
        typedef hpx::lcos::local::spinlock mutex_type;

        ucx_context(std::string const& domain, hpx::parcelset::locality& here);
        ~ucx_context();

        std::unique_lock<mutex_type> lock()
        {
            return std::unique_lock<mutex_type>(mtx_);
        }

        std::unique_lock<mutex_type> try_lock()
        {
            return std::unique_lock<mutex_type>(mtx_, std::try_to_lock);
        }

        void progress();

        uct_md_h pd_;
        uct_md_attr_t pd_attr_;

        uct_iface_h rma_iface_;
        uct_iface_attr_t rma_iface_attr_;

        uct_iface_h am_iface_;
        uct_ep_h am_ep_;
        uct_iface_attr_t am_iface_attr_;

    private:
        mutex_type mtx_;

        ucs_async_context_t context_;
        uct_worker_h worker_;
    };
}}}}

#endif
#endif
