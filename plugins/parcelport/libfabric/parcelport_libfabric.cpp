//  Copyright (c) 2015-2016 John Biddiscombe
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <plugins/parcelport/libfabric/parcelport_libfabric.hpp>

// TODO: cleanup includes

// config
#include <hpx/config.hpp>
// util
#include <hpx/synchronization/condition_variable.hpp>
#include <hpx/threading_base/thread_data.hpp>
#include <hpx/command_line_handling/command_line_handling.hpp>
#include <hpx/timing/high_resolution_timer.hpp>
#include <hpx/runtime_configuration/runtime_configuration.hpp>
#include <hpx/util/command_line_handling.hpp>
#include <hpx/util/get_entry_as.hpp>
#include <hpx/basic_execution/this_thread.hpp>

// The memory pool specialization need to be pulled in before encode_parcels
#include <hpx/plugins/parcelport_factory.hpp>
#include <hpx/runtime.hpp>
#include <hpx/runtime/parcelset/decode_parcels.hpp>
#include <hpx/runtime/parcelset/encode_parcels.hpp>
#include <hpx/runtime/parcelset/parcel_buffer.hpp>
#include <hpx/runtime/parcelset/parcelport.hpp>
#include <hpx/runtime/parcelset/parcelport_impl.hpp>
//
#include <hpx/assert.hpp>
#include <hpx/runtime_local/thread_stacktrace.hpp>
//
#include <boost/asio/ip/host_name.hpp>
//
// This header is generated by CMake and contains a number of configurable
// setting that affect the parcelport. It needs to be #included before
// other files that are part of the parcelport
#include <hpx/config/parcelport_defines.hpp>

// --------------------------------------------------------------------
// Enable the use of boost small_vector for certain short lived storage
// elements within the parcelport. This can reduce some memory allocations
#define HPX_PARCELPORT_LIBFABRIC_USE_SMALL_VECTOR    true

// --------------------------------------------------------------------
#include <hpx/runtime/parcelset/rma/memory_pool.hpp>
//
#include <plugins/parcelport/parcelport_logging.hpp>
#include <plugins/parcelport/performance_counter.hpp>
#include <plugins/parcelport/unordered_map.hpp>
//
#include <plugins/parcelport/libfabric/header.hpp>
#include <plugins/parcelport/libfabric/locality.hpp>
#include <plugins/parcelport/libfabric/libfabric_region_provider.hpp>
#include <plugins/parcelport/libfabric/connection_handler.hpp>
#include <plugins/parcelport/libfabric/rdma_locks.hpp>
#include <plugins/parcelport/libfabric/controller.hpp>

//
#if HPX_PARCELPORT_LIBFABRIC_USE_SMALL_VECTOR
# include <boost/container/small_vector.hpp>
#endif
//
#include <unordered_map>
#include <memory>
#include <mutex>
#include <sstream>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <list>
#include <string>
#include <utility>
#include <vector>

using namespace hpx::parcelset::policies;

namespace hpx {
namespace parcelset {
namespace policies {
namespace libfabric
{
    // --------------------------------------------------------------------
    // parcelport, the implementation of the parcelport itself
    // --------------------------------------------------------------------

    // --------------------------------------------------------------------
    // Constructor : mostly just initializes the superclass with 'here'
    // --------------------------------------------------------------------
    parcelport::parcelport(util::runtime_configuration const& ini,
        threads::policies::callback_notifier const& notifier)
      : base_type(ini, locality(), notifier)
        , bootstrap_complete(false)
        , stopped_(false)
        , completions_handled_(0)
        , senders_in_use_(0)
    {
        FUNC_START_DEBUG_MSG;

        // if we are not enabled, then skip allocating resources
        parcelport_enabled_ = hpx::util::get_entry_as<bool>(ini,
            "hpx.parcel.libfabric.enable", 0);
        LOG_DEBUG_MSG("Got enabled " << parcelport_enabled_);

        bootstrap_enabled_ = ("libfabric" ==
            hpx::util::get_entry_as<std::string>(ini, "hpx.parcel.bootstrap", ""));
        LOG_DEBUG_MSG("Got bootstrap " << bootstrap_enabled_);

        if (!parcelport_enabled_) return;

        // Get parameters that determine our fabric selection
        std::string provider = ini.get_entry("hpx.parcel.libfabric.provider",
            HPX_PARCELPORT_LIBFABRIC_PROVIDER);
        std::string domain = ini.get_entry("hpx.parcel.libfabric.domain",
            HPX_PARCELPORT_LIBFABRIC_DOMAIN);
        std::string endpoint = ini.get_entry("hpx.parcel.libfabric.endpoint",
            HPX_PARCELPORT_LIBFABRIC_ENDPOINT);

        LOG_DEBUG_MSG("libfabric parcelport function using attributes "
            << provider << " " << domain << " " << endpoint);

        // create our main fabric control structure
        controller_ = std::make_shared<controller>(
            provider, domain, endpoint, this);

        // get 'this' locality from the controller
        LOG_DEBUG_MSG("Getting local locality object");
        const locality & local = controller_->here();
        here_ = parcelset::locality(local);
        // and make a note of our ip address for convenience
        ip_addr_ = local.ip_address();

        FUNC_END_DEBUG_MSG;
    }

