//  Copyright (c) 2007-2013 Hartmut Kaiser
//  Copyright (c) 2014-2017 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/config.hpp>
#include <hpx/traits/plugin_config_data.hpp>

#include <hpx/plugins/parcelport_factory.hpp>

#include <hpx/runtime.hpp>
#include <hpx/runtime/parcelset/locality.hpp>
#include <hpx/runtime/parcelset/parcelport_impl.hpp>

#include <hpx/plugins/parcelport/ucx/active_messages.hpp>
#include <hpx/plugins/parcelport/ucx/sender.hpp>
#include <hpx/plugins/parcelport/ucx/receiver.hpp>
#include <hpx/plugins/parcelport/ucx/locality.hpp>

#include <hpx/util/runtime_configuration.hpp>
#include <hpx/util/safe_lexical_cast.hpp>
#include <hpx/util/detail/yield_k.hpp>

#define NVALGRIND
extern "C" {
#include <ucs/async/async.h>
#include <ucs/config/global_opts.h>
#include <uct/api/uct.h>
}

#include <boost/atomic.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace hpx { namespace parcelset
{
    namespace policies { namespace ucx
    {
        class HPX_EXPORT parcelport;
    }}

    template <>
    struct connection_handler_traits<policies::ucx::parcelport>
    {
        typedef policies::ucx::sender connection_type;
        typedef std::false_type send_early_parcel;
        typedef std::true_type do_background_work;
        typedef std::false_type send_immediate_parcels;

        static const char *type()
        {
            return "ucx";
        }

        static const char *pool_name()
        {
            return "parcel-pool-ucx";
        }

        static const char *pool_name_postfix()
        {
            return "-ucx";
        }
    };

    namespace policies { namespace ucx
    {
        class HPX_EXPORT parcelport
          : public parcelport_impl<parcelport>
        {
            typedef parcelport_impl<parcelport> base_type;
            typedef hpx::lcos::local::spinlock mutex_type;

            typedef receiver<parcelport> receiver_type;

            static parcelset::locality here()
            {
                return
                    parcelset::locality(locality());
            }

            static parcelport *this_;

            bool find_ifaces(util::runtime_configuration const& ini)
            {
                std::string domain = ini.get_entry("hpx.parcel.ucx.domain", "");

                ucs_status_t status;
                uct_md_resource_desc_t *md_resources = nullptr;
                uct_tl_resource_desc_t *tl_resources = nullptr;
                uct_iface_h iface = nullptr;
                unsigned num_md_resources = 0;

                status = uct_query_md_resources(&md_resources, &num_md_resources);
                if (status != UCS_OK)
                {
                    throw std::runtime_error(
                        "Querying of UCX protected domain resources failed.");
                }

                rma_iface_ = nullptr;
                am_iface_ = nullptr;

                try
                {
                    // Iterate through the protection domains and find the one
                    // matching the name in the config...
                    for (unsigned i = 0; i != num_md_resources; ++i)
                    {
                        if (domain != md_resources[i].md_name)
                            continue;

                        uct_md_config_t *md_config = nullptr;
                        pd_ = nullptr;

                        status = uct_md_config_read(
                            md_resources[i].md_name, NULL, NULL, &md_config);
                        if (status != UCS_OK)
                        {
                            throw std::runtime_error(
                                "Reading PD config failed.");
                        }

                        status = uct_md_open(md_resources[i].md_name, md_config, &pd_);
                        uct_config_release(md_config);
                        if (status != UCS_OK)
                        {
                            pd_ = nullptr;
                            throw std::runtime_error(
                                "Opening PD failed.");
                        }

                        unsigned num_tl_resources = 0;
                        status = uct_md_query_tl_resources(
                            pd_, &tl_resources, &num_tl_resources);
                        if (status != UCS_OK)
                        {
                            throw std::runtime_error(
                                "Error querying Transport resources");
                        }

                        // Iterate over the available transports.
                        for (unsigned j = 0; j != num_tl_resources; ++j)
                        {
                            uct_iface_config_t *iface_config;
                            uct_iface_params_t iface_params;
                            iface_params.tl_name = tl_resources[j].tl_name;
                            iface_params.dev_name = tl_resources[j].dev_name;
                            iface_params.stats_root = nullptr;
                            iface_params.rx_headroom = 0;
                            // @TODO: set proper mask here.
                            UCS_CPU_ZERO(&iface_params.cpu_mask);

                            // Read transport specific interface configuration
                            status = uct_iface_config_read(
                                iface_params.tl_name, NULL, NULL, &iface_config);
                            if (status != UCS_OK)
                            {
                                throw std::runtime_error(
                                    "Error reading interface config");
                            }

                            // Open Communication Interface
                            status = uct_iface_open(
                                pd_, worker_, &iface_params, iface_config, &iface);
                            uct_config_release(iface_config);
                            if (status != UCS_OK)
                            {
                                iface = nullptr;
                                throw std::runtime_error(
                                    "Error opening interface");
                            }

                            // Reading interface attributes...
                            uct_iface_attr_t iface_attr;
                            status = uct_iface_query(iface, &iface_attr);
                            if (status != UCS_OK)
                            {
                                throw std::runtime_error(
                                    "Error reading interface attributes");
                            }

                            // allow for multiple interfaces to be open
                            // only some might support all we need. On Aries,
                            // we need to have two, one for doing AM, one for
                            // RDMA...
                            //
                            // We need:
                            //  - Active message short support to signal new RDMA gets
                            //  - We need to be able to do zero copy gets to
                            //    retrieve our arguments
                            //  - We need to be able to connect to an iface directly
                            //    as point-to-point endpoints would require OOB
                            //    communication

                            // Check if the interface is suitable for AM
                            if (am_iface_ == nullptr &&
                                (iface_attr.cap.flags & UCT_IFACE_FLAG_AM_SHORT) &&
                                (iface_attr.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE))
                            {
//                                 std::cout << "found AM transport: " << iface_params.dev_name << ":" << iface_params.tl_name << '\n';
                                am_iface_ = iface;
                                std::memcpy(&am_iface_attr_, &iface_attr, sizeof(iface_attr));
                            }

                            // Check if the interface is suitable for RDMA
                            if (rma_iface_ == nullptr &&
                                (iface_attr.cap.flags & UCT_IFACE_FLAG_GET_ZCOPY))
                            {
//                                 std::cout << "found RDMA transport: " << iface_params.dev_name << ":" << iface_params.tl_name << '\n';
                                rma_iface_ = iface;
                                std::memcpy(&rma_iface_attr_, &iface_attr, sizeof(iface_attr));
                            }
                            if (rma_iface_ && am_iface_) break;

                            if (!rma_iface_ && !am_iface_)
                                uct_iface_close(iface);
                            iface = nullptr;
                        }

                        uct_release_tl_resource_list(tl_resources);
                        tl_resources = nullptr;

                        if (rma_iface_ && am_iface_) break;
                    }
                }
                catch(...)
                {

                    if (tl_resources != nullptr)
                    {
                        uct_release_tl_resource_list(tl_resources);
                    }

                    if(iface != nullptr)
                    {
                        uct_iface_close(iface);
                    }

                    if (pd_ != nullptr)
                    {
                        uct_md_close(pd_);
                        pd_ = nullptr;
                    }

                    uct_release_md_resource_list(md_resources);

                    throw;
                }

                uct_release_md_resource_list(md_resources);

                return rma_iface_ && am_iface_;
            }

        public:
            parcelport(util::runtime_configuration const& ini,
                util::function_nonser<void(std::size_t, char const*)> const& on_start,
                util::function_nonser<void()> const& on_stop)
              : base_type(ini, here(), on_start, on_stop)
              , stopped_(false)
            {
                ucs_status_t status;
                // Initialize our UCX context
                status = ucs_async_context_init(&context_, UCS_ASYNC_MODE_THREAD);
                if (status != UCS_OK)
                {
                    throw std::runtime_error(
                        "ucx::parcelport::parcelport() initialization of async context failed.");
                    return;
                }

                // Initialize our UCX worker
                status = uct_worker_create(&context_, UCS_THREAD_MODE_MULTI, &worker_);
                if (status != UCS_OK)
                {
                    throw std::runtime_error(
                        "ucx::parcelport::parcelport() initialization of worker failed.");
                    return;
                }

                // We need to find suitable network interfaces
                if (find_ifaces(ini))
                {
                    HPX_ASSERT(pd_ != nullptr);
                    HPX_ASSERT(rma_iface_ != nullptr);
                    HPX_ASSERT(am_iface_ != nullptr);

                    // get the PD related attributes, needed for memory
                    // registration
                    uct_md_query(pd_, &pd_attr_);

                    locality &l = here_.get<locality>();

                    // now get the addresses of the interfaces and set them to
                    // the locality struct in order to be exchanged with other
                    // localities through the bootstrap parcelport...

                    l.rma_addr().set_iface_attr(rma_iface_attr_);
                    uct_device_addr_t *rma_device_addr = l.rma_addr().device_addr();

                    status = uct_iface_get_device_address(rma_iface_, rma_device_addr);
                    HPX_ASSERT(status == UCS_OK);
                    if (rma_iface_attr_.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE)
                    {
                        HPX_ASSERT(l.rma_addr().iface_length_ != 0);
                        uct_iface_addr_t *rma_iface_addr = l.rma_addr().iface_addr();
                        status = uct_iface_get_address(rma_iface_, rma_iface_addr);
                        HPX_ASSERT(status == UCS_OK);
                    }

                    l.am_addr().set_iface_attr(am_iface_attr_);
                    uct_iface_addr_t *am_iface_addr = l.am_addr().iface_addr();
                    uct_device_addr_t *am_device_addr = l.am_addr().device_addr();

                    status = uct_iface_get_device_address(am_iface_, am_device_addr);
                    HPX_ASSERT(status == UCS_OK);
//                     HPX_ASSERT((am_iface_attr_.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE) != 0)
                    HPX_ASSERT(l.am_addr().iface_length_ != 0);
                    status = uct_iface_get_address(am_iface_, am_iface_addr);
                    HPX_ASSERT(status == UCS_OK);

//                     std::cout << "locality: " << here_ << '\n';

                    ucs_status_t status;

                    // Install active message handler...
                    std::uint32_t am_flags = 0;
                    if (am_iface_attr_.cap.flags & UCT_IFACE_FLAG_AM_CB_ASYNC)
                    {
                        am_flags = UCT_AM_CB_FLAG_ASYNC;
                    }
                    if (am_iface_attr_.cap.flags & UCT_IFACE_FLAG_AM_CB_SYNC)
                    {
                        am_flags = UCT_AM_CB_FLAG_SYNC;
                    }

                    status = uct_iface_set_am_handler(
                        am_iface_, connect_message, handle_connect, this, am_flags);
                    if (status != UCS_OK)
                    {
                        throw std::runtime_error(
                            "Could not set AM handler...");
                    }
                    status = uct_iface_set_am_handler(
                        am_iface_, connect_ack_message, handle_connect_ack, this, am_flags);
                    if (status != UCS_OK)
                    {
                        throw std::runtime_error(
                            "Could not set AM handler...");
                    }

                    status = uct_iface_set_am_handler(
                        am_iface_, read_message, handle_read, this, am_flags);
                    if (status != UCS_OK)
                    {
                        throw std::runtime_error(
                            "Could not set AM handler...");
                    }

                    status = uct_iface_set_am_handler(
                        am_iface_, read_ack_message, handle_read_ack, this, am_flags);
                    if (status != UCS_OK)
                    {
                        throw std::runtime_error(
                            "Could not set AM handler...");
                    }

                    status = uct_iface_set_am_handler(
                        am_iface_, close_message, handle_close, this, am_flags);
                    if (status != UCS_OK)
                    {
                        throw std::runtime_error(
                            "Could not set AM handler...");
                    }

                    this_ = this;
                }
                else
                {
                    throw std::runtime_error(
                        "No suitable UCX interface could have been found...");
                }
            }

            ~parcelport()
            {
                for (receiver_type *rcv: receivers_)
                {
                    delete rcv;
                }

                if (rma_iface_ != nullptr)
                {
                    if (rma_iface_ == am_iface_)
                    {
                        am_iface_ = nullptr;
                    }
                    uct_iface_close(rma_iface_);
                }
                std::memset(&rma_iface_attr_, 0, sizeof(rma_iface_attr_));
                if (am_iface_ != nullptr)
                {
                    uct_iface_close(am_iface_);
                }
                std::memset(&am_iface_attr_, 0, sizeof(am_iface_attr_));

                if (pd_ != nullptr)
                {
                    uct_md_close(pd_);
                }

                uct_worker_destroy(worker_);
                ucs_async_context_cleanup(&context_);
            }

            bool do_run()
            {

                return true;
            }

            void do_stop()
            {
                stopped_ = true;
            }

            std::string get_locality_name() const
            {
                return "UCX: @TODO";
            }

            std::shared_ptr<sender> create_connection(
                parcelset::locality const& there, error_code& ec)
            {
//                 std::cout << here_ << " create sender connection\n";
                std::shared_ptr<sender> res;
                if (rma_iface_attr_.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_EP)
                {
                    res = std::make_shared<sender>(there, am_iface_, rma_iface_, pd_, pd_attr_.rkey_packed_size);
                }
                else
                {
                    res = std::make_shared<sender>(there, am_iface_, nullptr, pd_, pd_attr_.rkey_packed_size);
                }

                for (std::size_t k = 0; !res->connect(here_, rma_iface_attr_.ep_addr_len); ++k)
                {
                    background_work(std::size_t(-1));
                    hpx::util::detail::yield_k(k, "ucx::parcelport::create_connection");
                }
//                 std::cout << "sender connection initiated\n";

                for (std::size_t k = 0; res->receive_handle_ == 0; ++k)
                {
                    background_work(std::size_t(-1));
                    hpx::util::detail::yield_k(k, "ucx::parcelport::create_connection");
                }

//                 std::cout << "sender connection established\n";

                return res;
            }

            parcelset::locality agas_locality(
                util::runtime_configuration const & ini) const
            {
                HPX_ASSERT(false);
                return parcelset::locality(locality());
            }

            parcelset::locality create_locality() const
            {
                return parcelset::locality(locality());
            }

            bool can_send_immediate()
            {
                return false;
            }

            bool background_work(std::size_t num_thread)
            {
                if (stopped_) return false;

//                 std::unique_lock<mutex_type> lk(worker_mtx_, std::try_to_lock);
//                 if (lk)
                    uct_worker_progress(worker_);
                return false;
            }

        private:
            boost::atomic<bool> stopped_;

            mutex_type worker_mtx_;

            ucs_async_context_t context_;
            uct_worker_h worker_;

            uct_md_h pd_;
            uct_md_attr_t pd_attr_;

            uct_iface_h rma_iface_;
            uct_iface_attr_t rma_iface_attr_;

            uct_iface_h am_iface_;
            uct_ep_h am_ep_;
            uct_iface_attr_t am_iface_attr_;

            mutex_type receivers_mtx_;
            std::unordered_set<receiver_type *> receivers_;

            mutex_type header_completions_mtx_;
            std::unordered_map<uct_completion_t *, receiver_type *> header_completions_;

            mutex_type data_completions_mtx_;
            std::unordered_map<uct_completion_t *, receiver_type *> data_completions_;

            // The message called for connect_message. Called by the sender. It creates
            // the receiver object, which will eventually issue the rdma messages.
            // Aruments:
            //  - arg: pointer to the parcelport
            //  - data:
            //      if RMA connects to EP:
            //          rma_ep_addr
            //          am_iface_addr
            //          am_device_addr
            //          sender *
            //          1
            //      if RMA connects to iface:
            //          rma_iface_addr
            //          rma_device_addr
            //          am_iface_addr
            //          am_device_addr
            //          sender *
            //          0
            //  - length:
            //      sizeof(sender *) + ep_addr_length + 1, if connect to ep
            //      sizeof(sender *) + iface_addr_length + device_addr_length + 1, if connect to iface
            static ucs_status_t handle_connect(void* arg, void* data, std::size_t length, void* desc)
            {
                parcelport *pp = reinterpret_cast<parcelport *>(arg);

                // @FIXME: Why do we to have that offset here?
                char *payload = reinterpret_cast<char *>(data);

                // we start to peel of our data and start from the back which is
                // common to both methos...
                std::size_t idx = length;

                // get the sender handle
                std::size_t sender_handle = 0;
                std::memcpy(&sender_handle,
                    payload,
                    sizeof(sender *));

                // get the remote header address
                idx -= sizeof(std::uint64_t);
                std::uint64_t remote_address = 0;
                std::memcpy(&remote_address, payload + idx, sizeof(std::uint64_t));

                // get the remote key buffer
                idx -= pp->pd_attr_.rkey_packed_size;
                void *rkey_buffer = reinterpret_cast<void *>(payload + idx);

                // get the am iface address
                idx -= pp->am_iface_attr_.device_addr_len;
                uct_device_addr_t *am_device_addr
                    = reinterpret_cast<uct_device_addr_t *>(payload + idx);

                // get the am device address
                idx -= pp->am_iface_attr_.iface_addr_len;
                uct_iface_addr_t *am_iface_addr
                    = reinterpret_cast<uct_iface_addr_t *>(payload + idx);

//                 std::cout << pp->here_ << " " << std::hex << sender_handle << " " << remote_address << " <-- connect\n";

                payload += sizeof(std::uint64_t);

                std::unique_ptr<receiver_type> rcv(new receiver_type(
                    pp->pd_,
                    sender_handle,
                    pp->pd_attr_.rkey_packed_size,
                    remote_address,
                    rkey_buffer,
                    *pp
                ));

                bool connects_to_ep = pp->rma_iface_attr_.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_EP;
                if (connects_to_ep)
                {
                    uct_device_addr_t *remote_rma_dev_addr =
                        reinterpret_cast<uct_device_addr_t *>(payload);
                    uct_ep_addr_t *remote_rma_ep_addr =
                        reinterpret_cast<uct_ep_addr_t *>(
                            payload + pp->rma_iface_attr_.device_addr_len);

                    rcv->connect(
                        pp->am_iface_,
                        am_iface_addr,
                        am_device_addr,
                        pp->rma_iface_,
                        remote_rma_dev_addr,
                        remote_rma_ep_addr);
                }
                else
                {
                    uct_iface_addr_t *remote_rma_iface_addr =
                        reinterpret_cast<uct_iface_addr_t *>(payload);
                    uct_device_addr_t *remote_rma_device_addr =
                        reinterpret_cast<uct_device_addr_t *>(
                            payload + pp->rma_iface_attr_.iface_addr_len);
                    rcv->connect(
                        pp->am_iface_,
                        am_iface_addr,
                        am_device_addr,
                        pp->rma_iface_,
                        remote_rma_iface_addr,
                        remote_rma_device_addr);
                }

                for (std::size_t k = 0;
                    !rcv->send_connect_ack(connects_to_ep, pp->rma_iface_attr_.ep_addr_len);
                    ++k)
                {
//                     pp->background_work(std::size_t(1));
//                     hpx::util::detail::yield_k(k, "ucx::parcelport::send_connect_ack");
                }

                {
                    std::lock_guard<mutex_type> lk(pp->receivers_mtx_);
                    pp->receivers_.insert(rcv.release());
                }
//                 std::cout << "receiver connection established\n";

                return UCS_OK;
            }

            // The message called for connect_ack_message. Called by the receiver. It
            // sends the pointer of the receiver along to the sender, so that
            // handle_header is able to pass along the pointer for fast lookup
            // Arguments:
            //  - arg: pointer to the parcelport
            //  - data: pointer to receiver
            //  - length: sizeof(receiver *)
            static ucs_status_t handle_connect_ack(void* arg, void* data, std::size_t length, void* desc)
            {
                parcelport *pp = reinterpret_cast<parcelport *>(arg);

                std::size_t receive_handle = 0;
                sender *snd = nullptr;

                char *payload = reinterpret_cast<char *>(data);

                std::memcpy(&receive_handle, payload, sizeof(std::uint64_t));
                std::memcpy(&snd, payload + sizeof(std::uint64_t), sizeof(std::uint64_t));

//                 std::cout << pp->here_ << " connection acknowledged! " << snd << " " << length << "\n";


                bool connects_to_ep = pp->rma_iface_attr_.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_EP;
                if (connects_to_ep)
                {
                    HPX_ASSERT(length == sizeof(std::uint64_t) * 2 + pp->rma_iface_attr_.ep_addr_len);
                    uct_ep_addr_t *rma_ep_addr =
                        reinterpret_cast<uct_ep_addr_t *>(payload + 2 * sizeof(std::uint64_t));
                    locality const &lt = snd->there_.get<locality>();
                    ucs_status_t status =
                        uct_ep_connect_to_ep(snd->rma_ep_, lt.rma_addr().device_addr(), rma_ep_addr);
                    if (status != UCS_OK)
                    {
                        throw std::runtime_error(
                            "Could not connect to remote RMA EP...");
                    }
                }
                else
                {
                    HPX_ASSERT(length == sizeof(std::uint64_t) * 2);
                }

                snd->receive_handle_ = receive_handle;

                return UCS_OK;
            }

            static void handle_data_completion(uct_completion_t *self, ucs_status_t status)
            {
                receiver_type *rcv = nullptr;
                {
                    std::lock_guard<mutex_type> lk(this_->data_completions_mtx_);
                    auto it = this_->data_completions_.find(self);

                    rcv = it->second;

                    this_->data_completions_.erase(it);

                    delete self;
                }

                if (status != UCS_OK)
                {
                    throw std::runtime_error("Error receiving header....");
                }

                rcv->read_done();
            }

            static void handle_header_completion(uct_completion_t *self, ucs_status_t status)
            {
                receiver_type *rcv = nullptr;
                {
                    std::lock_guard<mutex_type> lk(this_->header_completions_mtx_);
                    auto it = this_->header_completions_.find(self);

                    rcv = it->second;

                    this_->header_completions_.erase(it);

                    delete self;
                }

                if (status != UCS_OK)
                {
                    throw std::runtime_error("Error receiving header....");
                }

                if (rcv->read_header_done())
                {
                    rcv->read_done();
                }
                else
                {
                    uct_completion_t *data_read_completion = nullptr;
                    {
                        std::lock_guard<mutex_type> lk(this_->data_completions_mtx_);

                        std::pair<uct_completion_t *, receiver_type*> new_comp;
                        new_comp.second = rcv;
                        new_comp.first = new uct_completion_t;
                        new_comp.first->count = 1;
                        new_comp.first->func = handle_data_completion;
                        auto res = this_->data_completions_.insert(new_comp);
                        HPX_ASSERT(res.second);
                        data_read_completion = res.first->first;
                    }

                    rcv->read_data(data_read_completion);
                }
            }

            // The message called for header_message. Called by the sender. It will
            // send the necessary remote keys to the receiver, upon receiving this
            // message, the receiver is able to get the parcel data via RDMA get.
            // Arguments:
            //  - arg: pointer to the parcelport
            //  - data:
            //      receiver *
            //      header length
            //  - length:
            //      sizeof(std::uint64_t) * 2
            static ucs_status_t handle_read(void* arg, void* data, std::size_t length, void* desc)
            {
                parcelport *pp = reinterpret_cast<parcelport *>(arg);

                char *payload = reinterpret_cast<char *>(data);
                receiver_type *rcv = nullptr;
                std::memcpy(&rcv, payload, sizeof(receiver_type *));
                payload += sizeof(std::uint64_t);

                std::uint64_t header_length = 0;
                std::memcpy(&header_length, payload, sizeof(std::uint64_t));
//                 std::cout << rcv << " reading...\n";

                uct_completion_t *header_read_completion = nullptr;
                {
                    std::lock_guard<mutex_type> lk(pp->header_completions_mtx_);

                    std::pair<uct_completion_t *, receiver_type*> new_comp;
                    new_comp.second = rcv;
                    new_comp.first = new uct_completion_t;
                    new_comp.first->count = 1;
                    new_comp.first->func = handle_header_completion;
                    auto res = pp->header_completions_.insert(new_comp);
                    HPX_ASSERT(res.second);
                    header_read_completion = res.first->first;
                }

                rcv->read(header_length, header_read_completion);

                pp->background_work(0);

                return UCS_OK;
            }


            // The message called for ack_message. Called by the receiver. It is used to
            // notify the sender that all rdma get's are done, and the sender can be
            // reused.
            // Arguments:
            //  - arg: pointer to the parcelport
            //  - data: pointer to sender_
            //  - length: sizeof(sender *)
            static ucs_status_t handle_read_ack(void* arg, void* data, std::size_t length, void* desc)
            {
                HPX_ASSERT(length == sizeof(std::uint64_t));
                sender *snd = nullptr;
                std::memcpy(&snd, data, sizeof(sender *));

                HPX_ASSERT(snd);

                auto res = snd->done();
                return UCS_OK;
            }

            // Arguments:
            //  - arg: pointer to the parcelport
            //  - data: pointer to receiver
            //  - length: sizeof(receiver *)
            static ucs_status_t handle_close(void* arg, void* data, std::size_t length, void* desc)
            {
                parcelport *pp = reinterpret_cast<parcelport *>(arg);

                HPX_ASSERT(length == sizeof(receiver_type *));

                receiver_type *recv_raw = nullptr;

                std::memcpy(&recv_raw, data, sizeof(receiver_type *));
                std::unique_ptr<receiver_type> recv(recv_raw);

                HPX_ASSERT(recv);

                {
                    std::lock_guard<mutex_type> lk(pp->receivers_mtx_);
                    auto it = pp->receivers_.find(recv.get());
                    HPX_ASSERT(it != pp->receivers_.end());
                    pp->receivers_.erase(it);
                }

                return UCS_OK;
            }
        };

        parcelport *parcelport::this_ = nullptr;
    }}
}}

// @FIXME: add proper cmake generate macro...
#define HPX_PARCELPORT_UCX_DOMAIN "ib/mlx4_0"

namespace hpx { namespace traits
{
    // Inject additional configuration data into the factory registry for this
    // type. This information ends up in the system wide configuration database
    // under the plugin specific section:
    //
    //      [hpx.parcel.ucx]
    //      ...
    //      priority = 1000
    //
    template <>
    struct plugin_config_data<hpx::parcelset::policies::ucx::parcelport>
    {
        static char const* priority()
        {
            return "1000";
        }

        static void init(int *argc, char ***argv, util::command_line_handling &cfg)
        {
        }

        static char const* call()
        {
            return
                // @TODO: add zero copy optimization support ...
                "zero_copy_optimization = 0\n"
                "domain = ${HPX_PARCELPORT_UCX_DOMAIN:" HPX_PARCELPORT_UCX_DOMAIN "}\n"
                ;
        }
    };
}}

HPX_REGISTER_PARCELPORT(
    hpx::parcelset::policies::ucx::parcelport,
    ucx);
