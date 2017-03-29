//  Copyright (c) 2015-2017 John Biddiscombe
//  Copyright (c) 2017      Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <plugins/parcelport/libfabric/libfabric_memory_region.hpp>
#include <plugins/parcelport/libfabric/rdma_memory_pool.hpp>
#include <plugins/parcelport/libfabric/pinned_memory_vector.hpp>
#include <plugins/parcelport/libfabric/header.hpp>
#include <plugins/parcelport/libfabric/sender.hpp>

#include <hpx/util/high_resolution_timer.hpp>
#include <hpx/util/atomic_count.hpp>
#include <hpx/util/unique_function.hpp>
#include <hpx/util/detail/yield_k.hpp>

#include <rdma/fi_endpoint.h>

#include <memory>

namespace hpx {
namespace parcelset {
namespace policies {
namespace libfabric
{
    void sender::async_write_impl()
    {
        HPX_ASSERT(message_region_ == nullptr);
        HPX_ASSERT(completion_count_ == 0);
        // for each zerocopy chunk, we must create a memory region for the data
        LOG_EXCLUSIVE(
            for (auto &c : buffer_.chunks_) {
            LOG_DEBUG_MSG("write : chunk : size " << hexnumber(c.size_)
                << " type " << decnumber((uint64_t)c.type_)
                << " rkey " << hexpointer(c.rkey_)
                << " cpos " << hexpointer(c.data_.cpos_)
                << " index " << decnumber(c.data_.index_));
        });

        rma_regions_.reserve(buffer_.chunks_.size());

        // for each zerocopy chunk, we must create a memory region for the data
        int index = 0;
        for (auto &c : buffer_.chunks_)
        {
            if (c.type_ == serialization::chunk_type_pointer)
            {
                LOG_EXCLUSIVE(util::high_resolution_timer regtimer);
                libfabric_memory_region *zero_copy_region;
                {
                    // create a memory region from the pointer
                    zero_copy_region = new libfabric_memory_region(
                        domain_, c.data_.cpos_, (std::max)(c.size_,
                            (std::size_t)RDMA_POOL_SMALL_CHUNK_SIZE));
                    LOG_DEBUG_MSG("Time to register memory (ns) "
                        << decnumber(regtimer.elapsed_nanoseconds()));
                }
                c.rkey_  = zero_copy_region->get_remote_key();
                LOG_DEVEL_MSG("Zero-copy rdma Get region "
                    << decnumber(index) << " created for address "
                    << hexpointer(zero_copy_region->get_address())
                    << " and rkey " << hexpointer(c.rkey_));
                rma_regions_.push_back(zero_copy_region);

                LOG_DEBUG_MSG(
                    CRC32_MEM(zero_copy_region->get_address(),
                        zero_copy_region->get_message_length(),
                        "zero_copy_region (send) "));

            }
            index++;
        }

        // create the header in the pinned memory block
        char *header_memory = (char*)(header_region_->get_address());
        LOG_DEBUG_MSG("Placement new for header with piggyback copy disabled");
        header_ = new(header_memory) header_type(buffer_, this);
        header_region_->set_message_length(header_->header_length());

        LOG_DEVEL_MSG("sending, "
            << "sender " << hexpointer(this)
            << ", buffsize " << hexuint32(header_->size())
            << ", header_length " << decnumber(header_->header_length())
            << ", chunks zerocopy( " << decnumber(header_->num_chunks().first) << ") "
            << ", chunk_flag " << decnumber(header_->header_length())
            << ", normal( " << decnumber(header_->num_chunks().second) << ") "
            << ", chunk_flag " << decnumber(header_->header_length())
            << ", tag " << hexuint64(header_->tag())
        );

        // The number of completions we need before cleaning will be
        // 1 (header block send) + 1 (ack message if we have RMA chunks)
        completion_count_ = 1;
        if (rma_regions_.size()>0 || !header_->piggy_back()) {
            ++completion_count_;
        }

        // Get the block of pinned memory where the message was encoded
        // during serialization
        message_region_ = buffer_.data_.m_region_;
        message_region_->set_message_length(header_->size());
        HPX_ASSERT(header_->size() == buffer_.data_.size());
        LOG_DEBUG_MSG("Found region allocated during encode_parcel : address "
            << hexpointer(buffer_.data_.m_array_)
            << " region "<< hexpointer(message_region_));

        // header region is always sent, message region is usually piggybacked
        int num_regions = 1;

        region_list_[0] = { header_region_->get_address(),
                header_region_->get_message_length() };
        region_list_[1] = { message_region_->get_address(),
                message_region_->get_message_length() };

        desc_[0] = header_region_->get_desc();
        desc_[1] = message_region_->get_desc();

        if (header_->chunk_data()) {
            LOG_DEVEL_MSG("Sender " << hexpointer(this)
                << "Chunk info is piggybacked");
        }
        else {
            throw std::runtime_error("@TODO : implement chunk info rdma get "
                "when zero-copy chunks exceed header space");
        }

        if (header_->piggy_back()) {
            LOG_DEVEL_MSG("Sender " << hexpointer(this)
                << "Main message is piggybacked");
            num_regions += 1;

            LOG_DEBUG_MSG(CRC32_MEM(header_region_->get_address(),
                header_region_->get_message_length(),
                "Header region (send piggyback)"));

            LOG_DEBUG_MSG(CRC32_MEM(message_region_->get_address(),
                message_region_->get_message_length(),
                "Message region (send piggyback)"));
        }
        else {
            header_->set_message_rdma_key(message_region_->get_remote_key());
            header_->set_message_rdma_addr(message_region_->get_address());

            LOG_DEVEL_MSG("Sender " << hexpointer(this)
                << "Main message NOT piggybacked " << hexnumber(buffer_.data_.size())
                << "rkey " << hexnumber(message_region_->get_remote_key())
                << "desc " << hexnumber(message_region_->get_desc())
                << "addr " << hexpointer(message_region_->get_address()));

            LOG_DEBUG_MSG(CRC32_MEM(header_region_->get_address(),
                header_region_->get_message_length(),
                "Header region (send)"));

            LOG_DEBUG_MSG(CRC32_MEM(message_region_->get_address(),
                message_region_->get_message_length(),
                "Message region (send rdma fetch)"));
        }

        // send the header/main_chunk to the destination,
        // wr_id is header_region (entry 0 in region_list)
        LOG_DEVEL_MSG("Sender " << hexpointer(this)
            << "fi_send num regions " << decnumber(num_regions)
            << " sender " << hexpointer(this)
            << " client " << hexpointer(endpoint_)
            << " fi_addr " << hexpointer(dst_addr_)
            << " header_region"
            << " buffer " << hexpointer(header_region_->get_address())
            << " region " << hexpointer(header_region_));

        int ret = 0;
        if (num_regions>1)
        {
            LOG_TRACE_MSG("message_region"
              << " buffer " << hexpointer(message_region_->get_address())
              << " region " << hexpointer(message_region_));
            for (std::size_t k = 0; true; ++k)
            {
                ret = fi_sendv(endpoint_, region_list_,
                    desc_, num_regions, dst_addr_, this);
                if (ret == -FI_EAGAIN)
                {
                    LOG_DEVEL_MSG("reposting send...\n");
                    hpx::util::detail::yield_k(k,
                        "libfabric::sender::async_write");
                    continue;
                }
                if (ret) throw fabric_error(ret, "fi_sendv");
                break;
            }
        }
        else
        {
            for (std::size_t k = 0; true; ++k)
            {
                ret = fi_send(endpoint_,
                    region_list_[0].iov_base, region_list_[0].iov_len,
                desc_[0], dst_addr_, this);
                if (ret == -FI_EAGAIN)
                {
                    LOG_DEVEL_MSG("reposting send...\n");
                    hpx::util::detail::yield_k(k,
                        "libfabric::sender::async_write");
                    continue;
                }
                if (ret) throw fabric_error(ret, "fi_sendv");
                break;
            }
        }

        // log the time spent in performance counter
//                buffer.data_point_.time_ =
//                        timer.elapsed_nanoseconds() - buffer.data_point_.time_;

        // parcels_posted_.add_data(buffer.data_point_);
        FUNC_END_DEBUG_MSG;
    }