    // --------------------------------------------------------------------
    // during bootup, this is used by the service threads
    void parcelport::io_service_work()
    {
        while (hpx::is_starting())
        {
            background_work(0, parcelport_background_mode_all);
        }
        LOG_DEBUG_MSG("io service task completed");
    }

    // --------------------------------------------------------------------
    // Start the handling of communication.
    bool parcelport::do_run()
    {
        if (!parcelport_enabled_) return false;

#ifndef HPX_PARCELPORT_LIBFABRIC_HAVE_BOOTSTRAPPING
        auto &as = this->applier_->get_agas_client();
        controller_->initialize_localities(as);
#endif

        FUNC_START_DEBUG_MSG;
        controller_->startup(this);

        LOG_DEBUG_MSG("Fetching memory pool");
        chunk_pool_ = &controller_->get_memory_pool();

        // setup provider specific allocator for rma_object use
        rma::detail::allocator_impl<char, libfabric_region_provider> *default_allocator =
            new rma::detail::allocator_impl<char, libfabric_region_provider>(chunk_pool_);
        allocator_ = default_allocator;
        //
        for (std::size_t i = 0; i < HPX_PARCELPORT_LIBFABRIC_MAX_SENDS; ++i)
        {
            sender *snd =
               new sender(this,
                    controller_->ep_active_,
                    controller_->get_domain(),
                    chunk_pool_);
            // after a sender has been used, it's postprocess handler
            // is called, this returns it to the free list
            snd->postprocess_handler_ = [this](sender* s)
                {
                    --senders_in_use_;
                    LOG_DEBUG_MSG("senders in use (-- postprocess handler) "
                                  << hexpointer(s)
                                  << decnumber(senders_in_use_));
                    senders_.push(s);
//                    trigger_pending_work();
                };
            senders_.push(snd);
        }

        if (bootstrap_enabled_)
        {
            for (std::size_t i = 0; i != io_service_pool_.size(); ++i)
            {
                io_service_pool_.get_io_service(int(i)).post(
                    hpx::util::bind(
                        &parcelport::io_service_work, this));
            }
        }
        return true;
    }

    // --------------------------------------------------------------------
    void parcelport::send_raw_data(const libfabric::locality &dest,
                                   void const *data, std::size_t size,
                                   unsigned int flags)
    {
        FUNC_START_DEBUG_MSG;
        LOG_DEBUG_MSG("send_raw_data (bootstrap) " << hexnumber(size));
        HPX_ASSERT(size<HPX_PARCELPORT_LIBFABRIC_MESSAGE_HEADER_SIZE);

        // keep trying until we get a sender as we cannot block/yield
        // during bootup
        sender *sndr = nullptr;
        while (!sndr) {
            sndr = get_sender(dest);
            if (sndr) {
                LOG_DEBUG_MSG("send_raw_data gets sender " << hexpointer(sndr));
            }
        }

        // reset buffer for data
        sndr->buffer_ = sndr->get_new_buffer();

        // 0 zero copy chunks,
        // 1 index chunk containing our address
        sndr->buffer_.num_chunks_ = snd_buffer_type::count_chunks_type(0, 1);
        sndr->buffer_.chunks_.push_back(serialization::create_index_chunk(0, 0));
        // copy locality data into buffer
        sndr->buffer_.data_.resize(size);
        std::memcpy(sndr->buffer_.data_.begin(), data, size);
        sndr->buffer_.size_ = sndr->buffer_.data_.size();
        sndr->handler_ = [](error_code const &){
            LOG_DEBUG_MSG("send_raw_data (bootstrap) send completion handled");
        };

        sndr->async_write_impl(flags);
        FUNC_END_DEBUG_MSG;
    }

