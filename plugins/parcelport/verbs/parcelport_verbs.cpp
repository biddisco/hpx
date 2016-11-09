//  Copyright (c) 2015-2016 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// config
#include <hpx/config.hpp>

// util
#include <hpx/util/command_line_handling.hpp>
#include <hpx/util/runtime_configuration.hpp>
#include <hpx/util/high_resolution_timer.hpp>
#include <hpx/util/memory_chunk_pool_allocator.hpp>
#include <hpx/lcos/local/condition_variable.hpp>
#include <hpx/runtime/threads/thread_data.hpp>

// The memory pool specialization need to be pulled in before encode_parcels
#include <hpx/runtime.hpp>
#include <hpx/runtime/parcelset/parcelport.hpp>
#include <hpx/runtime/parcelset/parcel_buffer.hpp>
#include <hpx/runtime/parcelset/encode_parcels.hpp>
#include <hpx/runtime/parcelset/decode_parcels.hpp>
#include <hpx/plugins/parcelport_factory.hpp>
#include <hpx/runtime/parcelset/parcelport_impl.hpp>
//
#include <hpx/util/debug/thread_stacktrace.hpp>

// --------------------------------------------------------------------
// The parcelport does not support HPX bootstrapping by default
// it is a work in progress and may cause lockups on start if enabled
// this value should be either std::true_type or std::false_type
#define HPX_PARCELPORT_VERBS_ENABLE_BOOTSTRAP      std::true_type

// --------------------------------------------------------------------
// This defines the standard size of messages that can be sent/received without
// resorting to RDMA GET operations to fetch data. Chunks are still retrieved
// using GET operations, this only affects the main message body/header size.
#define HPX_PARCELPORT_VERBS_MESSAGE_HEADER_SIZE   RDMA_POOL_SMALL_CHUNK_SIZE

// --------------------------------------------------------------------
// This determines the lower size limit for messages that are copied into an
// existing RDMA buffer. Messages larger than this will be registered on the fly
// and sent without a memory copy
#define HPX_PARCELPORT_VERBS_MEMORY_COPY_THRESHOLD RDMA_POOL_MEDIUM_CHUNK_SIZE

// --------------------------------------------------------------------
// The maximum number of sends that can be posted before we activate throttling
#define HPX_PARCELPORT_VERBS_MAX_SEND_QUEUE        64

// --------------------------------------------------------------------
// Controls whether we are allowed to suspend threads that are sending
// when we have maxed out the number of sends we can handle
#define HPX_PARCELPORT_VERBS_SUSPEND_WAKE         (HPX_PARCELPORT_VERBS_MAX_SEND_QUEUE/2)

// --------------------------------------------------------------------
// When defined, we use a reader/writer lock on maps to squeeze a tiny performance
// improvement from our code.
#define HPX_PARCELPORT_VERBS_USE_CONCURRENT_MAPS true

// --------------------------------------------------------------------
// Enable the use of boost small_vector for certain short lived storage
// elements within the parcelport. This can reduce some memory allocations
#define HPX_PARCELPORT_VERBS_USE_SMALL_VECTOR    true

// --------------------------------------------------------------------
// Turn on the use of debugging log messages for lock/unlock
// to help find places where locks are being taken before suspending
#define HPX_PARCELPORT_VERBS_DEBUG_LOCKS         false

// --------------------------------------------------------------------
// When enabled, the parcelport can create a custom scheduler to help
// make progress on the network.
// This is experimental and should not be used in general
#undef HPX_PARCELPORT_VERBS_USE_SPECIALIZED_SCHEDULER

// --------------------------------------------------------------------
// HPX_PARCELPORT_VERBS_IMM_UNSUPPORTED is set by CMake if the machine is a
// BlueGene active storage node which does not support immediate data sends
#undef HPX_PARCELPORT_VERBS_IMM_UNSUPPORTED

// --------------------------------------------------------------------
#include <plugins/parcelport/verbs/rdma/rdma_logging.hpp>
#include <plugins/parcelport/verbs/unordered_map.hpp>
#include <plugins/parcelport/verbs/rdma/rdma_memory_pool.hpp>
//
#include "plugins/parcelport/verbs/sender_connection.hpp"
#include "plugins/parcelport/verbs/connection_handler.hpp"
#include "plugins/parcelport/verbs/locality.hpp"
#include "plugins/parcelport/verbs/header.hpp"
#include "plugins/parcelport/verbs/pinned_memory_vector.hpp"
//
#if HPX_PARCELPORT_VERBS_USE_SPECIALIZED_SCHEDULER
# include "plugins/parcelport/verbs/scheduler.hpp"
#endif
//
// rdmahelper library
#include <plugins/parcelport/verbs/rdma/rdma_logging.hpp>
#include <plugins/parcelport/verbs/rdma/rdma_locks.hpp>
#include <plugins/parcelport/verbs/rdma/memory_region.hpp>
#include <plugins/parcelport/verbs/rdma/rdma_chunk_pool.hpp>
#include <plugins/parcelport/verbs/rdmahelper/include/RdmaController.h>
//
#if HPX_PARCELPORT_VERBS_USE_SMALL_VECTOR
# include <boost/container/small_vector.hpp>
#endif
//
#include <unordered_map>
#include <memory>
#include <mutex>

using namespace hpx::parcelset::policies;

namespace hpx {
namespace parcelset {
namespace policies {
namespace verbs
{
    // --------------------------------------------------------------------
    // simple atomic counter we use for tags
    // when a parcel is sent to a remote locality, it may need to pull zero copy chunks from us.
    // we keep the chunks until the remote locality sends a zero byte message with the tag we gave
    // them and then we know it is safe to release the memory back to the pool.
    // The tags can have a short lifetime, but must be unique, so we encode the ip address with
    // a counter to generate tags per destination.
    // The tag is sent in immediate data so must be 32bits only : Note that the tag only has a
    // lifetime of the unprocessed parcel, so it can be reused as soon as the parcel has been completed
    // and therefore a 16bit count is sufficient as we only keep a few parcels per locality in flight at a time
    // --------------------------------------------------------------------
    struct tag_provider {
        tag_provider() : next_tag_(1) {}

        uint32_t next(uint32_t ip_addr)
        {
            // @TODO track wrap around and collisions (how?)
            return (next_tag_++ & 0x0000FFFF) + (ip_addr & 0xFFFF0000);
        }

        // using 16 bits currently.
        boost::atomic<uint32_t> next_tag_;
    };

    // --------------------------------------------------------------------
    // parcelport, the implementation of the parcelport itself
    // --------------------------------------------------------------------
    class HPX_EXPORT parcelport : public parcelport_impl<parcelport>
    {
    private:
        typedef parcelport_impl<parcelport> base_type;

        // --------------------------------------------------------------------
        // returns a locality object that represents 'this' locality
        // --------------------------------------------------------------------
        static parcelset::locality here(util::runtime_configuration const& ini)
        {
            FUNC_START_DEBUG_MSG;
            if (ini.has_section("hpx.parcel.verbs")) {
                util::section const * sec = ini.get_section("hpx.parcel.verbs");
                if (NULL != sec) {
                    std::string ibverbs_enabled(sec->get_entry("enable", "0"));
                    if (boost::lexical_cast<int>(ibverbs_enabled)) {
                        // _ibverbs_ifname    = sec->get_entry("ifname",    HPX_PARCELPORT_VERBS_IFNAME);
                        _ibverbs_device    = sec->get_entry("device",    HPX_PARCELPORT_VERBS_DEVICE);
                        _ibverbs_interface = sec->get_entry("interface", HPX_PARCELPORT_VERBS_INTERFACE);
                        char buff[256];
                        _ibv_ip = ::Get_rdma_device_address(_ibverbs_device.c_str(), _ibverbs_interface.c_str(), buff);
                        LOG_DEBUG_MSG("here() got hostname of " << buff);
                    }
                }
            }
            if (ini.has_section("hpx.agas")) {
                util::section const* sec = ini.get_section("hpx.agas");
                if (NULL != sec) {
                    LOG_DEBUG_MSG("hpx.agas port number "
                        << decnumber(hpx::util::get_entry_as<boost::uint16_t>(*sec,
                            "port", HPX_INITIAL_IP_PORT)));
                    _port = hpx::util::get_entry_as<boost::uint16_t>(*sec,
                        "port", HPX_INITIAL_IP_PORT);
                }
            }
            if (ini.has_section("hpx.parcel")) {
                util::section const* sec = ini.get_section("hpx.parcel");
                if (NULL != sec) {
                    LOG_DEBUG_MSG("hpx.parcel port number "
                        << decnumber(hpx::util::get_entry_as<boost::uint16_t>(*sec,
                            "port", HPX_INITIAL_IP_PORT)));
                }
            }
            FUNC_END_DEBUG_MSG;
            return parcelset::locality(locality(_ibv_ip));
        }

