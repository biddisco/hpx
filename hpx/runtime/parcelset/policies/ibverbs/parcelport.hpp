//  Copyright (c) 2014 Thomas Heller
//  Copyright (c) 2007-2014 Hartmut Kaiser
//  Copyright (c) 2007 Richard D Guidry Jr
//  Copyright (c) 2011 Bryce Lelbach
//  Copyright (c) 2011 Katelyn Kufahl
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_POLICIES_IBVERBS_PARCELPORT_HPP
#define HPX_PARCELSET_POLICIES_IBVERBS_PARCELPORT_HPP

#include <hpx/runtime/parcelset/parcelport.hpp>
#include <hpx/runtime/parcelset/detail/call_for_each.hpp>
#include <hpx/runtime/threads/thread.hpp>
#include <hpx/util/io_service_pool.hpp>
#include <hpx/util/connection_cache.hpp>
#include <hpx/util/runtime_configuration.hpp>

namespace hpx { namespace parcelset { namespace policies { namespace ibverbs {
    struct HPX_EXPORT parcelport_impl : hpx::parcelset::parcelport
    {
    public:
        static const char * connection_handler_name()
        {
            return "ibverbs";
        }

        static std::size_t thread_pool_size(util::runtime_configuration const& ini)
        {
            std::string key("hpx.parcel.ibverbs");

            std::string thread_pool_size =
                ini.get_entry(key + ".io_pool_size", "1");
            return boost::lexical_cast<std::size_t>(thread_pool_size);
        }

        static const char *pool_name()
        {
            return "parcel_pool_ibverbs";
        }

        static const char * pool_name_postfix()
        {
            return "-ibverbs";
        }

        static std::vector<std::string> runtime_configuration();

        /// Construct the parcelport on the given locality.
        parcelport_impl(util::runtime_configuration const& ini,
            HPX_STD_FUNCTION<void(std::size_t, char const*)> const& on_start_thread,
            HPX_STD_FUNCTION<void()> const& on_stop_thread);

        bool do_run();

        bool run(bool blocking = true)
        {
            io_service_pool_.run(false);    // start pool

            // The do_run function will "bootstrap" the locality.
            // That is, once this function returns, the parcelport should be
            // in a state where it can accept new connections. This function
            // does not block, it returns once the parcelport can accept connections
            // It will be called from the main thread.
            bool success = do_run();

            if (blocking)
                io_service_pool_.join();

            return success;
        }

        void stop(bool blocking = true)
        {
            // make sure no more work is pending, wait for service pool to get empty
            io_service_pool_.stop();
            if (blocking) {
                // Just stop any ongoing communication and call it a day
            }
        }

        void put_parcel(parcel p, write_handler_type f);
        void put_parcels(std::vector<parcel> parcels, std::vector<write_handler_type> handlers)
        {
            naming::locality const& locality_id =
                parcels[0].get_destination_locality();

            for (std::size_t i = 1; i != parcels.size(); ++i)
            {
                HPX_ASSERT(locality_id == parcels[i].get_destination_locality());
                put_parcel(parcels[i], handlers[i]);
            }
        }

        void do_background_work()
        {
            // This function will be called every now and then.
            // For example when the thread manager runs out of work.
            // This can be used to do maintenance tasks
        }

        void send_early_parcel(parcel& p)
        {
            // This should not be used for the ibverbs PP
            HPX_ASSERT(false);
        }

        util::io_service_pool* get_thread_pool(char const* name)
        {
            if (0 == std::strcmp(name, io_service_pool_.get_name()))
                return &io_service_pool_;
            return 0;
        }

        virtual std::string get_locality_name() const
        {
            return "ibverbs";
        }

        /// Retrieve the type of the locality represented by this parcelport
        connection_type get_type() const
        {
            return connection_ibverbs;
        }

        /// Cache specific functionality
        void remove_from_connection_cache(naming::locality const& loc)
        {
            // This function is called when a locality disconnects.
            // It makes sure that connections to a specific locality are
            // getting closed.
        }

        /// Temporarily enable/disable all parcel handling activities in the
        /// parcelport
        void enable(bool new_state)
        {
            enable_parcel_handling_ = new_state;
        }

        /////////////////////////////////////////////////////////////////////////
        // Return the given connection cache statistic
        boost::int64_t get_connection_cache_statistics(
            connection_cache_statistics_type t, bool reset)
        {
            // We don't need a cache here ... just return 0 always.
            return 0;
        }

        /// support enable_shared_from_this
        boost::shared_ptr<parcelport_impl> shared_from_this()
        {
            return boost::static_pointer_cast<parcelport_impl>(
                parcelset::parcelport::shared_from_this());
        }

        boost::shared_ptr<parcelport_impl const> shared_from_this() const
        {
            return boost::static_pointer_cast<parcelport_impl const>(
                parcelset::parcelport::shared_from_this());
        }

    public:
        /// The pool of io_service objects used to perform asynchronous operations.
        util::io_service_pool io_service_pool_;

        int archive_flags_;
    };
}}}}

#endif