    // --------------------------------------------------------------------
    void parcelport::send_bootstrap_address()
    {
        FUNC_START_DEBUG_MSG;
        LOG_DEBUG_MSG("Sending bootstrap address to agas server : here = "
                      << ipaddress(controller_->here_.ip_address()) << ":"
                      << decnumber(controller_->here_.port()));

        bootstrap_complete = false;
        send_raw_data(controller_->agas_,
                      controller_->here_.fabric_data(),
                      locality::array_size,
                      libfabric::header<HPX_PARCELPORT_LIBFABRIC_MESSAGE_HEADER_SIZE>::bootstrap_flag);
        FUNC_END_DEBUG_MSG;
    }

    // --------------------------------------------------------------------
    void parcelport::set_bootstrap_complete()
    {
        LOG_DEBUG_MSG("bootstrap complete");
        bootstrap_complete = true;
    }

    // --------------------------------------------------------------------
    void parcelport::recv_bootstrap_address(
            const std::vector<libfabric::locality> &addresses)
    {
        FUNC_START_DEBUG_MSG;
        libfabric::locality here = controller_->here_;
        // rank 0 is agas and should already be in our address vector
        for (const libfabric::locality &addr : addresses) {
            if (addr == controller_->agas_) {
                // agas (rank 0) should already be in vector, skip it
                LOG_DEBUG_MSG("bootstrap skipping agas " << iplocality(addr));
                continue;
            }
            // add this address to vector and get rank assignment
            auto full_addr = controller_->insert_address(addr);
            if (addr == here) {
                // update controller 'here' address with new rank assignment
                LOG_DEBUG_MSG("bootstrap we are " << iplocality(full_addr));
                controller_->here_ = full_addr;
                here_ = parcelset::locality(full_addr);
            }
        }
        FUNC_END_DEBUG_MSG;
    }

    // --------------------------------------------------------------------
    bool parcelport::bootstrapping()
    {
        return !bootstrap_complete;
    }

    // --------------------------------------------------------------------
    // return a sender object back to the parcelport_impl
    // this is used by the send_immediate version of parcelport_impl
    // --------------------------------------------------------------------
    libfabric::sender* parcelport::get_sender(libfabric::locality const& dest)
    {
        sender* snd = nullptr;
        if (senders_.pop(snd))
        {
            snd->dst_addr_ = dest.fi_address();
            ++senders_in_use_;
            LOG_DEBUG_MSG("get_sender "
                << hexpointer(snd)
                << " : get address from "
                << iplocality(here_.get<libfabric::locality>())
                << "to " << iplocality(dest)
                << "fi_addr (rank) " << hexnumber(snd->dst_addr_)
                << "senders in use (++ get_sender) "
                << decnumber(senders_in_use_));
        }
        return snd;
    }

    // --------------------------------------------------------------------
    // return a sender object back to the parcelport_impl
    // this is used by the send_immediate version of parcelport_impl
    // --------------------------------------------------------------------
    libfabric::sender* parcelport::get_connection(libfabric::locality const& dest)
    {
        FUNC_START_DEBUG_MSG;
        LOG_TIMED_INIT(background);
        LOG_TIMED_BLOCK(background, DEVEL, 60.0, {
            suspended_task_debug("");
        });

        sender* snd = nullptr;
        hpx::util::yield_while([this, &snd, &dest]()
        {
            snd = get_sender(dest);
            if (snd != nullptr) {
                // all ok, stop yielding
                FUNC_END_DEBUG_MSG;
                return false;
            }
            background_work(0);
            return true;
        });
        FUNC_END_DEBUG_MSG;
        return snd;
    }

    // --------------------------------------------------------------------
    // return a sender object back to the parcelport_impl
    // this is used by the send_immediate version of parcelport_impl
    // --------------------------------------------------------------------
    libfabric::sender* parcelport::get_connection(parcelset::locality const& dest)
    {
        return get_connection(dest.get<libfabric::locality>());
    }