    public:
        // These are the types used in the parcelport for locking etc
        // Note that spinlock is the only supported mutex that works on HPX+OS threads
        // and condition_variable_any can be used across HPX/OS threads
        typedef hpx::lcos::local::spinlock                               mutex_type;
        typedef hpx::parcelset::policies::verbs::scoped_lock<mutex_type> scoped_lock;
        typedef hpx::parcelset::policies::verbs::unique_lock<mutex_type> unique_lock;
        typedef hpx::lcos::local::condition_variable_any                 condition_type;

        // --------------------------------------------------------------------
        // main vars used to manage the RDMA controller and interface
        // --------------------------------------------------------------------
        static RdmaControllerPtr _rdmaController;
        static std::string       _ibverbs_device;
        static std::string       _ibverbs_interface;
        static boost::uint32_t   _port;
        static boost::uint32_t   _ibv_ip;

        // to quickly lookup a queue-pair (QP) from a destination ip address
        typedef hpx::concurrent::unordered_map<boost::uint32_t, boost::uint32_t> ip_map;
        typedef ip_map::iterator ip_map_iterator;
        ip_map ip_qp_map;

        // a map that stores the ip address and a boolean to tell us if a connection
        // to that address is currently being initiated. We need this to prevent two ends
        // of a connection simultaneously initiating a connection to each other
        typedef hpx::concurrent::unordered_map<boost::uint32_t, bool> connect_map;
        connect_map connection_requests_;

        // @TODO, clean up the allocators, buffers, chunk_pool etc so that there is a
        // more consistent reuse of classes/types.
        // The use of pointer allocators etc is a dreadful hack and needs reworking
        typedef header<HPX_PARCELPORT_VERBS_MESSAGE_HEADER_SIZE>   header_type;

        typedef rdma_memory_pool                                   memory_pool_type;
        typedef std::shared_ptr<memory_pool_type>                  memory_pool_ptr_type;
        typedef hpx::util::detail::memory_chunk_pool_allocator
                <char, memory_pool_type, mutex_type>               allocator_type;
        typedef util::detail::pinned_memory_vector<char>           rcv_data_type;
        typedef parcel_buffer<rcv_data_type>                       snd_buffer_type;
        typedef parcel_buffer<rcv_data_type, std::vector<char>>    rcv_buffer_type;

        // when terminating the parcelport, this is used to restrict access
        mutex_type  stop_mutex;
        //
        boost::atomic<bool>       stopped_;
        boost::atomic_uint        active_send_count_;
        boost::atomic<bool>       immediate_send_allowed_;

        memory_pool_ptr_type      chunk_pool_;
        verbs::tag_provider       tag_provider_;

#if HPX_PARCELPORT_VERBS_USE_SPECIALIZED_SCHEDULER
        custom_scheduler          parcelport_scheduler;
#endif
        // performance_counters::parcels::gatherer& parcels_sent_;

#if HPX_PARCELPORT_VERBS_USE_SMALL_VECTOR
        typedef boost::container::small_vector<rdma_memory_region*,4> zero_copy_vector;
#else
        typedef std::vector<rdma_memory_region*>                      zero_copy_vector;
#endif

        // --------------------------------------------------------------------
        // struct we use to keep track of all memory regions used during a send,
        // they must be held onto until all transfers of data are complete.
        // --------------------------------------------------------------------
        typedef struct {
            uint32_t                                                tag;
            std::atomic_flag                                        delete_flag;
            bool                                                    has_zero_copy;
            util::unique_function_nonser< void(error_code const&) > handler;
            rdma_memory_region                     *header_region, *message_region;
            zero_copy_vector                                        zero_copy_regions;
        } parcel_send_data;

        // --------------------------------------------------------------------
        // struct we use to keep track of all memory regions used during a recv,
        // they must be held onto until all transfers of data are complete.
        // --------------------------------------------------------------------
        typedef struct {
            std::atomic_uint                                 rdma_count;
            uint32_t                                         tag;
            std::vector<serialization::serialization_chunk>  chunks;
            rdma_memory_region              *header_region, *message_region;
            zero_copy_vector                                 zero_copy_regions;
        } parcel_recv_data;

        typedef std::list<parcel_send_data>      active_send_list_type;
        typedef active_send_list_type::iterator  active_send_iterator;
        //
        typedef std::list<parcel_recv_data>      active_recv_list_type;
        typedef active_recv_list_type::iterator  active_recv_iterator;

#if HPX_PARCELPORT_VERBS_USE_CONCURRENT_MAPS
        // map send/recv parcel wr_id to all info needed on completion
        typedef hpx::concurrent::unordered_map<uint64_t, active_send_iterator>
            send_wr_map;
        typedef hpx::concurrent::unordered_map<uint64_t, active_recv_iterator>
            recv_wr_map;
#else
        mutex_type  ReadCompletionMap_mutex;
        mutex_type  SendCompletionMap_mutex;
        mutex_type  TagSendCompletionMap_mutex;

        // map send/recv parcel wr_id to all info needed on completion
        typedef std::unordered_map<uint64_t, active_send_iterator> send_wr_map;
        typedef std::unordered_map<uint64_t, active_recv_iterator> recv_wr_map;
#endif
        // store received objects using a map referenced by verbs work request ID
        send_wr_map SendCompletionMap;
        send_wr_map TagSendCompletionMap;
        recv_wr_map ReadCompletionMap;

        // keep track of all outgoing sends in a list, protected by a mutex
        active_send_list_type   active_sends;
        mutex_type              active_send_mutex;

        // keep track of all active receive operations in a list, protected by a mutex
        active_recv_list_type   active_recvs;
        mutex_type              active_recv_mutex;

        // a count of all receives, for debugging/performance measurement
        boost::atomic_uint      sends_posted;
        boost::atomic_uint      handled_receives;
        boost::atomic_uint      completions_handled;
        boost::atomic_uint      total_reads;

        // Used to control incoming and outgoing connection requests
        mutex_type              new_connection_mutex;
        condition_type          new_connection_condition;

        // --------------------------------------------------------------------
        // Constructor : mostly just initializes the superclass with 'here'
        // --------------------------------------------------------------------
        parcelport(util::runtime_configuration const& ini,
                util::function_nonser<void(std::size_t, char const*)> const& on_start_thread,
                util::function_nonser<void()> const& on_stop_thread)
              : base_type(ini, here(ini), on_start_thread, on_stop_thread)
              , stopped_(false)
              , active_send_count_(0)
              , immediate_send_allowed_(true)
              , sends_posted(0)
              , handled_receives(0)
              , completions_handled(0)
              , total_reads(0)

        {
            FUNC_START_DEBUG_MSG;
            // we need this for background OS threads to get 'this' pointer
            parcelport::_parcelport_instance = this;
            // port number is set during locality initialization in 'here()'
            _rdmaController = std::make_shared<RdmaController>
                (_ibverbs_device.c_str(), _ibverbs_interface.c_str(), _port);
            //
            FUNC_END_DEBUG_MSG;
        }

        void io_service_work()
        {
            // We only execute work on the IO service while HPX is starting
            while (hpx::is_starting())
            {
                background_work(0);
            }
            LOG_DEBUG_MSG("io service task completed");
        }

        // Start the handling of connections.
        bool do_run()
        {
            FUNC_START_DEBUG_MSG;
            _rdmaController->startup();

            LOG_DEBUG_MSG("Fetching memory pool");
            chunk_pool_ = _rdmaController->getMemoryPool();

            LOG_DEBUG_MSG("Setting ConnectRequest function");
            auto connectRequest_function = std::bind(
                &parcelport::handle_verbs_preconnection, this,
                std::placeholders::_1, std::placeholders::_2);
            _rdmaController->setConnectRequestFunction(connectRequest_function);

            LOG_DEBUG_MSG("Setting Connection function");
            auto connection_function = std::bind(
                &parcelport::handle_verbs_connection, this,
                std::placeholders::_1, std::placeholders::_2);
            _rdmaController->setConnectionFunction(connection_function);

            LOG_DEBUG_MSG("Setting Completion function");
            auto completion_function = std::bind(
                &parcelport::handle_verbs_completion, this,
                std::placeholders::_1, std::placeholders::_2);
            _rdmaController->setCompletionFunction(completion_function);

            for (std::size_t i = 0; i != io_service_pool_.size(); ++i)
            {
                io_service_pool_.get_io_service(int(i)).post(
                    hpx::util::bind(
                        &parcelport::io_service_work, this
                    )
                );
            }

#if HPX_PARCELPORT_VERBS_USE_SPECIALIZED_SCHEDULER
            // initialize our custom scheduler
            parcelport_scheduler.init();

            //
            hpx::error_code ec(hpx::lightweight);
            parcelport_scheduler.register_thread_nullary(
                    util::bind(&parcelport::background_work_scheduler_thread, this),
                    "background_work_scheduler_thread",
                    threads::pending, true, threads::thread_priority_critical,
                    std::size_t(-1), threads::thread_stacksize_default, ec);

            FUNC_END_DEBUG_MSG;
            return ec ? false : true;
#else
            return true;
#endif
       }

