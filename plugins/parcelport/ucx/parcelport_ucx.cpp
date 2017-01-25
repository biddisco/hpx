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
#include <hpx/plugins/parcelport/ucx/ucx_context.hpp>

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

        public:
            parcelport(util::runtime_configuration const& ini,
                util::function_nonser<void(std::size_t, char const*)> const& on_start,
                util::function_nonser<void()> const& on_stop)
              : base_type(ini, here(), on_start, on_stop)
              , context_(ini.get_entry("hpx.parcel.ucx.domain", ""), here_)
              , stopped_(false)
            {
                ucs_status_t status;

                // Install active message handler...
                std::uint32_t am_flags = 0;
//                     if (am_iface_attr_.cap.flags & UCT_IFACE_FLAG_AM_CB_ASYNC)
                {
                    am_flags = UCT_AM_CB_FLAG_ASYNC;
                }
//                     if (am_iface_attr_.cap.flags & UCT_IFACE_FLAG_AM_CB_SYNC)
//                     {
//                         am_flags = UCT_AM_CB_FLAG_SYNC;
//                     }

                status = uct_iface_set_am_handler(
                    context_.am_iface_, connect_message, handle_connect, this, am_flags);
                if (status != UCS_OK)
                {
                    throw std::runtime_error(
                        "Could not set AM handler...");
                }
                status = uct_iface_set_am_handler(
                    context_.am_iface_, connect_ack_message, handle_connect_ack, this, am_flags);
                if (status != UCS_OK)
                {
                    throw std::runtime_error(
                        "Could not set AM handler...");
                }

                status = uct_iface_set_am_handler(
                    context_.am_iface_, read_message, handle_read, this, am_flags);
                if (status != UCS_OK)
                {
                    throw std::runtime_error(
                        "Could not set AM handler...");
                }

                status = uct_iface_set_am_handler(
                    context_.am_iface_, read_ack_message, handle_read_ack, this, am_flags);
                if (status != UCS_OK)
                {
                    throw std::runtime_error(
                        "Could not set AM handler...");
                }

                status = uct_iface_set_am_handler(
                    context_.am_iface_, close_message, handle_close, this, am_flags);
                if (status != UCS_OK)
                {
                    throw std::runtime_error(
                        "Could not set AM handler...");
                }

                this_ = this;
            }

            ~parcelport()
            {
                for (receiver_type *rcv: receivers_)
                {
                    delete rcv;
                }
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
                if (context_.rma_iface_attr_.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_EP)
                {
                    res = std::make_shared<sender>(there, context_, true);
                }
                else
                {
                    res = std::make_shared<sender>(there, context_, false);
                }

                for (std::size_t k = 0; !res->connect(here_, context_.rma_iface_attr_.ep_addr_len); ++k)
                {
                    context_.progress();
                    hpx::util::detail::yield_k(k, "ucx::parcelport::create_connection");
                }

                for (std::size_t k = 0; res->receive_handle_ == 0; ++k)
                {
                    context_.progress();
                    hpx::util::detail::yield_k(k, "ucx::parcelport::create_connection");
                }


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
                    context_.progress();
                return false;
            }

        private:
            ucx_context context_;
            boost::atomic<bool> stopped_;

            std::unordered_set<receiver_type *> receivers_;

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
                idx -= pp->context_.pd_attr_.rkey_packed_size;
                void *rkey_buffer = reinterpret_cast<void *>(payload + idx);

                // get the am iface address
                idx -= pp->context_.am_iface_attr_.device_addr_len;
                uct_device_addr_t *am_device_addr
                    = reinterpret_cast<uct_device_addr_t *>(payload + idx);

                // get the am device address
                idx -= pp->context_.am_iface_attr_.iface_addr_len;
                uct_iface_addr_t *am_iface_addr
                    = reinterpret_cast<uct_iface_addr_t *>(payload + idx);

//                 std::cout << pp->here_ << " " << std::hex << sender_handle << " " << remote_address << " <-- connect\n";

                payload += sizeof(std::uint64_t);

                std::unique_ptr<receiver_type> rcv(new receiver_type(
                    pp->context_,
                    sender_handle,
                    remote_address,
                    rkey_buffer,
                    *pp
                ));

                bool connects_to_ep = pp->context_.rma_iface_attr_.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_EP;
                if (connects_to_ep)
                {
                    uct_device_addr_t *remote_rma_dev_addr =
                        reinterpret_cast<uct_device_addr_t *>(payload);
                    uct_ep_addr_t *remote_rma_ep_addr =
                        reinterpret_cast<uct_ep_addr_t *>(
                            payload + pp->context_.rma_iface_attr_.device_addr_len);

                    rcv->connect(
                        am_iface_addr,
                        am_device_addr,
                        remote_rma_dev_addr,
                        remote_rma_ep_addr);
                }
                else
                {
                    uct_iface_addr_t *remote_rma_iface_addr =
                        reinterpret_cast<uct_iface_addr_t *>(payload);
                    uct_device_addr_t *remote_rma_device_addr =
                        reinterpret_cast<uct_device_addr_t *>(
                            payload + pp->context_.rma_iface_attr_.iface_addr_len);
                    rcv->connect(
                        am_iface_addr,
                        am_device_addr,
                        remote_rma_iface_addr,
                        remote_rma_device_addr);
                }

                for (std::size_t k = 0;
                    !rcv->send_connect_ack(connects_to_ep, pp->context_.rma_iface_attr_.ep_addr_len);
                    ++k)
                {
                    pp->context_.progress();
                    hpx::util::detail::yield_k(k, "ucx::parcelport::send_connect_ack");
                }

                rcv.release();
//                 {
//                     pp->receivers_.insert(rcv.release());
//                 }
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


                bool connects_to_ep = pp->context_.rma_iface_attr_.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_EP;
                if (connects_to_ep)
                {
                    HPX_ASSERT(length == sizeof(std::uint64_t) * 2 + pp->context_.rma_iface_attr_.ep_addr_len);
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

                rcv->read(header_length);

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