    // --------------------------------------------------------------------
    bool parcelport::can_send_immediate()
    {
        // there is an implicit race here because we might return
        // true or false and it may change before the parcel is ready
        // but it's ok because we handle either case - this is just a "hint"
        // to optimize performance when there are free senders and we can use them
        bool empty = senders_.empty();
        if (empty) {
            LOG_DEBUG_MSG("can_send_immediate false");
        }
        return !empty;
    }

    // --------------------------------------------------------------------
    // if no senders are available, can_send_immediate returns false
    // and parcels are queued up, then sent using this interface
    // --------------------------------------------------------------------
    std::shared_ptr<sender> parcelport::create_connection(
        parcelset::locality const& dest, error_code& ec)
    {
        FUNC_START_DEBUG_MSG;
        LOG_DEVEL_MSG("create_connection new sender");
        std::shared_ptr<sender> new_sender = std::make_shared<sender>(
                this,
                controller_->ep_active_,
                controller_->get_domain(),
                chunk_pool_);
        //
        ++senders_in_use_;
        //
        new_sender->postprocess_handler_ = [this](sender* s)
            {
                --senders_in_use_;
                LOG_DEVEL_MSG("senders in use (-- temp sender postprocess handler) "
                              << hexpointer(s)
                              << decnumber(senders_in_use_));
                // do not push this one onto the sender stack
                // since it is a temporary one.
//                trigger_pending_work();
            };
        FUNC_END_DEBUG_MSG;
        return nullptr; // return new_sender;
    }

    // --------------------------------------------------------------------
    // return a sender connection : for unknown reasons the parcelport_impl
    // sometimes gives back the connection/sender without using it
    // --------------------------------------------------------------------
    void parcelport::reclaim_connection(sender* s)
    {
        FUNC_START_DEBUG_MSG;
        LOG_DEBUG_MSG("senders in use (-- reclaim_connection) "
                      << hexpointer(s)
                      << decnumber(senders_in_use_));
        s->postprocess_handler_(s);
        FUNC_END_DEBUG_MSG;
    }

    rma::memory_region *parcelport::allocate_region(std::size_t size) {
        return controller_->get_memory_pool().allocate_region(size);
    }

    int parcelport::deallocate_region(rma::memory_region *region) {
        region_type *r = dynamic_cast<region_type*>(region);
        HPX_ASSERT(r);
        controller_->get_memory_pool().deallocate(r);
        return 0;
    }

    // --------------------------------------------------------------------
    // cleanup
    parcelport::~parcelport() {
        FUNC_START_DEBUG_MSG;
        scoped_lock lk(stop_mutex);
        sender *snd = nullptr;

        unsigned int sends_posted  = 0;
        unsigned int sends_deleted = 0;
        unsigned int acks_received = 0;
        //
        while (senders_.pop(snd)) {
            LOG_DEBUG_MSG("Popped a sender for delete " << hexpointer(snd));
            sends_posted  += snd->sends_posted_;
            sends_deleted += snd->sends_deleted_;
            acks_received += snd->acks_received_;
            delete snd;
        }
        LOG_DEBUG_MSG(
               "sends_posted "  << decnumber(sends_posted)
            << "sends_deleted " << decnumber(sends_deleted)
            << "acks_received " << decnumber(acks_received)
            << "non_rma-send "  << decnumber(sends_posted-acks_received));
        //
        controller_ = nullptr;
        FUNC_END_DEBUG_MSG;
    }

    // --------------------------------------------------------------------
    /// Should not be used any more as parcelport_impl handles this?
    bool parcelport::can_bootstrap() const {
        FUNC_START_DEBUG_MSG;
        bool can_boot = HPX_PARCELPORT_LIBFABRIC_HAVE_BOOTSTRAPPING();
        LOG_TRACE_MSG("Returning " << can_boot << " from can_bootstrap")
        FUNC_END_DEBUG_MSG;
        return can_boot;
    }

    // --------------------------------------------------------------------
    /// return a string form of the locality name
    std::string parcelport::get_locality_name() const
    {
        FUNC_START_DEBUG_MSG;
        // return hostname:iblibfabric ip address
        std::stringstream temp;
        temp << boost::asio::ip::host_name() << ":" << ipaddress(ip_addr_);
        std::string tstr = temp.str();
        FUNC_END_DEBUG_MSG;
        return tstr.substr(0, tstr.size()-1);
    }