        // --------------------------------------------------------------------
        //  return a sender_connection object back to the parcelport_impl
        // --------------------------------------------------------------------
        std::shared_ptr<sender_connection> create_connection(
            parcelset::locality const& dest, error_code& ec)
        {
            FUNC_START_DEBUG_MSG;

            boost::uint32_t dest_ip = dest.get<locality>().ip_;
            LOG_DEBUG_MSG("Create connection from " << ipaddress(_ibv_ip)
                << "to " << ipaddress(dest_ip) );

            RdmaClient *client = get_remote_connection(dest);
            std::shared_ptr<sender_connection> result = std::make_shared<sender_connection>(
                  this
                , dest_ip
                , dest.get<locality>()
                , client
                , chunk_pool_.get()
                , parcels_sent_
            );

            FUNC_END_DEBUG_MSG;
            return result;
        }

        // ----------------------------------------------------------------------------------------------
        // Clean up a completed send and all its regions etc
        // Called when we finish sending a simple message, or when all zero-copy Get operations are done
        // ----------------------------------------------------------------------------------------------
        void delete_send_data(active_send_iterator send) {
            parcel_send_data &send_data = *send;
            // trigger the send complete handler for hpx internal cleanup
            LOG_DEBUG_MSG("Calling write_handler for completed send");
            send_data.handler.operator()(error_code());
            //
            LOG_DEBUG_MSG("deallocating region 1 for completed send "
                << hexpointer(send_data.header_region));
            chunk_pool_->deallocate(send_data.header_region);
            send_data.header_region  = NULL;
            // if this message had multiple (2) SGEs then release other regions
            if (send_data.message_region) {
                LOG_DEBUG_MSG("deallocating region 2 for completed send "
                    << hexpointer(send_data.message_region));
                chunk_pool_->deallocate(send_data.message_region);
                send_data.message_region = NULL;
            }
            for (auto r : send_data.zero_copy_regions) {
                LOG_DEBUG_MSG("Deallocating zero_copy_regions " << hexpointer(r));
                chunk_pool_->deallocate(r);
            }

            {
                LOG_DEBUG_MSG("Taking lock on active_send_mutex in delete_send_data "
                    << hexnumber(active_send_count_) );
                //
                scoped_lock lock(active_send_mutex);
                active_sends.erase(send);
                if (--active_send_count_ < HPX_PARCELPORT_VERBS_SUSPEND_WAKE)
                {
                    if (!immediate_send_allowed_) {
                        LOG_DEVEL_MSG("Enabling immediate send");
                    }
                    immediate_send_allowed_ = true;
                }
                LOG_DEBUG_MSG("Active send after erase size "
                    << hexnumber(active_send_count_) );
            }
        }

        // ----------------------------------------------------------------------------------------------
        // Clean up a completed recv and all its regions etc
        // Called when a parcel_buffer finishes decoding a message
        // ----------------------------------------------------------------------------------------------
        void delete_recv_data(active_recv_iterator recv)
        {
            FUNC_START_DEBUG_MSG;
            parcel_recv_data &recv_data = *recv;
            chunk_pool_->deallocate(recv_data.header_region);
            LOG_DEBUG_MSG("Zero copy regions size is (delete) "
                << decnumber(recv_data.zero_copy_regions.size()));
            for (auto r : recv_data.zero_copy_regions) {
                LOG_DEBUG_MSG("Deallocating " << hexpointer(r));
                chunk_pool_->deallocate(r);
            }
            {
                scoped_lock lock(active_recv_mutex);
                active_recvs.erase(recv);
                LOG_DEBUG_MSG("Active recv after erase size " << hexnumber(active_recvs.size()) );
            }
            ++handled_receives;
        }

        int handle_verbs_preconnection(struct sockaddr_in *addr_src, struct sockaddr_in *addr_dst)
        {
            LOG_DEVEL_MSG("Callback for connect request from "
                << ipaddress(addr_src->sin_addr.s_addr) << "to "
                << ipaddress(addr_dst->sin_addr.s_addr) << "( " << ipaddress(_ibv_ip) << ")");

            // Set a flag to prevent any other connections to this ip address
            auto inserted = connection_requests_.insert(
                std::make_pair(addr_src->sin_addr.s_addr, true));

            // if the flag was already set, we must reject this connection attempt
            if (!inserted.second) {
                LOG_DEVEL_MSG("Connection detection in race from "
                    << ipaddress(addr_src->sin_addr.s_addr) << "to "
                    << ipaddress(addr_dst->sin_addr.s_addr) << "( " << ipaddress(_ibv_ip) << ")");
                if (addr_src->sin_addr.s_addr>addr_dst->sin_addr.s_addr) {
                    LOG_DEVEL_MSG("Reject connection , priority from "
                        << ipaddress(addr_src->sin_addr.s_addr) << "to "
                        << ipaddress(addr_dst->sin_addr.s_addr) << "( " << ipaddress(_ibv_ip) << ")");
                    return 0;
                }
            }
            return 1;
        }

        // ----------------------------------------------------------------------------------------------
        // handler for connections, this is triggered as a callback from the rdmaController when
        // a connection event has occurred.
        // When we connect to another locality, all internal structures are updated accordingly,
        // but when another locality connects to us, we must do it manually via this callback
        // ----------------------------------------------------------------------------------------------
        int handle_verbs_connection(std::pair<uint32_t,uint64_t> qpinfo, RdmaClientPtr client)
        {
            boost::uint32_t dest_ip = client->getRemoteIPv4Address();
            LOG_DEVEL_MSG("Connection callback received from "
                << ipaddress(dest_ip) << "to "
                << ipaddress(_ibv_ip) << "( " << ipaddress(_ibv_ip) << ")");
            ip_map_iterator ip_it = ip_qp_map.find(dest_ip);
            if (ip_it==ip_qp_map.end()) {
                ip_qp_map.insert(std::make_pair(dest_ip, qpinfo.first));
                if (connection_requests_.is_in_map(dest_ip).second) {
                    LOG_DEVEL_MSG("Completed connection request from "
                        << ipaddress(dest_ip) << "to "
                        << ipaddress(_ibv_ip) << "( " << ipaddress(_ibv_ip) << ")");
                    connection_requests_.erase(dest_ip);
                }
                else {
                    LOG_ERROR_MSG("Connection not present in map " << ipaddress(dest_ip));
                    std::terminate();
                }
                LOG_DEBUG_MSG("handle_verbs_connection OK adding " << ipaddress(dest_ip)
                    << "and waking new_connection_condition");
                new_connection_condition.notify_all();
            }
            else {
                throw std::runtime_error("verbs parcelport : should not be receiving a connection more than once");
            }
            return 0;
        }

        // ----------------------------------------------------------------------------------------------
        // When a send completes take one of two actions ...
        // if there are no zero copy chunks, we consider the parcel sending complete
        // so release memory and trigger write_handler.
        // If there are zero copy chunks to be RDMA GET from the remote end, then
        // we hold onto data until they have completed.
        // ----------------------------------------------------------------------------------------------
        void handle_send_completion(uint64_t wr_id)
        {
            bool                 found_wr_id = false;
            active_send_iterator current_send;
            {
                // we must be very careful here.
                // if the lock is not obtained and this thread is waiting, then
                // zero copy Gets might complete and another thread might receive a zero copy
                // complete message then delete the current send data whilst we are still waiting
#if HPX_PARCELPORT_VERBS_USE_CONCURRENT_MAPS
                auto is_present = SendCompletionMap.is_in_map(wr_id);
                if (is_present.second) {
                    found_wr_id = true;
                    current_send = is_present.first->second;
                    SendCompletionMap.erase(is_present.first);
                    LOG_DEBUG_MSG("erasing " << hexpointer(wr_id)
                        << "from SendCompletionMap : size before erase "
                        << SendCompletionMap.size());
                }
#else
                unique_lock lock(SendCompletionMap_mutex);
                auto it = SendCompletionMap.find(wr_id);
                if (it!=SendCompletionMap.end()) {
                    found_wr_id = true;
                    current_send = it->second;
                    SendCompletionMap.erase(it);
                    LOG_DEBUG_MSG("erasing " << hexpointer(wr_id)
                        << "from SendCompletionMap : size before erase "
                        << SendCompletionMap.size());
                }
#endif
                else {
#if HPX_PARCELPORT_VERBS_IMM_UNSUPPORTED
                    lock.unlock();
                    handle_tag_send_completion(wr_id);
                    return;
#else
                    LOG_ERROR_MSG("FATAL : SendCompletionMap did not find "
                        << hexpointer(wr_id));
                    std::terminate();
#endif
                }
            }
            if (found_wr_id) {
                // if the send had no zero_copy regions, then it has completed
                if (!current_send->has_zero_copy) {
                    LOG_DEBUG_MSG("Deleting send data "
                        << hexpointer(&(*current_send)) << "normal");
                    delete_send_data(current_send);
                }
                // if another thread signals to say zero-copy is complete, delete the data
                else if (current_send->delete_flag.test_and_set(std::memory_order_acquire)) {
                    LOG_DEBUG_MSG("Deleting send data "
                        << hexpointer(&(*current_send)) << "after race detection");
                    delete_send_data(current_send);
                }
            }
            else {
                throw std::runtime_error("RDMA Send completed with unmatched Id");
            }
        }

