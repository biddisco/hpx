//  Copyright (c) 2015-2017 John Biddiscombe
//  Copyright (c) 2017      Thomas Heller
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <plugins/parcelport/libfabric/parcelport_libfabric.hpp>
#include <plugins/parcelport/libfabric/rma_receiver.hpp>
//
#include <hpx/runtime/parcelset/decode_parcels.hpp>
#include <hpx/runtime/parcelset/parcel_buffer.hpp>
//
#include <hpx/assertion.hpp>
#include <hpx/basic_execution/this_thread.hpp>
//
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>
#include <chrono>
//
#include <rdma/fabric.h>
#include <rdma/fi_rma.h>

#include <hpx/debugging/print.hpp>
namespace hpx {
    // cppcheck-suppress ConfigurationNotChecked
    static hpx::debug::enable_print<false> rma_deb("RMA_RCV");
}   // namespace hpx

namespace hpx {
namespace parcelset {
namespace policies {
namespace libfabric
{

    performance_counter<unsigned int> rma_receiver::msg_plain_;
    performance_counter<unsigned int> rma_receiver::msg_rma_;
    performance_counter<unsigned int> rma_receiver::sent_ack_;
    performance_counter<unsigned int> rma_receiver::rma_reads_;
    performance_counter<unsigned int> rma_receiver::recv_deletes_;

    // --------------------------------------------------------------------
    rma_receiver::rma_receiver(
        parcelport* pp,
        fid_ep* endpoint,
        rma::memory_pool<region_provider>* memory_pool,
        completion_handler&& handler)
      : rma_base(ctx_rma_receiver)
      , parcelport_(pp)
      , endpoint_(endpoint)
      , header_region_(nullptr)
      , chunk_region_(nullptr)
      , message_region_(nullptr)
      , header_(nullptr)
      , memory_pool_(memory_pool)
      , handler_(std::move(handler))
      , rma_count_(0)
      , chunk_fetch_(false)
    {}

    // --------------------------------------------------------------------
    void rma_receiver::read_message(region_type* region,
        fi_addr_t const& src_addr)
    {
        start_time_ = util::high_resolution_clock::now();
        HPX_ASSERT(rma_count_ == 0);
        HPX_ASSERT(header_ == nullptr);
        HPX_ASSERT(header_region_ == nullptr);
        HPX_ASSERT(chunk_region_ == nullptr);
        HPX_ASSERT(message_region_ == nullptr);
        HPX_ASSERT(rma_regions_.size() == 0);
        HPX_ASSERT(chunk_fetch_ == false);

        // where this message came from
        src_addr_ = src_addr;

        // the region posted as a receive contains the received header
        header_region_ = region;
        header_        = reinterpret_cast<header_type*>(header_region_->get_address());

        HPX_ASSERT(header_);
        HPX_ASSERT(header_region_->get_address());

        rma_deb.debug("receiver " , hpx::debug::ptr(this) , "Header : " , *header_);

        rma_deb.trace(
            hpx::debug::mem_crc32(header_, header_->header_length(), "Header region (recv)"));

        if (header_->bootstrap()) {
            handle_bootstrap_message();
            parcelport_->set_bootstrap_complete();
            return;
        }

        if (header_->chunk_ptr()==nullptr) {
            // the header does not have piggybacked chunks, we must rma-get them before
            // we can decode the message, they may need further rma-get operations
            handle_message_no_chunk_data();
            return;
        }

        // how many RMA operations are needed
        rma_count_ = header_->num_zero_copy_chunks();

        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "is expecting " << hpx::debug::dec<>(rma_count_) << "read completions");

        // If we have no zero copy chunks and piggy backed data, we can
        // process the message immediately, otherwise, dispatch to receiver
        // If we have neither piggy back, nor zero copy chunks, rma_count is 0
        if (rma_count_ == 0)
        {
            handle_message_no_rma();
            ++msg_plain_;
        }
        else {
            handle_message_with_zerocopy_rma();
            ++msg_rma_;
        }
    }

    // --------------------------------------------------------------------
    void rma_receiver::handle_message_no_rma()
    {
        HPX_ASSERT(header_);
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "handle piggy backed send without zero copy regions");

        char *piggy_back = header_->message_data();
        HPX_ASSERT(piggy_back);

        rma_deb.trace(
            hpx::debug::mem_crc32(piggy_back, header_->message_size(),
                "(Message region recv piggybacked - no rdma)"));

