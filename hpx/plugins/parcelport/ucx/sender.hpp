//  Copyright (c) 2017 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_POLICIES_UCX_SENDER_HPP
#define HPX_PARCELSET_POLICIES_UCX_SENDER_HPP

#include <hpx/config.hpp>

#if defined(HPX_HAVE_PARCELPORT_UCX)

#include <hpx/runtime/parcelset/parcelport_connection.hpp>
#include <hpx/runtime/parcelset/locality.hpp>

#include <hpx/plugins/parcelport/ucx/active_messages.hpp>
#include <hpx/plugins/parcelport/ucx/locality.hpp>
#include <hpx/plugins/parcelport/ucx/header.hpp>

#define NVALGRIND

extern "C" {
#include <ucs/async/async.h>
#include <uct/api/uct.h>
}

namespace hpx { namespace parcelset { namespace policies { namespace ucx
{
    struct sender
      : parcelset::parcelport_connection<
            sender
          , std::vector<char>
        >
    {
        // @FIXME: make header size configurable
        sender(
            parcelset::locality const& there,
            uct_iface_h am_iface,
            uct_iface_h rma_iface,
            uct_md_h pd,
            std::size_t rpack_length)
          : there_(there)
          , am_iface_(am_iface)
          , am_ep_(nullptr)
          , rma_ep_(nullptr)
          , pd_(pd)
          , header_(pd, 512, rpack_length)
          , rkey_(rpack_length)
          , receive_handle_(0)
          , rma_connect_to_ep_(rma_iface != nullptr)
        {
//             std::cout << "sending to: " << there_ << '\n';
            locality &lt = there_.get<locality>();
            ucs_status_t status;
            status = uct_ep_create_connected(
                am_iface_, lt.am_addr().device_addr(), lt.am_addr().iface_addr(), &am_ep_);

            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "sender AM endpoint connection could not be established");
            }

            if (rma_connect_to_ep_)
            {
                status = uct_ep_create(rma_iface, &rma_ep_);

                if (status != UCS_OK)
                {
                    throw std::runtime_error(
                        "sender AM endpoint connection could not be established");
                }
            }

//             std::cout << "sender ... " << this << "\n";
        }

        ~sender()
        {
            if (am_ep_ != nullptr)
                uct_ep_destroy(am_ep_);

            if (rma_ep_ != nullptr)
                uct_ep_destroy(rma_ep_);
        }

        parcelset::locality const& destination() const
        {
            return there_;
        }

        void verify(parcelset::locality const & parcel_locality_id) const
        {
        }

        bool connect(parcelset::locality const& here, std::size_t rma_ep_addr_len)
        {
            ucs_status_t status;
            // @TODO: factor out payload creation to avoid recreation...
            locality const &lh = here.get<locality>();

            auto rkey = header_.rkey();

            std::size_t am_iface_addr_len = lh.am_addr().iface_length_;
            std::size_t am_device_addr_len = lh.am_addr().device_length_;
            std::size_t rma_iface_addr_len = lh.rma_addr().iface_length_;
            std::size_t rma_device_addr_len = lh.rma_addr().device_length_;
            std::size_t am_iface_offset = 0;
            std::size_t size = am_device_addr_len + am_iface_addr_len +
                // Space for the header information: remote key and address
                rkey.second + sizeof(std::uint64_t);

            std::vector<char> payload;
            // If the rma ep is not set, the RMA endpoint on the receiver side
            // needs to be connected to the iface directly...
            if (rma_ep_ == nullptr)
            {
                am_iface_offset = rma_iface_addr_len + rma_device_addr_len;
                size += am_iface_offset;
                payload.resize(size);
                std::memcpy(payload.data(),
                    lh.rma_addr().iface_addr(), rma_iface_addr_len);
                std::memcpy(payload.data() + rma_iface_addr_len,
                    lh.rma_addr().device_addr(), rma_device_addr_len);
            }
            // if it is set, we need to create an endpoint to endpoint
            // connection...
            else
            {
//                 std::cout << "need EP connection...\n";
                am_iface_offset = rma_device_addr_len + rma_ep_addr_len;
                size += am_iface_offset;
                payload.resize(size);
                // get RMA device address...
                std::memcpy(payload.data(), lh.rma_addr().device_addr(), rma_device_addr_len);
                // get RMA EP address...
                uct_ep_addr_t *rma_ep_addr = reinterpret_cast<uct_ep_addr_t *>(payload.data() + rma_device_addr_len);
                status = uct_ep_get_address(rma_ep_, rma_ep_addr);
                if (status != UCS_OK)
                {
                    throw std::runtime_error(
                        "Could not retrieve EP address");
                }
            }
            std::memcpy(payload.data() + am_iface_offset, lh.am_addr().iface_addr(), am_iface_addr_len);

            std::size_t am_device_offset = am_iface_offset + am_iface_addr_len;
            std::memcpy(payload.data() + am_device_offset, lh.am_addr().device_addr(), am_device_addr_len);

            // send our rkey information along...
            std::size_t rkey_offset = am_device_offset + am_device_addr_len;
            std::memcpy(payload.data() + rkey_offset, rkey.first, rkey.second);
            std::memcpy(payload.data() + rkey_offset + rkey.second, &header_.data_, sizeof(std::uint64_t));

            HPX_ASSERT(rkey_offset + rkey.second + sizeof(std::uint64_t) == payload.size());

//             std::cout << "sender remote address " << header_.data_ << '\n';

            sender *this_ = this;
            std::uint64_t header = 0;
            std::memcpy(&header, &this_, sizeof(sender *));
            status = uct_ep_am_short(
                am_ep_, connect_message, header, payload.data(), payload.size());
            if (status == UCS_ERR_NO_RESOURCE)
            {
                return false;
            }
            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "sender AM endpoint could not send AM");
            }