        // ----------------------------------------------------------------------------------------------
        // When a recv completes, take one of two actions ...
        // if there are no zero copy chunks, we consider the parcel receive complete
        // so release memory and trigger write_handler.
        // If there are zero copy chunks to be RDMA GET from the remote end, then
        // we hold onto data until they have completed.
        // ----------------------------------------------------------------------------------------------
        void handle_recv_completion(uint64_t wr_id, RdmaClient* client)
        {
            util::high_resolution_timer timer;

            // bookkeeping : decrement counter that keeps preposted queue full
            client->pop_receive_count();
            _rdmaController->refill_client_receives();

            // store details about this parcel so that all memory buffers can be kept
            // until all recv operations have completed.
            active_recv_iterator current_recv;
            {
                scoped_lock lock(active_recv_mutex);
                active_recvs.emplace_back();
                current_recv = std::prev(active_recvs.end());
                LOG_DEBUG_MSG("Active recv after insert size "
                    << hexnumber(active_recvs.size()));
            }
            parcel_recv_data &recv_data = *current_recv;
            // get the header of the new message/parcel
            recv_data.header_region  = (rdma_memory_region *)wr_id;
            header_type *h = (header_type*)recv_data.header_region->get_address();
            recv_data.message_region = NULL;
            // zero copy chunks we have to GET from the source locality
            if (h->piggy_back()) {
              recv_data.rdma_count        = h->num_chunks().first;
            }
            else {
                recv_data.rdma_count      = 1 + h->num_chunks().first;
            }
            // each parcel has a unique tag which we use to organize
            // zero-copy data if we need any
            recv_data.tag            = h->tag();
            //
            LOG_DEBUG_MSG( "received IBV_WC_RECV " <<
                    "buffsize " << decnumber(h->size())
                    << "chunks zerocopy( " << decnumber(h->num_chunks().first) << ") "
                    << ", chunk_flag " << decnumber(h->header_length())
                    << ", normal( " << decnumber(h->num_chunks().second) << ") "
                    << " chunkdata " << decnumber((h->chunk_data()!=NULL))
                    << " piggyback " << decnumber((h->piggy_back()!=NULL))
                    << " tag " << hexuint32(h->tag())
                    << " total receives " << decnumber(handled_receives)
            );

            // setting this flag to false - if more data is needed -
            // disables final parcel receive call
            bool parcel_complete = true;

            // if message was not piggybacked
            char *piggy_back = h->piggy_back();
            char *chunk_data = h->chunk_data();
            if (chunk_data) {
                // all the info about chunks we need is stored inside the header
                recv_data.chunks.resize(h->num_chunks().first + h->num_chunks().second);
                size_t chunkbytes = recv_data.chunks.size() * sizeof(serialization::serialization_chunk);
                std::memcpy(recv_data.chunks.data(), chunk_data, chunkbytes);
                LOG_DEBUG_MSG("Copied chunk data from header : size " << decnumber(chunkbytes));

                // setup info for zero-copy rdma get chunks (if there are any)
                if (recv_data.rdma_count>0) {
                    parcel_complete = false;
                    int index = 0;
                    LOG_EXCLUSIVE(
                    for (const serialization::serialization_chunk &c : recv_data.chunks) {
                        LOG_DEBUG_MSG("recv : chunk : size " << hexnumber(c.size_)
                                << " type " << decnumber((uint64_t)c.type_)
                                << " rkey " << decnumber(c.rkey_)
                                << " cpos " << hexpointer(c.data_.cpos_)
                                << " pos " << hexpointer(c.data_.pos_)
                                << " index " << decnumber(c.data_.index_));
                    })
                    for (serialization::serialization_chunk &c : recv_data.chunks) {
                        if (c.type_ == serialization::chunk_type_pointer) {
                            rdma_memory_region *get_region = chunk_pool_->allocate_region(c.size_);
                            LOG_DEBUG_MSG("RDMA Get address " << hexpointer(c.data_.cpos_)
                                    << " rkey " << decnumber(c.rkey_) << " size " << hexnumber(c.size_)
                                    << " tag " << hexuint32(recv_data.tag)
                                    << " local address " << get_region->get_address() << " length " << c.size_);
                            recv_data.zero_copy_regions.push_back(get_region);
                            LOG_DEBUG_MSG("Zero copy regions size is (create) "
                                << decnumber(recv_data.zero_copy_regions.size()));
                            // put region into map before posting read in case it
                            // completes whilst this thread is suspended
                            {
#if HPX_PARCELPORT_VERBS_USE_CONCURRENT_MAPS
                                ReadCompletionMap.insert(std::make_pair((uint64_t)get_region, current_recv));
#else
                                scoped_lock lock(ReadCompletionMap_mutex);
                                ReadCompletionMap.insert(std::make_pair((uint64_t)get_region, current_recv));
#endif
                            }
                            // overwrite the serialization data to account for the local pointers instead of remote ones
                            /// post the rdma read/get
                            const void *remoteAddr = c.data_.cpos_;
                            recv_data.chunks[index] = hpx::serialization::create_pointer_chunk(get_region->get_address(), c.size_, c.rkey_);
                            ++total_reads;
                            client->post_read(get_region, c.rkey_, remoteAddr, c.size_);
                        }
                        index++;
                    }
                }
            }
            else {
                // no chunk information was sent in this message, so we must GET it
                parcel_complete = false;

                LOG_ERROR_MSG("@TODO implement RDMA GET of mass chunk information when header too small");
                std::terminate();
                throw std::runtime_error("@TODO implement RDMA GET of mass chunk information when header too small");
            }

            LOG_DEBUG_MSG("piggy_back is " << hexpointer(piggy_back) << " chunk data is " << hexpointer(h->chunk_data()));
            // if the main serialization chunk is piggybacked in second SGE
            if (piggy_back) {
                if (parcel_complete) {
                    rcv_data_type wrapped_pointer(piggy_back, h->size(),
                        boost::bind(&parcelport::delete_recv_data, this, current_recv), chunk_pool_.get(), NULL);
                    rcv_buffer_type buffer(std::move(wrapped_pointer), chunk_pool_.get());
                    LOG_DEBUG_MSG("calling parcel decode for complete NORMAL parcel");
                    hpx::parcelset::decode_message_with_chunks<parcelport, rcv_buffer_type>
                        (*this, std::move(buffer), 0, recv_data.chunks);
                    LOG_DEBUG_MSG("parcel decode called for complete NORMAL parcel");
                }
            }
            else {
                std::size_t size = h->get_message_rdma_size();
                rdma_memory_region *get_region = chunk_pool_->allocate_region(size);
                get_region->set_message_length(size);
                recv_data.zero_copy_regions.push_back(get_region);
                // put region into map before posting read in case it completes whilst this thread is suspended
                {

#if HPX_PARCELPORT_VERBS_USE_CONCURRENT_MAPS
                    ReadCompletionMap.insert(std::make_pair((uint64_t)get_region, current_recv));
#else
                    scoped_lock lock(ReadCompletionMap_mutex);
                    ReadCompletionMap.insert(std::make_pair((uint64_t)get_region, current_recv));
#endif
                }
                const void *remoteAddr = h->get_message_rdma_addr();
                LOG_DEBUG_MSG("@TODO Pushing back an extra chunk description");
                recv_data.chunks.push_back(
                    hpx::serialization::create_pointer_chunk(get_region->get_address(),
                        size, h->get_message_rdma_key()));
                ++total_reads;
                client->post_read(get_region, h->get_message_rdma_key(), remoteAddr,
                    h->get_message_rdma_size());
            }

            // @TODO replace performance counter data
            //          performance_counters::parcels::data_point& data = buffer.data_point_;
            //          data.time_ = timer.elapsed_nanoseconds();
            //          data.bytes_ = static_cast<std::size_t>(buffer.size_);
            //          ...
            //          data.time_ = timer.elapsed_nanoseconds() - data.time_;
        }

#if HPX_PARCELPORT_VERBS_IMM_UNSUPPORTED
        void handle_tag_send_completion(uint64_t wr_id)
        {
            LOG_DEBUG_MSG("Handle 4 byte completion" << hexpointer(wr_id));
            rdma_memory_region *region = (rdma_memory_region *)wr_id;
            uint32_t tag = *(uint32_t*) (region->get_address());
            chunk_pool_->deallocate(region);
            LOG_DEBUG_MSG("Cleaned up from 4 byte ack message with tag " << hexuint32(tag));
        }
#endif

