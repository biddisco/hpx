//  Copyright (c) 2017 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_POLICIES_UCX_RECEIVER_HPP
#define HPX_PARCELSET_POLICIES_UCX_RECEIVER_HPP

#include <hpx/config.hpp>

#if defined(HPX_HAVE_PARCELPORT_UCX)

#include <hpx/runtime/parcelset/decode_parcels.hpp>
#include <hpx/runtime/parcelset/locality.hpp>

#include <hpx/plugins/parcelport/ucx/locality.hpp>

#define NVALGRIND

extern "C" {
#include <ucs/async/async.h>
#include <uct/api/uct.h>
}

namespace hpx { namespace parcelset { namespace policies { namespace ucx
{

    template <typename Parcelport>
    struct receiver
    {
        typedef std::vector<char>
            data_type;
        typedef parcel_buffer<data_type, data_type> buffer_type;

        // @FIXME: make header size configurable
        receiver(
            uct_md_h pd,
            std::size_t sender_handle,
            std::size_t rpack_length,
            std::uint64_t remote_address,
            void *packed_key,
            Parcelport& pp
        )
          : am_ep_(nullptr)
          , rma_ep_(nullptr)
          , pd_(pd)
          , header_(pd, 512, rpack_length)
          , rkey_(rpack_length)
          , sender_handle_(sender_handle)
          , remote_header_address_(remote_address)
          , pp_(pp)
        {
            ucs_status_t status;

            status = uct_rkey_unpack(packed_key, &remote_header_);
            if (status != UCS_OK)
            {
                throw std::runtime_error("receiver failed to unpack remote key");
            }
            header_iov_.buffer = header_.data_;
            header_iov_.memh = header_.uct_mem_;
            header_iov_.stride = 1;
            header_iov_.count = 1;
        }

        ~receiver()
        {
            if (am_ep_ != nullptr)
                uct_ep_destroy(am_ep_);
            if (rma_ep_ != nullptr)
                uct_ep_destroy(rma_ep_);

            uct_rkey_release(&remote_header_);
        }

        // connect to iface...
        void connect(
            uct_iface_h am_iface,
            uct_iface_addr_t *am_iface_addr,
            uct_device_addr_t *am_device_addr,
            uct_iface_h rma_iface,
            uct_iface_addr_t *rma_iface_addr,
            uct_device_addr_t *rma_device_addr
        )
        {
            ucs_status_t status;
            // Establish the connection to our AM endpoint
            status = uct_ep_create_connected(
                am_iface, am_device_addr, am_iface_addr, &am_ep_);

            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "receiver AM endpoint connection could not be established");
            }

            status = uct_ep_create_connected(
                rma_iface, rma_device_addr, rma_iface_addr, &rma_ep_);

            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "receiver RMA endpoint connection could not be established");
            }
        }

        // connect to ep...
        void connect(
            uct_iface_h am_iface,
            uct_iface_addr_t *am_iface_addr,
            uct_device_addr_t *am_device_addr,
            uct_iface_h rma_iface,
            uct_device_addr_t *rma_dev_addr,
            uct_ep_addr_t *rma_ep_addr
        )
        {
            ucs_status_t status;
            // Establish the connection to our AM endpoint
            status = uct_ep_create_connected(
                am_iface, am_device_addr, am_iface_addr, &am_ep_);

            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "receiver AM endpoint connection could not be established");
            }

            // Establish the connection to our RMA endpoint...
            status = uct_ep_create(rma_iface, &rma_ep_);
            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "receiver RMA endpoint connection could not be created");
            }

            status = uct_ep_connect_to_ep(rma_ep_, rma_dev_addr, rma_ep_addr);
            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "receiver RMA endpoint connection could not be established");
            }

//             std::cout << this << " receiver connection EP established ...\n";
        }

        bool send_connect_ack(bool connects_to_ep, std::size_t ep_addr_length)
        {
            ucs_status_t status;

            receiver *this_ = this;
            std::uint64_t header = 0;
            std::memcpy(&header, &this_, sizeof(receiver *));

            std::vector<char> payload;

            if (connects_to_ep)
            {
                payload.resize(sizeof(std::uint64_t) + ep_addr_length);
                uct_ep_addr_t *rma_ep_addr
                    = reinterpret_cast<uct_ep_addr_t *>(payload.data() + sizeof(std::uint64_t));
                uct_ep_get_address(rma_ep_, rma_ep_addr);
//                 std::cout << "sending connect ack, connect to EP\n";
            }
            else
            {
                payload.resize(sizeof(std::uint64_t));
            }

            std::memcpy(payload.data(), &sender_handle_, sizeof(std::uint64_t));

            // Notify the sender that we finished the connection
            status = uct_ep_am_short(
                am_ep_, connect_ack_message, header, payload.data(), payload.size());
            if (status == UCS_ERR_NO_RESOURCE)
            {
                return false;
            }
            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "sender AM endpoint could not send AM");
            }
