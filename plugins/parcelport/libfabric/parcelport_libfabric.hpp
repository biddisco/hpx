//  Copyright (c) 2015-2016 John Biddiscombe
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// config
#include <hpx/config.hpp>
// util
#include <hpx/command_line_handling/command_line_handling.hpp>
#include <hpx/modules/format.hpp>
#include <hpx/runtime_configuration/runtime_configuration.hpp>
#include <hpx/timing/high_resolution_timer.hpp>
#include <hpx/threading_base/thread_data.hpp>

// The memory pool specialization need to be pulled in before encode_parcels
#include <hpx/runtime.hpp>
#include <hpx/runtime/parcelset/parcelport.hpp>
#include <hpx/runtime/parcelset/parcel_buffer.hpp>
#include <hpx/plugins/parcelport_factory.hpp>
#include <hpx/runtime/parcelset/parcelport_impl.hpp>
//
#include <hpx/runtime_local/thread_stacktrace.hpp>
//
// This header is generated by CMake and contains a number of configurable
// setting that affect the parcelport. It needs to be #included before
// other files that are part of the parcelport
#include <hpx/config/parcelport_defines.hpp>

// --------------------------------------------------------------------
// Enable the use of boost small_vector for certain short lived storage
// elements within the parcelport. This can reduce some memory allocations
#define HPX_PARCELPORT_LIBFABRIC_USE_SMALL_VECTOR    true

#define HPX_PARCELPORT_LIBFABRIC_IMM_UNSUPPORTED 1

// --------------------------------------------------------------------
//
#include <hpx/runtime/parcelset/rma/detail/memory_region_impl.hpp>
#include <hpx/runtime/parcelset/rma/memory_pool.hpp>
//
#include <plugins/parcelport/unordered_map.hpp>
#include <plugins/parcelport/performance_counter.hpp>
#include <plugins/parcelport/parcelport_logging.hpp>
//
#include <plugins/parcelport/libfabric/header.hpp>
#include <plugins/parcelport/libfabric/locality.hpp>
#include <plugins/parcelport/libfabric/libfabric_region_provider.hpp>
#include <plugins/parcelport/libfabric/rdma_locks.hpp>
#include <plugins/parcelport/libfabric/sender.hpp>
#include <plugins/parcelport/libfabric/connection_handler.hpp>

//
#if HPX_PARCELPORT_LIBFABRIC_USE_SMALL_VECTOR
# include <boost/container/small_vector.hpp>
#endif
//
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace hpx::parcelset::policies;

namespace hpx {
namespace parcelset {
namespace policies {
namespace libfabric
{

    class controller;

    // Smart pointer for controller obje
    typedef std::shared_ptr<controller> controller_ptr;

    // --------------------------------------------------------------------
    // parcelport, the implementation of the parcelport itself
    // --------------------------------------------------------------------
    struct HPX_EXPORT parcelport : public parcelport_impl<parcelport>
    {
    private:
        typedef parcelport_impl<parcelport> base_type;

    public:

        // These are the types used in the parcelport for locking etc
        // Note that spinlock is the only supported mutex that works on HPX+OS threads
        // and condition_variable_any can be used across HPX/OS threads
        typedef hpx::lcos::local::spinlock                                   mutex_type;
        typedef hpx::parcelset::policies::libfabric::scoped_lock<mutex_type> scoped_lock;
        typedef hpx::parcelset::policies::libfabric::unique_lock<mutex_type> unique_lock;
        typedef rma::detail::memory_region_impl<libfabric_region_provider>   region_type;
        //typedef memory_region_allocator<libfabric_region_provider>        allocator_type;

        // --------------------------------------------------------------------
        // main vars used to manage the RDMA controller and interface
        // These are called from a static function, so use static
        // --------------------------------------------------------------------
        controller_ptr controller_;

        // our local ip address (estimated based on fabric PP address info)
        uint32_t ip_addr_;

        // Not currently working, we support bootstrapping, but when not enabled
        // we should be able to skip it
        bool bootstrap_enabled_;
        bool parcelport_enabled_;

        bool bootstrap_complete;

        // @TODO, clean up the allocators, buffers, chunk_pool etc so that there is a
        // more consistent reuse of classes/types.
        // The use of pointer allocators etc is a dreadful hack and needs reworking

        typedef header<HPX_PARCELPORT_LIBFABRIC_MESSAGE_HEADER_SIZE> header_type;
        static constexpr unsigned int header_size = header_type::header_block_size;
        typedef rma::memory_pool<libfabric_region_provider> memory_pool_type;
        typedef pinned_memory_vector<char, header_size, region_type, memory_pool_type>
            snd_data_type;
        typedef parcel_buffer<snd_data_type> snd_buffer_type;
        // when terminating the parcelport, this is used to restrict access
        mutex_type  stop_mutex;

        // We must maintain a list of senders that are being used
        using sender_list =
            boost::lockfree::stack<
                sender*,
                boost::lockfree::capacity<HPX_PARCELPORT_LIBFABRIC_MAX_SENDS>,
                boost::lockfree::fixed_sized<true>
            >;

        sender_list senders_;