        void handle_tag_recv_completion(uint64_t wr_id, uint32_t tag, const RdmaClient *client)
        {
#if HPX_PARCELPORT_VERBS_IMM_UNSUPPORTED
            rdma_memory_region *region = (rdma_memory_region *)wr_id;
            tag = *((uint32_t*) (region->get_address()));
            LOG_DEBUG_MSG("Received 4 byte ack message with tag " << hexuint32(tag));
#else
            rdma_memory_region *region = (rdma_memory_region *)wr_id;
            LOG_DEBUG_MSG("Received 0 byte ack message with tag " << hexuint32(tag));
#endif
            // bookkeeping : decrement counter that keeps preposted queue full
            client->pop_receive_count();

            // let go of this region (waste really as this was a zero byte message)
            chunk_pool_->deallocate(region);

            // now release any zero copy regions we were holding until parcel complete
            active_send_iterator current_send;
            {
#if HPX_PARCELPORT_VERBS_USE_CONCURRENT_MAPS
                auto is_present = TagSendCompletionMap.is_in_map(tag);
                if (is_present.second) {
                    current_send = is_present.first->second;
                    TagSendCompletionMap.erase(is_present.first);
                }
#else
                scoped_lock lock(TagSendCompletionMap_mutex);
                send_wr_map::iterator it = TagSendCompletionMap.find(tag);
                if (it!=TagSendCompletionMap.end()) {
                    current_send = it->second;
                    TagSendCompletionMap.erase(it);
                }
#endif
                else {
                    LOG_ERROR_MSG("Tag not present in Send map, FATAL");
                    std::terminate();
                }
           }
            // we cannot delete the send data until we are absolutely sure that
            // the initial send has been cleaned up
            if (current_send->delete_flag.test_and_set(std::memory_order_acquire)) {
                LOG_DEBUG_MSG("Deleting send data " << hexpointer(&(*current_send)) << "with no race detection");
                delete_send_data(current_send);
            }
            //
            ++handled_receives;

            LOG_DEBUG_MSG( "received IBV_WC_RECV handle_tag_recv_completion "
                    << " total receives " << decnumber(handled_receives)
            );

        }

        void handle_rdma_get_completion(uint64_t wr_id, RdmaClient *client)
        {
            bool                 found_wr_id;
            active_recv_iterator current_recv;
            {
#if HPX_PARCELPORT_VERBS_USE_CONCURRENT_MAPS
                auto is_present = ReadCompletionMap.is_in_map(wr_id);
                if (is_present.second) {
                    found_wr_id = true;
                    current_recv = is_present.first->second;
                    LOG_DEBUG_MSG("erasing " << hexpointer(wr_id)
                        << "from ReadCompletionMap : size before erase "
                        << ReadCompletionMap.size());
                    ReadCompletionMap.erase(is_present.first);
                }
#else
                scoped_lock lock(ReadCompletionMap_mutex);
                auto it = ReadCompletionMap.find(wr_id);
                if (it!=ReadCompletionMap.end()) {
                    found_wr_id = true;
                    current_recv = it->second;
                    LOG_DEBUG_MSG("erasing " << hexpointer(wr_id)
                        << "from ReadCompletionMap : size before erase "
                        << ReadCompletionMap.size());
                    ReadCompletionMap.erase(it);
                }
#endif
                else {
                    LOG_ERROR_MSG("Fatal error as wr_id is not in completion map");
                    std::terminate();
                }
            }
            if (found_wr_id) {
                parcel_recv_data &recv_data = *current_recv;
                LOG_DEBUG_MSG("RDMA Get tag " << hexuint32(recv_data.tag) << " has count of " << decnumber(recv_data.rdma_count));
                if (--recv_data.rdma_count > 0) {
                    // we can't do anything until all zero copy chunks are here
                    return;
                }
                //
#if HPX_PARCELPORT_VERBS_IMM_UNSUPPORTED
                LOG_DEBUG_MSG("RDMA Get tag " << hexuint32(recv_data.tag) << " has completed : posting 4 byte ack to origin");
                rdma_memory_region *tag_region = chunk_pool_->allocate_region(4); // space for a uint32_t
                uint32_t *tag_memory = (uint32_t*)(tag_region->get_address());
                *tag_memory = recv_data.tag;
                tag_region->set_message_length(4);
                client->post_send(tag_region, true, false, 0);
#else
                LOG_DEBUG_MSG("RDMA Get tag " << hexuint32(recv_data.tag)
                    << " has completed : posting zero byte ack to origin");
                // convert uint32 to uint64 so we can use it as a fake message region
                // (wr_id only for 0 byte send)
                uint64_t fake_region = recv_data.tag;
                client->post_send_x0((rdma_memory_region*)fake_region,
                    false, true, recv_data.tag);
#endif
                //
                LOG_DEBUG_MSG("Zero copy regions size is (completion) "
                    << decnumber(recv_data.zero_copy_regions.size()));

                header_type *h = (header_type*)recv_data.header_region->get_address();
                LOG_DEBUG_MSG( "get completion " <<
                        "buffsize " << decnumber(h->size())
                        << "chunks zerocopy( " << decnumber(h->num_chunks().first) << ") "
                        << ", chunk_flag " << decnumber(h->header_length())
                        << ", normal( " << decnumber(h->num_chunks().second) << ") "
                        << " chunkdata " << decnumber((h->chunk_data()!=NULL))
                        << " piggyback " << decnumber((h->piggy_back()!=NULL))
                        << " tag " << hexuint32(h->tag())
                );

                std::size_t message_length;
                char *message = h->piggy_back();
                if (message) {
                    message_length = h->size();
                }
                else {
                    rdma_memory_region *message_region = recv_data.zero_copy_regions.back();
                    recv_data.zero_copy_regions.resize(recv_data.zero_copy_regions.size()-1);
                    message = static_cast<char *>(message_region->get_address());
                    message_length = message_region->get_message_length();
                    LOG_DEBUG_MSG("No piggy_back message, RDMA GET : " << hexpointer(message_region) << " length " decnumber(message_length));
                    LOG_DEBUG_MSG("No piggy_back message, RDMA GET : " << hexpointer(recv_data.message_region) << " length " decnumber(message_length));
                }

                LOG_DEBUG_MSG("Creating a release buffer callback for tag " << hexuint32(recv_data.tag));
                rcv_data_type wrapped_pointer(message, message_length,
                        boost::bind(&parcelport::delete_recv_data, this, current_recv),
                        chunk_pool_.get(), NULL);
                rcv_buffer_type buffer(std::move(wrapped_pointer), chunk_pool_.get());
                LOG_DEBUG_MSG("calling parcel decode for complete ZEROCOPY parcel");

                LOG_EXCLUSIVE(
                for (serialization::serialization_chunk &c : recv_data.chunks) {
                    LOG_DEBUG_MSG("get : chunk : size " << hexnumber(c.size_)
                            << " type " << decnumber((uint64_t)c.type_)
                            << " rkey " << decnumber(c.rkey_)
                            << " cpos " << hexpointer(c.data_.cpos_)
                            << " pos " << hexpointer(c.data_.pos_)
                            << " index " << decnumber(c.data_.index_));
                })

                buffer.num_chunks_ = h->num_chunks();
                //buffer.data_.resize(static_cast<std::size_t>(h->size()));
                //buffer.data_size_ = h->size();
                buffer.chunks_.resize(recv_data.chunks.size());
                decode_message_with_chunks(*this, std::move(buffer), 0, recv_data.chunks);
                LOG_DEBUG_MSG("parcel decode called for ZEROCOPY complete parcel");
            }
            else {
                throw std::runtime_error("RDMA Send completed with unmatched Id");
            }
        }