    void sender::handle_send_completion()
    {
        LOG_DEVEL_MSG("Sender " << hexpointer(this)
            << "handle send_completion "
            << "RMA regions " << decnumber(rma_regions_.size())
            << "completion count " << decnumber(completion_count_));
        cleanup();
    }

    void sender::handle_message_completion_ack()
    {
        LOG_DEVEL_MSG("Sender " << hexpointer(this)
            << "handle handle_message_completion_ack ( "
            << "RMA regions " << decnumber(rma_regions_.size())
            << "completion count " << decnumber(completion_count_));
        cleanup();
    }

    void sender::cleanup()
    {
        LOG_DEBUG_MSG("Sender " << hexpointer(this)
            << "decrementing completion_count from " << completion_count_);
        if (--completion_count_ > 0)
            return;

        error_code ec;
        handler_(ec);

        // Why does deleting the message region cause problems?
        LOG_DEBUG_MSG(
            CRC32_MEM(message_region_->get_address(), message_region_->get_message_length(), "Message region (sender delete)"));
        std::fill(message_region_->get_address(), message_region_->get_address()+message_region_->get_message_length(), 255);

        memory_pool_->deallocate(message_region_);
        message_region_ = nullptr;

        // release all the RMA regions we were holding on to
        for (auto& region: rma_regions_)
        {
            memory_pool_->deallocate(region);
        }
        rma_regions_.clear();
        header_ = nullptr;

        // put this sender back into the free sender's list
        postprocess_handler_(this);
    }

}}}}
