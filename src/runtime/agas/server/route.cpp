//  Copyright (c) 2011 Vinay C Amatya
//  Copyright (c) 2007-2015 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/config.hpp>
#include <hpx/runtime/applier/applier.hpp>
#include <hpx/runtime/actions/continuation.hpp>
#include <hpx/runtime/agas/server/primary_namespace.hpp>
#include <hpx/runtime/components/stubs/runtime_support.hpp>
#include <hpx/runtime.hpp>
#include <hpx/util/scoped_timer.hpp>

#include <boost/atomic.hpp>
#include <boost/format.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace hpx { namespace agas { namespace server
{
    void primary_namespace::route(parcelset::parcel && p)
    { // {{{ route implementation
        util::scoped_timer<boost::atomic<std::int64_t> > update(
            counter_data_.route_.time_
        );
        counter_data_.increment_route_count();

        error_code ec = throws;

        naming::gid_type const& gid = p.destination();
        naming::address& addr = p.addr();
        resolved_type cache_address;

        runtime& rt = get_runtime();

        // resolve destination addresses, we should be able to resolve all of
        // them, otherwise it's an error
        {
            std::unique_lock<mutex_type> l(mutex_);

            // wait for any migration to be completed
            wait_for_migration_locked(l, gid, ec);

            cache_address = resolve_gid_locked(l, gid, ec);

            if (ec || hpx::util::get<0>(cache_address) == naming::invalid_gid)
            {
                l.unlock();

                HPX_THROWS_IF(ec, no_success,
                    "primary_namespace::route",
                    boost::str(boost::format(
                            "can't route parcel to unknown gid: %s"
                        ) % gid));

                return;
            }

            // retain don't store in cache flag
            if (!naming::detail::store_in_cache(gid))
            {
                naming::detail::set_dont_store_in_cache(
                    hpx::util::get<0>(cache_address));
            }

            gva const g = hpx::util::get<1>(cache_address).resolve(
                gid, hpx::util::get<0>(cache_address));

            addr.locality_ = g.prefix;
            addr.type_ = g.type;
            addr.address_ = g.lva();
        }

        naming::id_type source = p.source_id();

        // either send the parcel on its way or execute actions locally
        if (addr.locality_ == get_locality())
        {
            // destination is local
            p.schedule_action();
        }
        else
        {
            // destination is remote
            rt.get_parcel_handler().put_parcel(std::move(p), false);
        }

        if (rt.get_state() < state_pre_shutdown)
        {
            // asynchronously update cache on source locality
            // update remote cache if the id is not flagged otherwise
            naming::gid_type const& id = hpx::util::get<0>(cache_address);
            if (id && naming::detail::store_in_cache(id))
            {
                gva const& g = hpx::util::get<1>(cache_address);
                naming::address addr(g.prefix, g.type, g.lva());

                using components::stubs::runtime_support;
                runtime_support::update_agas_cache_entry_colocated(
                    source, id, addr, g.count, g.offset);
            }
        }
    } // }}}
}}}