//             std::cout << "connecting done...\n";
            return true;
        }

        void read(std::uint64_t header_length, uct_completion_t *completion)
        {
            ucs_status_t status;

            uct_mem_ = nullptr;
            header_.reset(header_length);
            header_iov_.length = header_length;

//             std::cout << "reading header with length " << header_length << " " << std::hex << remote_header_address_ << "\n";

            status = uct_ep_get_zcopy(
                rma_ep_, &header_iov_, 1,
                remote_header_address_, remote_header_.rkey, completion);
            // If the status is in progress, the completion handle will get called.
            if (status == UCS_INPROGRESS) return;

            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "sender could not dispatch RMA get operation");
            }
        }

        bool read_header_done()
        {
            // This assumes, the header contains all data ...
            // @FIXME: implement non piggy backed data and zero copy chunks

            buffer_.data_.resize(static_cast<std::size_t>(header_.size()));
            buffer_.num_chunks_ = header_.num_chunks();

            // determine the size of the chunk buffer
            std::size_t num_zero_copy_chunks =
                static_cast<std::size_t>(
                    static_cast<std::uint32_t>(buffer_.num_chunks_.first));
            std::size_t num_non_zero_copy_chunks =
                static_cast<std::size_t>(
                    static_cast<std::uint32_t>(buffer_.num_chunks_.second));
            buffer_.transmission_chunks_.resize(
                num_zero_copy_chunks + num_non_zero_copy_chunks
            );

            char *piggy_back = header_.piggy_back();
            if(piggy_back)
            {
                std::memcpy(&buffer_.data_[0], piggy_back, buffer_.data_.size());
                return true;
            }

            return false;
        }

        void read_data(uct_completion_t *completion)
        {
            ucs_status_t status;

            status =
                uct_md_mem_reg(pd_, buffer_.data_.data(), buffer_.data_.size(),
                    UCT_MD_MEM_FLAG_NONBLOCK, &uct_mem_);
            if (status != UCS_OK)
            {
                throw std::runtime_error("sender failing to register memory");
            }

            data_iov_.length = buffer_.data_.size();
            data_iov_.buffer = buffer_.data_.data();
            data_iov_.memh = uct_mem_;
            data_iov_.stride = 1;
            data_iov_.count = 1;

            std::uint64_t remote_data_address = 0;
            char *message = header_.data() + header_.length() - sizeof(std::uint64_t) - rkey_.size();
            std::memcpy(&remote_data_address, message, sizeof(std::uint64_t));

            message += sizeof(std::uint64_t);
            std::memcpy(rkey_.data(), message, rkey_.size());

            uct_rkey_bundle_t remote_data;
            status = uct_rkey_unpack(rkey_.data(), &remote_data);

            status = uct_ep_get_zcopy(
                rma_ep_, &data_iov_, 1,
                remote_data_address, remote_data.rkey, completion);
            // If the status is in progress, the completion handle will get called.
            if (status == UCS_INPROGRESS) return;

            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "sender could not dispatch RMA get operation");
            }
        }

        void read_done()
        {
            decode_parcels(pp_, std::move(buffer_), -1);

            if (uct_mem_ != nullptr)
            {
                uct_md_mem_dereg(pd_, uct_mem_);
                uct_mem_ = nullptr;
            }

            ucs_status_t status;
            status = uct_ep_am_short(
                am_ep_, read_ack_message, sender_handle_, nullptr, 0);
            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "sender AM endpoint could not send AM");
            }
        }

        uct_ep_h am_ep_;
        uct_ep_h rma_ep_;
        uct_md_h pd_;
        header header_;
        std::vector<char> rkey_;
        std::uint64_t sender_handle_;
        std::uint64_t remote_header_address_;
        uct_rkey_bundle_t remote_header_;
        uct_iov_t header_iov_;
        uct_iov_t data_iov_;
        uct_mem_h uct_mem_;

        buffer_type buffer_;
        Parcelport &pp_;
    };
}}}}

#endif
#endif