        typedef pinned_memory_vector<char, header_size, region_type, memory_pool_type>
            rcv_data_type;
        typedef parcel_buffer<rcv_data_type, std::vector<char>> rcv_buffer_type;

        // when parcel decoding from the wrapped pointer buffer has completed,
        // the lambda function will be called
        rcv_data_type wrapped_pointer(
            piggy_back, header_->message_size(), [](){}, nullptr, nullptr);

        rcv_buffer_type buffer(std::move(wrapped_pointer), nullptr);

        auto zc_chunks =
            std::count_if(chunks_.begin(), chunks_.end(), [](chunktype &c) {
                return c.type_ == serialization::chunk_type_pointer ||
                       c.type_ == serialization::chunk_type_rma;
            });
        HPX_ASSERT(zc_chunks==0);
        unsigned int oo_chunks = chunks_.size() - zc_chunks;

        buffer.num_chunks_ = std::make_pair(zc_chunks, oo_chunks);
        buffer.data_size_  = header_->message_size();
        performance_counters::parcels::data_point& data = buffer.data_point_;
        data.bytes_ = static_cast<std::size_t>(header_->message_size());
        data.time_ = util::high_resolution_clock::now() - start_time_;
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "calling parcel decode for complete NORMAL parcel");
        std::size_t num_thread = hpx::get_worker_thread_num();
        decode_message_with_chunks(*parcelport_, std::move(buffer), 0, chunks_, num_thread);
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "parcel decode called for complete NORMAL (small) parcel");