        // ----------------------------------------------------------------------------------------------
        // Every (signalled) rdma operation triggers a completion event when it completes,
        // the rdmaController calls this callback function and we must clean up all temporary
        // memory etc and signal hpx when sends or receives finish.
        // ----------------------------------------------------------------------------------------------
        int handle_verbs_completion(const struct ibv_wc completion, RdmaClient *client)
        {
            uint64_t wr_id = completion.wr_id;
            LOG_DEBUG_MSG("Handle verbs completion " << hexpointer(wr_id) << RdmaCompletionQueue::wc_opcode_str(completion.opcode));

            completions_handled++;
/*
            _rdmaController->for_each_client(
                [](std::pair<uint32_t, RdmaClientPtr> clientpair)
                {
                RdmaClient* client = clientpair.second.get();
                LOG_TIMED_INIT(clientlog);
                LOG_TIMED_MSG(clientlog, DEVEL, 0.1,
                    "internal reported, \n"
                    << "recv " << decnumber(client->get_total_posted_recv_count()) << "\n"
                    << "send " << decnumber(client->get_total_posted_send_count()) << "\n"
                    << "read " << decnumber(client->get_total_posted_read_count()) << "\n"
                );
                }
            );

            LOG_TIMED_INIT(background);
            LOG_TIMED_MSG(background, DEVEL, 0.1,
                "PP reported\n"
                << "actv " << decnumber(active_send_count_) << "\n"
                << "recv " << decnumber(handled_receives) << "\n"
                << "send " << decnumber(sends_posted) << "\n"
                << "read " << decnumber(total_reads) << "\n"
                << "Total completions " << decnumber(completions_handled)
                << decnumber(sends_posted+handled_receives+total_reads));
*/


            if (completion.opcode==IBV_WC_SEND) {
                LOG_DEBUG_MSG("Handle general completion " << hexpointer(wr_id) << RdmaCompletionQueue::wc_opcode_str(completion.opcode));
                handle_send_completion(wr_id);
                return 0;
            }

            //
            // When an Rdma Get operation completes, either add it to an ongoing parcel
            // receive, or if it is the last one, trigger decode message
            //
            else if (completion.opcode==IBV_WC_RDMA_READ) {
                handle_rdma_get_completion(wr_id, client);
                return 0;
            }

            //
            // A zero byte receive indicates we are being informed that remote GET operations are complete
            // we can release any data we were holding onto and signal a send as finished.
            // On hardware that does not support immediate data, a 4 byte tag message is used.
            //

            else if (completion.opcode==IBV_WC_RECV && completion.byte_len<=4) {
                handle_tag_recv_completion(wr_id, completion.imm_data, client);
                return 0;
            }

            //
            // When an unmatched receive completes, it is a new parcel, if everything fits into
            // the header, call decode message, otherwise, queue all the Rdma Get operations
            // necessary to complete the message
            //
            else if (completion.opcode==IBV_WC_RECV) {
                LOG_DEBUG_MSG("Entering receive (completion handler) section with received size " << decnumber(completion.byte_len));
                handle_recv_completion(wr_id, client);
                return 0;
            }

            throw std::runtime_error("Unexpected opcode in handle_verbs_completion "
                + rdma_sender_receiver::
                ibv_wc_opcode_string((ibv_wr_opcode)(completion.opcode)));
            return 0;
        }

        ~parcelport() {
            FUNC_START_DEBUG_MSG;
            _rdmaController = nullptr;
            FUNC_END_DEBUG_MSG;
        }

        /// Should not be used any more as parcelport_impl handles this
        bool can_bootstrap() const {
            FUNC_START_DEBUG_MSG;
            FUNC_END_DEBUG_MSG;
            return HPX_PARCELPORT_VERBS_ENABLE_BOOTSTRAP();
        }

        /// Return the name of this locality
        std::string get_locality_name() const {
            FUNC_START_DEBUG_MSG;

            FUNC_END_DEBUG_MSG;
            // return ip address + ?
            return "verbs";
        }

        parcelset::locality agas_locality(util::runtime_configuration const & ini) const
        {
            FUNC_START_DEBUG_MSG;
            // load all components as described in the configuration information
            if (ini.has_section("hpx.agas")) {
                util::section const* sec = ini.get_section("hpx.agas");
                if (nullptr != sec) {
                    struct in_addr buf;
                    std::string addr = sec->get_entry("address", HPX_INITIAL_IP_ADDRESS);
                    LOG_DEBUG_MSG("Got AGAS addr " << addr.c_str();)
                    inet_pton(AF_INET, &addr[0], &buf);
                    return
                        parcelset::locality(locality(buf.s_addr));
                }
            }
            FUNC_END_DEBUG_MSG;
            LOG_DEBUG_MSG("Returning NULL agas locality")
            return parcelset::locality(locality(0xFFFF));
        }

        parcelset::locality create_locality() const {
            FUNC_START_DEBUG_MSG;
            FUNC_END_DEBUG_MSG;
            return parcelset::locality(locality());
        }

        static void suspended_task_debug(const std::string &match)
        {
            std::string temp = hpx::util::debug::suspended_task_backtraces();
            if (match.size()==0 ||
                temp.find(match)!=std::string::npos)
            {
                LOG_DEVEL_MSG("Suspended threads " << temp);
            }
        }

        void do_stop() {
            LOG_DEBUG_MSG("Entering verbs stop ");
            FUNC_START_DEBUG_MSG;
            if (!stopped_) {
                bool finished = false;
                do {
                    finished = active_sends.empty() && active_recvs.empty();
                    if (!finished) {
                        LOG_ERROR_MSG("Entering STOP when not all parcels have completed");
                        std::terminate();
                    }
                } while (!finished && !hpx::is_stopped());

                scoped_lock lock(stop_mutex);
                LOG_DEBUG_MSG("Removing all initiated connections");
                _rdmaController->removeAllInitiatedConnections();

                // wait for all clients initiated elsewhere to be disconnected
                while (_rdmaController->num_clients()!=0 && !hpx::is_stopped()) {
                    _rdmaController->eventMonitor(0);
                    std::cout << "Polling before shutdown" << std::endl;
                }
                LOG_DEBUG_MSG("stopped removing clients and terminating");
            }
            stopped_ = true;
            // Stop receiving and sending of parcels
        }

        // ----------------------------------------------------------------------------------------------
        // find the client queue pair object that is at the destination ip address
        // if no connection has been made yet, make one.
        // ----------------------------------------------------------------------------------------------
        RdmaClient *get_remote_connection(parcelset::locality const& dest)
        {
            boost::uint32_t dest_ip = dest.get<locality>().ip_;
            // @TODO, don't need smartpointers here, remove them as they waste an atomic refcount
            RdmaClientPtr client;
            {
                // if a connection exists to this destination, get it
                ip_map_iterator ip_it = ip_qp_map.find(dest_ip);
                if (ip_it!=ip_qp_map.end()) {
                    LOG_TRACE_MSG("Client found connection made from "
                        << ipaddress(_ibv_ip) << "to " << ipaddress(dest_ip)
                        << "with QP " << ip_it->second);
                    client = _rdmaController->getClient(ip_it->second);
                    return client.get();
                }
                // Didn't find a connection. We must create a new one
                else {
                    // set a flag to prevent more connections to the same ip address
                    auto inserted =
                        connection_requests_.insert(std::make_pair(dest_ip, true));

                    // if a connection to this dest has already been started
                    if (inserted.second==false) {
                        LOG_DEVEL_MSG("Connection race (in request) from "
                            << ipaddress(_ibv_ip) << "to " << ipaddress(dest_ip)
                            << "( " << ipaddress(_ibv_ip) << ")");

                        // wait until a connection has been established
                        LOG_DEVEL_MSG("Going to wait/sleep on connection mutex");
                        unique_lock lock(new_connection_mutex);
                        new_connection_condition.wait(lock, [&,this] {
                            return ip_qp_map.find(dest_ip)!=ip_qp_map.end();
                        });
                        LOG_DEVEL_MSG("Waking up for the connection from "
                            << ipaddress(_ibv_ip) << "to " << ipaddress(dest_ip)
                            << "( " << ipaddress(_ibv_ip) << ")");

                        ip_it = ip_qp_map.find(dest_ip);
                        if (ip_it==ip_qp_map.end()) {
                            LOG_DEVEL_MSG("After wake, did not find connection from "
                                << ipaddress(_ibv_ip) << "to " << ipaddress(dest_ip)
                                << "( " << ipaddress(_ibv_ip) << ")");
                            std::terminate();
                        }
                        client = _rdmaController->getClient(ip_it->second);
                        return client.get();
                    }
                    // start a new connection
                    else {
                        LOG_DEVEL_MSG("Starting new connect request from "
                            << ipaddress(_ibv_ip) << "to " << ipaddress(dest_ip)
                            << "( " << ipaddress(_ibv_ip) << ")");
                        client = _rdmaController->makeServerToServerConnection(
                            dest_ip, _rdmaController->getPort(),
                            _rdmaController->getServer()->getContext());
                        if (client) {
                            LOG_DEVEL_MSG("Setting qpnum in main client map "
                                << client->getQpNum());
                            ip_qp_map.insert(std::make_pair(dest_ip, client->getQpNum()));
                            new_connection_condition.notify_all();
                            return client.get();
                        }
                        else {
                            LOG_DEVEL_MSG("Failed new server connection from "
                                << ipaddress(_ibv_ip) << "to " << ipaddress(dest_ip)
                                << "( " << ipaddress(_ibv_ip) << ")");
                        }
                    }
                }
            }
            return NULL;
        }

