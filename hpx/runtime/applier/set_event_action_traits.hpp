//  Copyright (c) 2016 Hartmut Kaiser
//  Copyright (c) 2016 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_BASIC_ACTION_TRAITS_AUG_2016)
#define HPX_BASIC_ACTION_TRAITS_AUG_2016

#include <hpx/config.hpp>
#include <hpx/runtime/actions/continuation.hpp>
#include <hpx/runtime/actions/plain_action.hpp>
#include <hpx/traits/action_put_parcel.hpp>
#include <hpx/runtime/naming/id_type.hpp>

namespace hpx { namespace traits
{

       template <typename Action>
        inline naming::address&& complement_addr(naming::address& addr)
        {
            if (components::component_invalid == addr.type_)
            {
                addr.type_ = components::get_component_type<
                    typename Action::component_type>();
            }
            return std::move(addr);
        }

    template <>
    struct action_put_parcel<hpx::lcos::base_lco::set_event_action>
    {   
        typedef typename
            hpx::traits::extract_action<hpx::lcos::base_lco::set_event_action>::type 
                action_type;

        template <typename ...Ts>
        static inline bool
        call(naming::id_type const& id, naming::address&& addr,
            threads::thread_priority priority, Ts&&... vs)
        {
            typedef typename 
            hpx::traits::extract_action<hpx::lcos::base_lco::set_event_action>::type
                action_type;
            action_type act;

            parcelset::put_parcel(id, std::move(complement_addr<action_type>(addr)),
                act, priority, std::forward<Ts>(vs)...);

            return false;     // destinations are remote
        }

        template <typename Continuation, typename ...Ts>
        static bool call_cont(naming::id_type const& id, naming::address&& addr,
            threads::thread_priority priority, Continuation && cont, Ts&&... vs)
        {
            parcelset::parcelhandler& ph =
                hpx::applier::get_applier().get_parcel_handler();

            parcelset::parcel p(id, complement_addr<action_type>(addr),
                std::forward<Continuation>(cont),
                action_type(), priority, std::forward<Ts>(vs)...);

            ph.put_parcel(std::move(p));
            return false;
        }

        template <typename ...Ts>
        static bool call_cb(naming::id_type const& id, naming::address&& addr,
            threads::thread_priority priority,
            parcelset::parcelhandler::write_handler_type cb, Ts&&... vs)
        {
            parcelset::parcelhandler& ph =
                hpx::applier::get_applier().get_parcel_handler();

            parcelset::parcel p(id, complement_addr<action_type>(addr),
                action_type(), priority, std::forward<Ts>(vs)...);

            ph.put_parcel(std::move(p), std::move(cb));
            return false;
        }

        template <typename Continuation, typename ...Ts>
        static bool call_cont_cb(naming::id_type const& id,
            naming::address&& addr, threads::thread_priority priority,
            Continuation && cont,
            parcelset::parcelhandler::write_handler_type cb, Ts&&... vs)
        {
            parcelset::parcelhandler& ph =
                hpx::applier::get_applier().get_parcel_handler();

            parcelset::parcel p(id, complement_addr<action_type>(addr),
                std::forward<Continuation>(cont),
                action_type(), priority, std::forward<Ts>(vs)...);

            ph.put_parcel(std::move(p), std::move(cb));
            return false;
        }
    };
}}

#endif