            return true;
        }

        template <typename Handler, typename ParcelPostprocess>
        void async_write(Handler && handler, ParcelPostprocess && parcel_postprocess)
        {
            uct_mem_ = nullptr;
            this_ = shared_from_this();
            HPX_ASSERT(receive_handle_ != 0);
            // @TODO: add zero copy optimization support ...
            HPX_ASSERT(buffer_.transmission_chunks_.empty());
//             std::cout << std::hex << receive_handle_ << " <--- async_write\n";

            handler_ = std::forward<Handler>(handler);
            postprocess_handler_ = std::forward<ParcelPostprocess>(parcel_postprocess);
            HPX_ASSERT(handler_);
            HPX_ASSERT(postprocess_handler_);

            // fill the header
            HPX_ASSERT(!buffer_.data_.empty());
            header_.reset(buffer_);

            ucs_status_t status;
            // if we don't have the message piggy backed, register the buffers
            // data, and send it along
            if (header_.piggy_back() == nullptr)
            {
                // @TODO: memory registration cache...
                status =
                    uct_md_mem_reg(pd_, buffer_.data_.data(), buffer_.data_.size(),
                        UCT_MD_MEM_FLAG_NONBLOCK, &uct_mem_);
                if (status != UCS_OK)
                {
                    throw std::runtime_error("sender failing to register memory");
                }
                char *data_ptr = buffer_.data_.data();

                char *message = header_.data() + header_.length() - sizeof(std::uint64_t) - rkey_.size();
                std::memcpy(message, &data_ptr, sizeof(std::uint64_t));

                message += sizeof(std::uint64_t);
                uct_md_mkey_pack(pd_, uct_mem_, rkey_.data());
                std::memcpy(message, rkey_.data(), rkey_.size());
            }

//             std::cout << "sending " << header_.size() << " " << header_.length() << "\n";

            // Notify the receiver that the message is ready to be read
            std::uint64_t payload = header_.length();
            status = uct_ep_am_short(
                am_ep_, read_message, receive_handle_, &payload, sizeof(std::uint64_t));
            if (status != UCS_OK)
            {
                throw std::runtime_error(
                    "sender AM endpoint could not send AM for header");
            }
        }

        std::shared_ptr<sender> done()
        {
            HPX_ASSERT(handler_);
            HPX_ASSERT(postprocess_handler_);
//             std::cout << this << " sending done!\n";

            // We are done and are able to call the handlers now
            error_code ec;
            handler_(ec);

            if (uct_mem_ != nullptr)
            {
                uct_md_mem_dereg(pd_, uct_mem_);
                uct_mem_ = nullptr;
            }

            buffer_.clear();
            postprocess_handler_(ec, there_, this_);

            // This is needed to keep ourselv alive long enough...
            std::shared_ptr<sender> res;
            std::swap(this_, res);
            return res;
        }

        // we use this to keep ourselves alive during async sends...
        std::shared_ptr<sender> this_;

        parcelset::locality there_;
        uct_iface_h am_iface_;
        uct_ep_h am_ep_;
        uct_ep_h rma_ep_;
        uct_md_h pd_;

        header header_;
        std::vector<char> rkey_;
        uct_mem_h uct_mem_;

        util::unique_function_nonser<
            void(
                error_code const&
            )
        > handler_;
        util::unique_function_nonser<
            void(
                error_code const&
              , parcelset::locality const&
              , std::shared_ptr<sender>
            )
        > postprocess_handler_;

        std::size_t receive_handle_;

        bool rma_connect_to_ep_;
    };
}}}}

#endif
#endif
