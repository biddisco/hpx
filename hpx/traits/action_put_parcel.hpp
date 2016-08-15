//  Copyright (c) 2016 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_TRAITS_ACTION_PUT_PARCEL_AUG_2016)
#define HPX_TRAITS_ACTION_PUT_PARCEL_AUG_2016

#include <hpx/config.hpp>
#include <hpx/runtime/threads/thread_enums.hpp>
#include <hpx/runtime/naming/id_type.hpp>
#include <hpx/runtime/naming/address.hpp>

namespace hpx { namespace traits
{
    ///////////////////////////////////////////////////////////////////////////
    // Customization point for action put parcel
    template <typename Action, typename Enable = void>
    struct action_put_parcel
    {
        template <typename ...Ts>
        static bool call(naming::id_type const& id, naming::address&& addr,
            threads::thread_priority priority, Ts&&... vs)
        {
            // placeholder for action specialization
            std::terminate();
            return false;
        }

        template <typename Continuation, typename ...Ts>
        static bool call_cont(naming::id_type const& id, naming::address&& addr,
            threads::thread_priority priority, Continuation && cont, Ts&&... vs)
        {
            // placeholder for action specialization
            std::terminate();
            return false;
        }

        template <typename ...Ts>
        static bool call_cb(naming::id_type const& id, naming::address&& addr,
            threads::thread_priority priority,
            parcelset::parcelhandler::write_handler_type cb, Ts&&... vs)
        {
            // placeholder for action specialization
            std::terminate();
            return false;
        }

        template <typename Continuation, typename ...Ts>
        static bool call_cont_cb(naming::id_type const& id,
            naming::address&& addr, threads::thread_priority priority,
            Continuation && cont,
            parcelset::parcelhandler::write_handler_type cb, Ts&&... vs)
        {
            // placeholder for action specialization
            std::terminate();
            return false;
        }

    };
}}

#endif