        // Used to help with shutdown
        std::atomic<bool>         stopped_;
        memory_pool_type*         chunk_pool_;

        // performance_counters::parcels::gatherer& parcels_sent_;

        // for debugging/performance measurement
        performance_counter<unsigned int> completions_handled_;
        std::atomic<unsigned int> senders_in_use_;

        // --------------------------------------------------------------------
        // Constructor : mostly just initializes the superclass with 'here'
        // --------------------------------------------------------------------
        parcelport(util::runtime_configuration const& ini,
            threads::policies::callback_notifier const& notifier);

        // Start the handling of connections.
        bool do_run();

        // --------------------------------------------------------------------
        // return a sender object back to the parcelport_impl
        // this is used by the send_immediate version of parcelport_impl
        // --------------------------------------------------------------------
        libfabric::sender* get_sender(libfabric::locality const& dest);
        libfabric::sender* get_connection(parcelset::locality const& dest);

        // --------------------------------------------------------------------
        // put a used sender object back into the sender queue for reuse
        void reclaim_connection(sender* s);

        // --------------------------------------------------------------------
        // return a sender object back to the parcelport_impl
        // this is for compatibility with non send_immediate operation
        // --------------------------------------------------------------------
        sender *create_connection_raw(parcelset::locality const& dest);

        // --------------------------------------------------------------------
        // parcelport destructor
        ~parcelport();

        // --------------------------------------------------------------------
        // Should not be used any more as parcelport_impl handles this?
        bool can_bootstrap() const;

        // --------------------------------------------------------------------
        // this will copy the data into the header and send it without going
        // through the usual parcel encoding process. It is intended for
        // bootstrap messages and should not be used during normal runtime
        // unless special flags are used in the header to distinguish the messages
        // from normal hpx traffic.
        // Data sent must be small and fit into a header message.
        void send_raw_data(const libfabric::locality &dest,
                           void const *data, std::size_t size,
                           unsigned int flags=0);

        // --------------------------------------------------------------------
        // bootstrap functions. @TODO document fully.
        void send_bootstrap_address();
        void recv_bootstrap_address(const std::vector<libfabric::locality> &addresses);
        void set_bootstrap_complete();
        bool bootstrapping();

        /// Return the name of this locality
        std::string get_locality_name() const;

        parcelset::locality agas_locality(util::runtime_configuration const & ini) const;

        parcelset::locality create_locality() const;

        // @TODO make this inline (circular ref problem)
        rma::memory_region *allocate_region(std::size_t size) override;

        // @TODO make this inline (circular ref problem)
        int deallocate_region(rma::memory_region *region) override;

        /*static*/ void suspended_task_debug(const std::string &match);

        void do_stop();

        // --------------------------------------------------------------------
        bool can_send_immediate();

        // --------------------------------------------------------------------
        template <typename Handler>
        bool async_write(Handler && handler,
            sender *sender, snd_buffer_type &buffer);

        // --------------------------------------------------------------------
        // This is called to poll for completions and handle all incoming messages
        // as well as complete outgoing messages.
        // --------------------------------------------------------------------
        // Background work
        //
        // This is called whenever the main thread scheduler is idling,
        // is used to poll for events, messages on the libfabric connection
        // --------------------------------------------------------------------
        bool background_work(
            std::size_t num_thread, parcelport_background_mode mode);
        void io_service_work();
        bool background_work_OS_thread();

    };
}}}}

namespace hpx {
namespace traits {
// Inject additional configuration data into the factory registry for this
// type. This information ends up in the system wide configuration database
// under the plugin specific section:
//
//      [hpx.parcel.libfabric]
//      ...
//      priority = 100
//
template<>
struct plugin_config_data<hpx::parcelset::policies::libfabric::parcelport> {
    static char const* priority() {
        FUNC_START_DEBUG_MSG;
        FUNC_END_DEBUG_MSG;
        return "10000";
    }

    // This is used to initialize your parcelport,
    // for example check for availability of devices etc.
    static void init(int *argc, char ***argv, util::command_line_handling &cfg) {
        FUNC_START_DEBUG_MSG;
#ifdef HPX_PARCELPORT_LIBFABRIC_HAVE_BOOTSTRAPPING
//#ifdef HPX_PARCELPORT_LIBFABRIC_HAVE_PMI
        cfg.ini_config_.push_back("hpx.parcel.bootstrap!=libfabric");
#endif
        FUNC_END_DEBUG_MSG;
    }

    static char const* call()
    {
        FUNC_START_DEBUG_MSG;
        FUNC_END_DEBUG_MSG;
        // @TODO : check which of these are obsolete after recent changes
        return
        "provider = ${HPX_PARCELPORT_LIBFABRIC_PROVIDER:"
                HPX_PARCELPORT_LIBFABRIC_PROVIDER "}\n"
        "domain = ${HPX_PARCELPORT_LIBFABRIC_DOMAIN:"
                HPX_PARCELPORT_LIBFABRIC_DOMAIN "}\n"
        "endpoint = ${HPX_PARCELPORT_LIBFABRIC_ENDPOINT:"
                HPX_PARCELPORT_LIBFABRIC_ENDPOINT "}\n"
        ;
    }
};
}}