    // --------------------------------------------------------------------
    // the root node has spacial handling, this returns its Id
    parcelset::locality parcelport::
    agas_locality(util::runtime_configuration const & ini) const
    {
        FUNC_START_DEBUG_MSG;
        // load all components as described in the configuration information
        if (!bootstrap_enabled_)
        {
            LOG_ERROR_MSG("Should only return agas locality when bootstrapping");
        }
        FUNC_END_DEBUG_MSG;
        return controller_->agas_;
    }

    // --------------------------------------------------------------------
    parcelset::locality parcelport::create_locality() const {
        FUNC_START_DEBUG_MSG;
        FUNC_END_DEBUG_MSG;
        return parcelset::locality(locality());
    }

    // --------------------------------------------------------------------
    /// for debugging
    void parcelport::suspended_task_debug(const std::string &match)
    {
        std::string temp = hpx::util::debug::suspended_task_backtraces();
        if (match.size()==0 ||
            temp.find(match)!=std::string::npos)
        {
            if (temp.size()>0) {
                LOG_DEVEL_MSG("Rank "
                    << decnumber(this->controller_->here_.fi_address())
                    << "Suspended threads " << temp);
            }
        }
    }

    // --------------------------------------------------------------------
    /// stop the parcelport, prior to shutdown
    void parcelport::do_stop() {
        LOG_DEBUG_MSG("Entering libfabric stop ");
        FUNC_START_DEBUG_MSG;
        if (!stopped_) {
            // we don't want multiple threads trying to stop the clients
            scoped_lock lock(stop_mutex);

            LOG_DEBUG_MSG("Removing all initiated connections");
            controller_->disconnect_all();

            // wait for all clients initiated elsewhere to be disconnected
            while (controller_->active() /*&& !hpx::is_stopped()*/) {
                completions_handled_ += controller_->poll_endpoints(true);
                LOG_TIMED_INIT(disconnect_poll);
                LOG_TIMED_BLOCK(disconnect_poll, DEVEL, 5.0,
                    {
                        LOG_DEBUG_MSG("Polling before shutdown");
                    }
                )
            }
            LOG_DEBUG_MSG("stopped removing clients and terminating");
        }
        stopped_ = true;
        // Stop receiving and sending of parcels
    }

    // --------------------------------------------------------------------
    template <typename Handler>
    bool parcelport::async_write(Handler && handler,
        sender *snd, snd_buffer_type &buffer)
    {
        LOG_DEBUG_MSG("parcelport::async_write using sender " << hexpointer(snd));
        snd->buffer_ = std::move(buffer);
        HPX_ASSERT(!snd->handler_);
        snd->handler_ = std::forward<Handler>(handler);
        snd->async_write_impl();
        return true;
    }

    // --------------------------------------------------------------------
    // This is called to poll for completions and handle all incoming messages
    // as well as complete outgoing messages.
    //
    // Since the parcelport can be serviced by hpx threads or by OS threads,
    // we must use extra care when dealing with mutexes and condition_variables
    // since we do not want to suspend an OS thread, but we do want to suspend
    // hpx threads when necessary.
    //
    // NB: There is no difference any more between background polling work
    // on OS or HPX as all has been tested thoroughly
    // --------------------------------------------------------------------
    inline bool parcelport::background_work_OS_thread() {
        LOG_TIMED_INIT(background);
        LOG_TIMED_BLOCK(background, DEVEL, 60.0, {
            suspended_task_debug("");
        });

        unsigned int numc = 0;
        do {
            // if an event comes in, we may spend time processing/handling it
            // and another may arrive during this handling,
            // so keep checking until none are received
            numc = controller_->poll_endpoints();
            completions_handled_ += numc;
        } while (numc>0);
        return false;
    }

    // --------------------------------------------------------------------
    // Background work
    //
    // This is called whenever the main thread scheduler is idling,
    // is used to poll for events, messages on the libfabric connection
    // --------------------------------------------------------------------
    bool parcelport::background_work(
        std::size_t /*num_thread*/, parcelport_background_mode mode)
    {
        if (stopped_ || hpx::is_stopped()) {
            return false;
        }
        return background_work_OS_thread();
    }
}}}}

HPX_REGISTER_PARCELPORT(hpx::parcelset::policies::libfabric::parcelport, libfabric);