        bool can_send_immediate() {
            while (!immediate_send_allowed_) {
                background_work(0);
            }
            return true;
        }

        template <typename Handler>
        bool async_write(Handler && handler, sender_connection *sender, snd_buffer_type &buffer)
        {
            FUNC_START_DEBUG_MSG;
            // if the serialization overflows the block, panic and rewrite this.
            {
                // create a tag, needs to be unique per client
                uint32_t tag = tag_provider_.next(sender->dest_ip_);
                LOG_DEBUG_MSG("Generated tag " << hexuint32(tag) << " from "
                    << hexuint32(sender->dest_ip_));

                // we must store details about this parcel so that all memory buffers can be kept
                // until all send operations have completed.
                active_send_iterator current_send;
                {
                    LOG_DEBUG_MSG("Taking lock on active_send_mutex in async_write "
                        << hexnumber(active_send_count_) );
                    //
                    scoped_lock lock(active_send_mutex);
                    active_sends.emplace_back();
                    current_send = std::prev(active_sends.end());
                    if (++active_send_count_ >= HPX_PARCELPORT_VERBS_MAX_SEND_QUEUE) {
                        if (immediate_send_allowed_) {
                            LOG_DEVEL_MSG("Disabling immediate send");
                        }
                        immediate_send_allowed_ = false;
                    }
                    LOG_DEBUG_MSG("Active send after insert size "
                        << hexnumber(active_send_count_));
                }
                parcel_send_data &send_data = *current_send;
                send_data.tag            = tag;
                send_data.handler        = std::move(handler);
                send_data.header_region  = NULL;
                send_data.message_region = NULL;
                send_data.has_zero_copy  = false;
                send_data.delete_flag.clear();

                LOG_DEBUG_MSG("Generated unique dest " << hexnumber(sender->dest_ip_)
                    << " coded tag " << hexuint32(send_data.tag));

                // for each zerocopy chunk, we must create a memory region for the data
                LOG_EXCLUSIVE(
                    for (serialization::serialization_chunk &c : buffer.chunks_) {
                    LOG_DEBUG_MSG("write : chunk : size " << hexnumber(c.size_)
                            << " type " << decnumber((uint64_t)c.type_)
                            << " rkey " << decnumber(c.rkey_)
                            << " cpos " << hexpointer(c.data_.cpos_)
                            << " pos " << hexpointer(c.data_.pos_)
                            << " index " << decnumber(c.data_.index_));
                })

                // for each zerocopy chunk, we must create a memory region for the data
                int index = 0;
                for (serialization::serialization_chunk &c : buffer.chunks_) {
                    if (c.type_ == serialization::chunk_type_pointer) {
                        send_data.has_zero_copy  = true;
                        // if the data chunk fits into a memory block, copy it
//                        util::high_resolution_timer regtimer;
                        rdma_memory_region *zero_copy_region;
                        if (c.size_<=HPX_PARCELPORT_VERBS_MEMORY_COPY_THRESHOLD) {
                            zero_copy_region = chunk_pool_->allocate_region(std::max(c.size_, (std::size_t)RDMA_POOL_SMALL_CHUNK_SIZE));
                            char *zero_copy_memory = (char*)(zero_copy_region->get_address());
                            std::memcpy(zero_copy_memory, c.data_.cpos_, c.size_);
                            // the pointer in the chunk info must be changed
                            buffer.chunks_[index] = serialization::create_pointer_chunk(zero_copy_memory, c.size_);
//                            LOG_DEBUG_MSG("Time to copy memory (ns) " << decnumber(regtimer.elapsed_nanoseconds()));
                        }
                        else {
                            // create a memory region from the pointer
                            zero_copy_region = new rdma_memory_region(
                                    _rdmaController->getProtectionDomain(), c.data_.cpos_, std::max(c.size_, (std::size_t)RDMA_POOL_SMALL_CHUNK_SIZE));
//                            LOG_DEBUG_MSG("Time to register memory (ns) " << decnumber(regtimer.elapsed_nanoseconds()));
                        }
                        c.rkey_  = zero_copy_region->get_remote_key();
                        LOG_DEBUG_MSG("Zero-copy rdma Get region " << decnumber(index) << " created for address "
                                << hexpointer(zero_copy_region->get_address())
                                << " and rkey " << decnumber(c.rkey_));
                        send_data.zero_copy_regions.push_back(zero_copy_region);
                    }
                    index++;
                }

                // grab a memory block from the pinned pool to use for the header
                send_data.header_region = chunk_pool_->allocate_region(chunk_pool_->small_.chunk_size_);
                char *header_memory = (char*)(send_data.header_region->get_address());

                // create the header in the pinned memory block
                LOG_DEBUG_MSG("Placement new for the header with piggyback copy disabled");
                header_type *h = new(header_memory) header_type(buffer, send_data.tag);
                send_data.header_region->set_message_length(h->header_length());

                LOG_DEBUG_MSG(
                        "sending, buffsize " << decnumber(h->size())
                        << "header_length " << decnumber(h->header_length())
                        << "chunks zerocopy( " << decnumber(h->num_chunks().first) << ") "
                        << ", chunk_flag " << decnumber(h->header_length())
                        << ", normal( " << decnumber(h->num_chunks().second) << ") "
                        << ", chunk_flag " << decnumber(h->header_length())
                        << "tag " << hexuint32(h->tag())
                );

                // Get the block of pinned memory where the message was encoded during serialization
                send_data.message_region = buffer.data_.m_region_;
                send_data.message_region->set_message_length(h->size());
                LOG_DEBUG_MSG("Found region allocated during encode_parcel : address " << hexpointer(buffer.data_.m_array_) << " region "<< hexpointer(send_data.message_region));

                // header region is always sent, message region is usually piggybacked
                int num_regions = 1;
                rdma_memory_region *region_list[2] = { send_data.header_region, send_data.message_region };
                if (h->chunk_data()) {
                    LOG_DEBUG_MSG("Chunk info is piggybacked");
                }
                else {
                    throw std::runtime_error(
                            "@TODO : implement chunk info rdma get when zero-copy chunks exceed header space");
                }

                if (h->piggy_back()) {
                    LOG_DEBUG_MSG("Main message is piggybacked");
                    num_regions += 1;
                }
                else {
                    LOG_DEBUG_MSG("Main message NOT piggybacked ");
                    h->set_message_rdma_size(h->size());
                    h->set_message_rdma_key(send_data.message_region->get_local_key());
                    h->set_message_rdma_addr(send_data.message_region->get_address());
                    send_data.zero_copy_regions.push_back(send_data.message_region);
                    send_data.has_zero_copy  = true;
                    LOG_DEBUG_MSG("RDMA message " << hexnumber(buffer.data_.size())
                            << " rkey " << decnumber(send_data.message_region->get_local_key())
                            << " pos " << hexpointer(send_data.message_region->get_address()));
                    // do not delete twice, clear the message block pointer as it
                    // is also used in the zero_copy_regions list
                    send_data.message_region = NULL;
                }

                uint64_t wr_id = (uint64_t)(send_data.header_region);
                {
                    // add wr_id's to completion map
                    // put everything into map to be retrieved when send completes
#if HPX_PARCELPORT_VERBS_USE_CONCURRENT_MAPS
#else
                    scoped_lock lock(SendCompletionMap_mutex);
#endif
                    SendCompletionMap.insert(std::make_pair(wr_id, current_send));
                    LOG_DEBUG_MSG("wr_id added to SendCompletionMap "
                            << hexpointer(wr_id) << " Entries " << SendCompletionMap.size());
                }
                {
                    // if there are zero copy regions (or message/chunks are not piggybacked),
                    // we must hold onto the regions until the destination tells us
                    // it has completed all rdma Get operations
                    if (send_data.has_zero_copy) {
#if HPX_PARCELPORT_VERBS_USE_CONCURRENT_MAPS
#else
                        scoped_lock lock(TagSendCompletionMap_mutex);
#endif
                        // put the data into a new map which is indexed by the Tag of the send
                        // zero copy blocks will be released when we are told this has completed
                        TagSendCompletionMap.insert(std::make_pair(send_data.tag, current_send));
                    }
                }

                // send the header/main_chunk to the destination, wr_id is header_region (entry 0 in region_list)
                LOG_DEBUG_MSG("num regions to send " << num_regions
                    << "Block header_region"
                    << " buffer "    << hexpointer(send_data.header_region->get_address())
                    << " region "    << hexpointer(send_data.header_region));
                if (num_regions>1) {
                    LOG_TRACE_MSG(
                    "Block message_region "
                    << " buffer "    << hexpointer(send_data.message_region->get_address())
                    << " region "    << hexpointer(send_data.message_region));
                }
                ++sends_posted;
                sender->client_->post_send_xN(region_list, num_regions, true, false, 0);

                // log the time spent in performance counter
//                buffer.data_point_.time_ =
//                        timer.elapsed_nanoseconds() - buffer.data_point_.time_;

                // parcels_posted_.add_data(buffer.data_point_);
            }
            FUNC_END_DEBUG_MSG;
            return true;
        }