        cleanup_receive();
    }

    // --------------------------------------------------------------------
    void rma_receiver::handle_message_with_zerocopy_rma()
    {
        chunks_.resize(header_->num_chunks());
        char *chunk_data = header_->chunk_data();
        HPX_ASSERT(chunk_data);

        size_t chunkbytes =
            chunks_.size() * sizeof(chunktype);

        std::memcpy(chunks_.data(), chunk_data, chunkbytes);
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "Copied chunk data from header : size "
            << hpx::debug::dec<>(chunkbytes));

        LOG_EXCLUSIVE(
        for (const chunktype &c : chunks_)
        {
            LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
                << "recv : chunk : size " << hpx::debug::hex<4>(c.size_)
                << "type "   << hpx::debug::dec<>((uint64_t)c.type_)
                << "rma "    << hpx::debug::ptr(c.rma_)
                << "cpos "   << hpx::debug::ptr(c.data_.cpos_)
                << "index "  << hpx::debug::dec<>(c.data_.index_));
        });

        rma_regions_.reserve(rma_count_);

        // for each zerocopy chunk, schedule a read operation
        read_chunk_list();
    }

    // --------------------------------------------------------------------
    void rma_receiver::handle_bootstrap_message()
    {
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "handle bootstrap message");
        HPX_ASSERT(header_);

        char *piggy_back = header_->message_data();
        HPX_ASSERT(piggy_back);

        rma_deb.trace(
            hpx::debug::mem_crc32(piggy_back, header_->message_size(),
                "(Message region recv piggybacked - no rdma)"));
        //
        std::size_t N = header_->message_size()/sizeof(libfabric::locality);
        //
        std::vector<libfabric::locality> addresses;
        addresses.reserve(N);
        //
        const libfabric::locality *data =
                reinterpret_cast<libfabric::locality*>(header_->message_data());
        for (std::size_t i=0; i<N; ++i) {
            addresses.push_back(data[i]);
            LOG_DEBUG_MSG("bootstrap received " << iplocality(data[i]));
        }
        LOG_DEBUG_MSG("bootstrap received " << hpx::debug::dec<>(N) << "addresses");
        parcelport_->recv_bootstrap_address(addresses);
        //
        cleanup_receive();
    }

    // --------------------------------------------------------------------
    void rma_receiver::handle_message_no_chunk_data()
    {
        chunk_fetch_ = true;
        // get the remote chunk block memory region details
        auto &cb = header_->chunk_header_ptr()->chunk_rma;
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "Fetching RMA chunk for chunk data with "
            << "size "   << hpx::debug::dec<>(cb.size_)
            << "rma "    << hpx::debug::ptr(cb.rma_)
            << "addr "   << hpx::debug::ptr(cb.data_.cpos_));

        // we need a local memory region to read the chunks into
        chunk_region_ = memory_pool_->allocate_region(cb.size_);
        chunk_region_->set_message_length(cb.size_);
        uint64_t rkey1 = cb.rma_;
        const void *remoteAddr1 = cb.data_.cpos_;
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "Fetching chunk region with size " << hpx::debug::dec<>(cb.size_));
        rma_count_ = 1;

        // if the message region requires rma-get, we can get it now as well
        if (!header_->message_piggy_back())
        {
            auto &mc = header_->message_chunk_ptr()->message_rma;
            message_region_ = memory_pool_->allocate_region(mc.size_);
            message_region_->set_message_length(mc.size_);
            uint64_t rkey2 = mc.rma_;
            const void *remoteAddr2 = mc.data_.cpos_;
            LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
                << "Fetching message region with size " << hpx::debug::dec<>(mc.size_));
            ++rma_count_;
            // call the rma read function for the message chunk
            read_one_chunk(src_addr_, message_region_, remoteAddr2, rkey2);
        }

        // call the rma read function for the chunk-info chunk
        // we do this last so that a completion does not come before the message
        // chunk read has been triggered - this would cause the rma receive to decrement
        // the counter and might hit zero before we had sent the second request
        read_one_chunk(src_addr_, chunk_region_, remoteAddr1, rkey1);
    }

    // --------------------------------------------------------------------
    int rma_receiver::handle_chunks_read_message()
    {
        char *chunk_data = chunk_region_->get_address();
        HPX_ASSERT(chunk_data);
        //
        uint64_t chunkbytes = chunk_region_->get_message_length();
        uint64_t num_chunks = chunkbytes/sizeof(chunktype);
        chunks_.resize(num_chunks);
        std::memcpy(chunks_.data(), chunk_data, chunkbytes);
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "Copied chunk data from chunk_region: size " << hpx::debug::dec<>(chunkbytes)
            << "with num chunks " << hpx::debug::dec<>(num_chunks));
        //
        HPX_ASSERT(rma_regions_.size() == 0);
        //
        chunk_fetch_ = false;
        // for each zerocopy chunk, schedule a read operation
        uint64_t zc_count =
            std::count_if(chunks_.begin(), chunks_.end(), [](chunktype &c) {
                return c.type_ == serialization::chunk_type_pointer ||
                       c.type_ == serialization::chunk_type_rma;
            });
        // this is the number of rma-completions we must wait for
        rma_count_ = zc_count;
        //
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "Restarting RMA reads with " << hpx::debug::dec<>(zc_count) << "rma chunks");
        // do not return rma_count_ as it might already have decremented! (racey)
        read_chunk_list();
        return zc_count;
    }

    // --------------------------------------------------------------------
    void rma_receiver::read_chunk_list()
    {
        for (chunktype &c : chunks_)
        {
            if (c.type_ == serialization::chunk_type_pointer ||
                c.type_ == serialization::chunk_type_rma)
            {
                region_type *get_region =
                    memory_pool_->allocate_region(c.size_);
                // Set the used space limit to the incoming buffer size
                get_region->set_message_length(c.size_);

                rma_deb.trace(
                    hpx::debug::mem_crc32(get_region->get_address(), c.size_,
                        "(RDMA GET region (new))"));

                // store the remote key in case we overwrite it
                std::uint64_t remote_key = c.rma_;

                if (c.type_ == serialization::chunk_type_rma) {
                    // rma object/vector chunks are not deleted
                    // so do not add them to the rma_regions list for cleanup
                    rma_deb.trace("Passing rma region to chunk structure");
                    c.rma_ = std::uintptr_t(get_region);
                }
                else {
                    rma_regions_.push_back(get_region);
                }
                // overwrite the serialization chunk data pointer because the chunk
                // info sent contains the pointer to the remote data and when we
                // decode the parcel we want the chunk to point to the local copy of it
                const void *remoteAddr = c.data_.cpos_;
                c.data_.cpos_ = get_region->get_address();

                // call the rma read function for the chunk
                read_one_chunk(src_addr_, get_region, remoteAddr, remote_key);
            }
        }
    }

    // --------------------------------------------------------------------
    void rma_receiver::read_one_chunk(
        fi_addr_t src_addr, region_type *get_region,
        const void *remoteAddr, uint64_t rkey)
    {
        // post the rdma read/get
        LOG_DEBUG_MSG("rma_receiver " << hpx::debug::ptr(this)
            << "RDMA Get fi_read :"
            << "client " << hpx::debug::ptr(endpoint_)
            << "fi_addr " << hpx::debug::ptr(src_addr_)
            << "tag " << hexuint64(header_->tag())
            << "local addr " << hpx::debug::ptr(get_region->get_address())
            << "local local key " << hpx::debug::ptr(get_region->get_local_key())
            << "size " << hpx::debug::hex<6>(get_region->get_message_length())
            << "rkey " << hpx::debug::ptr(rkey)
            << "remote cpos " << hpx::debug::ptr(remoteAddr));

        // count reads
        ++rma_reads_;

        bool ok = false;
        while(!ok) {
            LOG_EXCLUSIVE(
                // write a pattern and dump out data for debugging purposes
                uint32_t *buffer =
                    reinterpret_cast<uint32_t*>(get_region->get_address());
                std::fill(buffer, buffer + get_region->get_size()/4,
                   0xDEADC0DE);
                rma_deb.trace(
                    hpx::debug::mem_crc32(get_region->get_address(), get_region->get_message_length(),
                              "(RDMA GET region (pre-fi_read))"));
            );

            ssize_t ret = fi_read(endpoint_, get_region->get_address(),
                get_region->get_message_length(), get_region->get_local_key(),
                src_addr_, (uint64_t)(remoteAddr), rkey, this);

            if (ret==0) {
                ok = true;
            }
            else if (ret == -FI_EAGAIN)
            {
                LOG_ERROR_MSG("receiver " << hpx::debug::ptr(this)
                    << "reposting fi_read...\n");
                parcelport_->background_work(0, 
                    hpx::parcelset::parcelport_background_mode_all);
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
            else if (ret)
            {
                throw fabric_error(ret, "fi_read");
            }
        }
    }

    // --------------------------------------------------------------------
    // whenever and rma read completion event occurs, this function is entered.
    // an atomic counter, counts down with each completion, until we hit zero,
    // when all expected read results are available.
    void rma_receiver::handle_rma_read_completion()
    {
        FUNC_START_DEBUG_MSG;
        HPX_ASSERT(rma_count_ > 0);
        // If we haven't read all chunks, we can return and wait
        // for the other incoming read completions
        if (--rma_count_ > 0)
        {
            LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
                << "Not yet read all RMA regions " << hpx::debug::ptr(this));
            FUNC_START_DEBUG_MSG;
            return;
        }

        HPX_ASSERT(rma_count_ == 0);

        // when the chunk structure could not be piggybacked, the chunk_fetch_
        // flag is set prior to reading the chunks.
        if (chunk_fetch_) {
            LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
                << "rma read chunk list complete");
            if (handle_chunks_read_message()>0) {
                // more rma reads have been started, so exit and wait for them
                return;
            }
        }
        else {
            LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
                << "all RMA regions now read ");
        }

        // If the main message was not piggy backed, then the message region
        // is either the final chunk of the rma list (if chunks were piggybacked)
        // or read via rma during the chunk fetch (chunks not piggybacked)
        if (!header_->message_piggy_back())
        {
            if (header_->chunk_ptr()) {
                message_region_ = rma_regions_.back();
                //
                rma_regions_.resize(rma_regions_.size()-1);
                chunks_.resize(chunks_.size()-1);
            }
            else {
                LOG_DEBUG_MSG("No piggy back message or chunks");
                // message region should have been read by handle_message_no_chunk_data
                HPX_ASSERT(message_region_);
            }
        }

        std::size_t message_length = header_->message_size();
        char *message = nullptr;
        if (message_region_)
        {
            message = static_cast<char *>(message_region_->get_address());
            HPX_ASSERT(message);
            LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
                << "No piggy_back RDMA message "
                << "region " << hpx::debug::ptr(message_region_)
                << "address " << hpx::debug::ptr(message_region_->get_address())
                << "length " << hexuint32(message_length));
            rma_deb.trace(
                hpx::debug::mem_crc32(message, message_length, "Message region (recv rdma)"));

            // do this after dumping out data as otherwise we lose some debug info
            HPX_ASSERT(message_region_->get_message_length() == header_->message_size());
        }
        else
        {
            HPX_ASSERT(header_->message_data());
            message = header_->message_data();
            rma_deb.trace(hpx::debug::mem_crc32(message, message_length,
                "Message region (recv piggyback with rdma)"));
        }

        for (auto &r : rma_regions_)
        {
            HPX_UNUSED(r);
            rma_deb.trace(hpx::debug::mem_crc32(r->get_address(), r->get_message_length(),
                "rdma region (recv) "));
        }

        // wrap the message and chunks into a pinned vector so that they
        // can be passed into the parcel decode functions and when released have
        // the pinned buffers returned to the memory pool
        typedef pinned_memory_vector<char, header_size, region_type, memory_pool_type>
            rcv_data_type;
        typedef parcel_buffer<rcv_data_type, std::vector<char>> rcv_buffer_type;

        rcv_data_type wrapped_pointer(message, message_length,
            [this, message, message_length]()
            {
                // deleted cleanup until all problems resolved, now after parcel decode
                if (message_region_) {
                    rma_deb.trace(hpx::debug::mem_crc32(message, message_length,
                        "Message region (receiver delete)"));
                }
            }, nullptr, nullptr);

        rcv_buffer_type buffer(std::move(wrapped_pointer), nullptr);

        LOG_EXCLUSIVE(
            for (chunktype &c : chunks_) {
                LOG_DEBUG_MSG("get : chunk : size " << hpx::debug::hex<4>(c.size_)
                    << "type "   << hpx::debug::dec<>((uint64_t)c.type_)
                    << "rma "    << hpx::debug::ptr(c.rma_)
                    << "cpos "   << hpx::debug::ptr(c.data_.cpos_)
                    << "index "  << hpx::debug::dec<>(c.data_.index_));
        });

        int zc_chunks =
            std::count_if(chunks_.begin(), chunks_.end(), [](chunktype &c) {
                return c.type_ == serialization::chunk_type_pointer ||
                       c.type_ == serialization::chunk_type_rma;
            });
        int oo_chunks = chunks_.size() - zc_chunks;

        buffer.num_chunks_ = std::make_pair(zc_chunks, oo_chunks);
        buffer.data_size_  = header_->message_size();
        performance_counters::parcels::data_point& data = buffer.data_point_;
        data.bytes_ = static_cast<std::size_t>(header_->message_size());
        data.time_ = util::high_resolution_clock::now() - start_time_;

        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "calling parcel decode for ZEROCOPY complete parcel");
        std::size_t num_thread = hpx::get_worker_thread_num();
        decode_message_with_chunks(*parcelport_, std::move(buffer), 0, chunks_, num_thread);
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "parcel decode called for ZEROCOPY complete parcel");

        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this) << "Sending ack");
        send_rdma_complete_ack();

        cleanup_receive();
        FUNC_END_DEBUG_MSG;
    }

    // --------------------------------------------------------------------
    void rma_receiver::send_rdma_complete_ack()
    {
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "RDMA Get tag " << hexuint64(header_->tag())
            << "has completed : posting 8 byte ack to origin");

        ++sent_ack_;

        bool ok = false;
        while(!ok) {
            // when we received the incoming message, the tag was already set
            // with the sender context so that we can signal it directly
            // that we have completed RMA and the sender my now cleanup.
            // Note : fi_inject does not trigger a completion locally, it just
            // sends and then we can reuse buffers and move on.
            std::uint64_t tag = this->header_->tag();
            ssize_t ret = fi_inject(this->endpoint_, &tag,
                sizeof(std::uint64_t), this->src_addr_);
            if (ret==0) {
                ok = true;
            }
            else if (ret == -FI_EAGAIN)
            {
                LOG_ERROR_MSG("receiver " << hpx::debug::ptr(this)
                    << "reposting fi_inject...\n");
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
            else if (ret)
            {
                throw fabric_error(ret, "fi_inject ack notification error");
            }
        }
    }

    // --------------------------------------------------------------------
    void rma_receiver::cleanup_receive()
    {
        LOG_DEBUG_MSG("cleanup for receiver rma " << hpx::debug::ptr(this));
        //
        HPX_ASSERT(rma_count_ == 0);
        //
        ++recv_deletes_;
        //
        memory_pool_->deallocate(header_region_);
        header_region_ = nullptr;
        header_        = nullptr;
        src_addr_      = 0 ;
        //
        if (chunk_region_) {
            memory_pool_->deallocate(chunk_region_);
            chunk_region_  = nullptr;
        }
        //
        if (message_region_) {
            memory_pool_->deallocate(message_region_);
            message_region_ = nullptr;
        }
        //
        for (auto region: rma_regions_) {
            memory_pool_->deallocate(region);
        }
        rma_regions_.clear();
        chunks_.clear();
        //
        LOG_DEBUG_MSG("receiver " << hpx::debug::ptr(this)
            << "Cleaned up, posting self back to rma stack");
        handler_(this);
    }

    // --------------------------------------------------------------------
    void rma_receiver::handle_error(struct fi_cq_err_entry err)
    {
        LOG_ERROR_MSG("rma_receiver handling an error (unimplemented)"
            << hpx::debug::ptr(this));
    }

}}}}