        // ----------------------------------------------------------------------------------------------
        // This is called to poll for completions and handle all incoming messages as well as complete
        // outgoing messages.
        //
        // @TODO : It is assumed that hpx_bakground_work is only going to be called from
        // an hpx thread.
        // Since the parcelport can be serviced by hpx threads or by OS threads, we must use extra
        // care when dealing with mutexes and condition_variables since we do not want to suspend an
        // OS thread, but we do want to suspend hpx threads when necessary.
        // ----------------------------------------------------------------------------------------------
        inline bool background_work_OS_thread() {
            bool done = false;
            do {
                // if an event comes in, we may spend time processing/handling it
                // and another may arrive during this handling,
                // so keep checking until none are received
                _rdmaController->refill_client_receives();
                done = (_rdmaController->eventMonitor(0) == 0);
            } while (!done);
            return true;
        }

        inline bool background_work_hpx_thread() {
            // this must be called on an HPX thread
            HPX_ASSERT(threads::get_self_ptr() != nullptr);
            //
            return background_work_OS_thread();
        }


        // --------------------------------------------------------------------
        // Background work, HPX thread version, used on custom scheduler
        // to poll in an OS background thread which is pre-emtive and therefore
        // could help reduce latencies (when hpx threads are all doing work).
        // --------------------------------------------------------------------
#if HPX_PARCELPORT_VERBS_USE_SPECIALIZED_SCHEDULER
        void background_work_scheduler_thread()
        {
            // repeat until no more parcels are to be sent
            while (!stopped_ || !hpx::is_stopped()) {
                background_work_hpx_thread();
            }
            LOG_DEBUG_MSG("hpx background work thread stopped");
        }
#endif

        // --------------------------------------------------------------------
        // Background work
        //
        // This is called whenever the main thread scheduler is idling,
        // is used to poll for events, messages on the verbs connection
        // --------------------------------------------------------------------

        bool background_work(std::size_t num_thread) {
            if (stopped_ || hpx::is_stopped()) {
                return false;
            }
            LOG_TIMED_INIT(background_work);
            LOG_TIMED_MSG(background_work, DEVEL, 5.0,
                "active_send_count_ " << decnumber(active_send_count_)
                << "active_send_size " << decnumber(active_sends.size())
                << "Total sends " << decnumber(sends_posted)
                << "Total recvs " << decnumber(handled_receives)
                << "Total reads " << decnumber(total_reads)
                << "Total completions " << decnumber(completions_handled)
                << decnumber(sends_posted+handled_receives+total_reads));

            LOG_TIMED_INIT(background_sends);
            LOG_TIMED_BLOCK(background_sends, DEVEL, 5.0,
                for (const auto & as : active_sends) {
                    LOG_DEVEL_MSG("active_send "
                        << ", tag "     << hexnumber(as.tag)
                        << ", ZC "      << as.has_zero_copy
                        << ", ZC size " << as.zero_copy_regions.size());
                }
                for (const auto & as : active_recvs) {
                    LOG_DEVEL_MSG("active_recvs "
                        << ", tag "         << hexnumber(as.tag)
                        << ", rdma_count "  << as.rdma_count
                        << ", chunks "      << as.chunks.size()
                        << ", ZC size "     << as.zero_copy_regions.size());
                }
//                suspended_task_debug("");
            )

            if (threads::get_self_ptr() == nullptr) {
                // this is an OS thread, we are still not 100% sure our code is
                // safe on an OS thread, call this function to skip checks
                return background_work_OS_thread();
            }
            return background_work_hpx_thread();
        }

        static parcelport *get_singleton()
        {
            return _parcelport_instance;
        }

        static parcelport *_parcelport_instance;

    };

    // --------------------------------------------------------------------
    // sender_connection functions
    //
    // could not be included in sender_connection.hpp because they
    // need the full definition of the verbs PP to come first
    // --------------------------------------------------------------------
    template <typename Handler, typename ParcelPostprocess>
    void sender_connection::async_write(Handler && handler,
        ParcelPostprocess && parcel_postprocess)
    {
        HPX_ASSERT(!buffer_.data_.empty());
        //
        postprocess_handler_ = std::forward<ParcelPostprocess>(parcel_postprocess);
        //
        if (!parcelport_->async_write(std::move(handler), this, buffer_)) {
            // after send has done, setup a fresh buffer for next time
            LOG_DEBUG_MSG("Wiping buffer 1");

            snd_data_type pinned_vector(chunk_pool_);
            snd_buffer_type buffer(std::move(pinned_vector), chunk_pool_);
            buffer_ = std::move(buffer);
            error_code ec;
            postprocess_handler_(ec, there_, shared_from_this());
        }
        else {
            // after send has done, setup a fresh buffer for next time
            LOG_DEBUG_MSG("Wiping buffer 2");
            snd_data_type pinned_vector(chunk_pool_);
            snd_buffer_type buffer(std::move(pinned_vector), chunk_pool_);
            buffer_ = std::move(buffer);
            error_code ec;
            postprocess_handler_(ec, there_, shared_from_this());
        }
        LOG_DEBUG_MSG("Leaving sender_connection::async_write");
    }

    bool sender_connection::can_send_immediate() const
    {
        return parcelport_->can_send_immediate();
    }


}}}}

namespace hpx {
namespace traits {
// Inject additional configuration data into the factory registry for this
// type. This information ends up in the system wide configuration database
// under the plugin specific section:
//
//      [hpx.parcel.verbs]
//      ...
//      priority = 100
//
template<>
struct plugin_config_data<hpx::parcelset::policies::verbs::parcelport> {
    static char const* priority() {
        FUNC_START_DEBUG_MSG;
        static int log_init = false;
        if (!log_init) {
#ifdef HPX_PARCELPORT_VERBS_HAVE_LOGGING
            std::cout << "Initializing logging " << std::endl;
            boost::log::add_console_log(
            std::clog,
            // This makes the sink to write log records that look like this:
            // 1: <normal> A normal severity message
            // 2: <error> An error severity message
            boost::log::keywords::format =
                (
                    boost::log::expressions::stream
                    //            << (boost::format("%05d") % expr::attr< unsigned int >("LineID"))
                    << boost::log::expressions::attr< unsigned int >("LineID")
                    << ": <" << boost::log::trivial::severity
                    << "> " << boost::log::expressions::smessage
                )
            );
            boost::log::add_common_attributes();
#endif
            log_init = true;
        }
        FUNC_END_DEBUG_MSG;
        return "100";
    }

    // This is used to initialize your parcelport,
    // for example check for availability of devices etc.
    static void init(int *argc, char ***argv, util::command_line_handling &cfg) {
        FUNC_START_DEBUG_MSG;

        FUNC_END_DEBUG_MSG;
    }

    static char const* call()
    {
        FUNC_START_DEBUG_MSG;
        FUNC_END_DEBUG_MSG;

        return
            "device = ${HPX_PARCELPORT_VERBS_DEVICE:" HPX_PARCELPORT_VERBS_DEVICE "}\n"
            "interface = ${HPX_PARCELPORT_VERBS_INTERFACE:"
            HPX_PARCELPORT_VERBS_INTERFACE "}\n"
            "memory_chunk_size = ${HPX_PARCELPORT_VERBS_MEMORY_CHUNK_SIZE:"
            BOOST_PP_STRINGIZE(HPX_PARCELPORT_VERBS_MEMORY_CHUNK_SIZE) "}\n"
            "max_memory_chunks = ${HPX_PARCELPORT_VERBS_MAX_MEMORY_CHUNKS:"
            BOOST_PP_STRINGIZE(HPX_PARCELPORT_VERBS_MAX_MEMORY_CHUNKS) "}\n"
            "zero_copy_optimization = 1\n"
            "io_pool_size = 2\n"
            "use_io_pool = 1\n"
            "enable = 0";
    }
};
}
}
RdmaControllerPtr hpx::parcelset::policies::verbs::parcelport::_rdmaController;
std::string       hpx::parcelset::policies::verbs::parcelport::_ibverbs_device;
std::string       hpx::parcelset::policies::verbs::parcelport::_ibverbs_interface;
boost::uint32_t   hpx::parcelset::policies::verbs::parcelport::_ibv_ip;
boost::uint32_t   hpx::parcelset::policies::verbs::parcelport::_port;
hpx::parcelset::policies::verbs::parcelport *hpx::parcelset::policies::verbs::parcelport::_parcelport_instance;

HPX_REGISTER_PARCELPORT(hpx::parcelset::policies::verbs::parcelport, verbs);
