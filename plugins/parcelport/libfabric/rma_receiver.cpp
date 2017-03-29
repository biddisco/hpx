//  Copyright (c) 2015-2017 John Biddiscombe
//  Copyright (c) 2017      Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <plugins/parcelport/libfabric/rma_receiver.hpp>
#include <plugins/parcelport/libfabric/parcelport.hpp>

#include <hpx/runtime/parcelset/parcel_buffer.hpp>
#include <hpx/runtime/parcelset/decode_parcels.hpp>

#include <hpx/util/detail/yield_k.hpp>

namespace hpx {
namespace parcelset {
namespace policies {
namespace libfabric
{
    rma_receiver::rma_receiver(
        parcelport* pp,
        fid_ep* endpoint,
        rdma_memory_pool* memory_pool,
        completion_handler&& handler)
      : pp_(pp),
        endpoint_(endpoint),
        header_region_(nullptr),
        message_region_(nullptr),
        memory_pool_(memory_pool),
        handler_(std::move(handler)),
        rma_count_(0)
    {}

    rma_receiver::rma_receiver(rma_receiver&& other)
      : pp_(other.pp_),
        endpoint_(other.endpoint_),
        header_region_(nullptr),
        message_region_(nullptr),
        memory_pool_(other.memory_pool_),
        handler_(std::move(other.handler_)),
        rma_count_(static_cast<long>(other.rma_count_))
    {
        HPX_ASSERT(other.header_region_ == nullptr);
        HPX_ASSERT(other.message_region_ == nullptr);
    }

    rma_receiver& rma_receiver::operator=(rma_receiver&& other)
    {
        pp_ = other.pp_;
        endpoint_ = other.endpoint_;
        header_region_ = nullptr;
        message_region_ = nullptr;
        memory_pool_ = other.memory_pool_;
        handler_ = std::move(other.handler_);
        rma_count_ = static_cast<long>(other.rma_count_);
        HPX_ASSERT(other.header_region_ == nullptr);
        HPX_ASSERT(other.message_region_ == nullptr);

        return *this;
    }

    rma_receiver::~rma_receiver()
    {
        LOG_DEVEL_MSG("rma_receiver " << hexpointer(this) << "message complete - destructor");
    }

    void rma_receiver::async_read(
        libfabric_memory_region* region,
        fi_addr_t const& src_addr)
    {
        HPX_ASSERT(rma_count_ == 0);
        HPX_ASSERT(header_region_ == nullptr);
        HPX_ASSERT(message_region_ == nullptr);
        HPX_ASSERT(rma_regions_.size() == 0);
        HPX_ASSERT(region);

        header_region_ = region;
        header_        = reinterpret_cast<header_type*>(header_region_->get_address());
        src_addr_      = src_addr;

        HPX_ASSERT(header_);
        HPX_ASSERT(header_region_->get_address());
        LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
            << "buffsize " << hexuint32(header_->size())
            << ", chunks zerocopy( " << decnumber(header_->num_chunks().first) << ") "
            << ", chunk_flag " << decnumber(header_->header_length())
            << ", normal( " << decnumber(header_->num_chunks().second) << ") "
            << " chunkdata " << decnumber((header_->chunk_data()!=nullptr))
            << " piggyback " << decnumber((header_->piggy_back()!=nullptr))
            << " tag " << hexuint64(header_->tag())
        );

        LOG_DEBUG_MSG(
            CRC32_MEM(header_, header_->header_length(), "Header region (recv)"));

        long rma_count = header_->num_chunks().first;
        if (!header_->piggy_back())
        {
            ++rma_count;
        }
        rma_count_ = rma_count;

        LOG_DEBUG_MSG("rma_receiver " << hexpointer(this)
            << "is expecting " << decnumber(rma_count_) << "read completions");

        // If we have no zero copy chunks and piggy backed data, we can
        // process the message immediately, otherwise, dispatch to rma_receiver
        // If we have neither piggy back, nor zero copy chunks, rma_count is 0
        if (rma_count == 0)
        {
            handle_non_rma();
            return;
        }

        chunks_.resize(header_->num_chunks().first + header_->num_chunks().second);

        char *chunk_data = header_->chunk_data();
        HPX_ASSERT(chunk_data);

        size_t chunkbytes =
            chunks_.size() * sizeof(chunk_struct);
        HPX_ASSERT(chunkbytes == header_->message_header.message_offset);

        std::memcpy(chunks_.data(), chunk_data, chunkbytes);
        LOG_DEBUG_MSG("rma_receiver " << hexpointer(this)
            << "Copied chunk data from header : size "
            << decnumber(chunkbytes));

        LOG_EXCLUSIVE(
        for (const chunk_struct &c : chunks_)
        {
            LOG_DEBUG_MSG("rma_receiver " << hexpointer(this)
                << "recv : chunk : size " << hexnumber(c.size_)
                << " type " << decnumber((uint64_t)c.type_)
                << " rkey " << hexpointer(c.rkey_)
                << " cpos " << hexpointer(c.data_.cpos_)
                << " index " << decnumber(c.data_.index_));
        });

        rma_regions_.reserve(header_->num_chunks().first);

        std::size_t index = 0;
        for (const chunk_struct &c : chunks_)
        {
            if (c.type_ == serialization::chunk_type_pointer)
            {
                libfabric_memory_region *get_region =
                    memory_pool_->allocate_region(c.size_);
                LOG_DEBUG_MSG(
                    CRC32_MEM(get_region->get_address(), c.size_,
                        "(RDMA GET region (new))"));

                LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
                    << "RDMA Get addr " << hexpointer(c.data_.cpos_)
                    << "rkey " << hexpointer(c.rkey_)
                    << "size " << hexnumber(c.size_)
                    << "tag " << hexuint64(header_->tag())
                    << "local addr " << hexpointer(get_region->get_address())
                    << "length " << hexlength(c.size_));

                rma_regions_.push_back(get_region);

                // overwrite the serialization data to account for the
                // local pointers instead of remote ones
                const void *remoteAddr = c.data_.cpos_;
                chunks_[index] =
                    hpx::serialization::create_pointer_chunk(
                        get_region->get_address(), c.size_, c.rkey_);

                // post the rdma read/get
                LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
                    << "RDMA Get fi_read :"
                    << "chunk " << decnumber(index)
                    << "client " << hexpointer(endpoint_)
                    << "fi_addr " << hexpointer(src_addr_)
                    << "local addr " << hexpointer(get_region->get_address())
                    << "local desc " << hexpointer(get_region->get_desc())
                    << "size " << hexnumber(c.size_)
                    << "rkey " << hexpointer(c.rkey_)
                    << "remote cpos " << hexpointer(remoteAddr)
                    << "index " << decnumber(c.data_.index_));

                ssize_t ret = 0;
                for (std::size_t k = 0; true; ++k)
                {
                    uint32_t *dead_buffer = reinterpret_cast<uint32_t*>(get_region->get_address());
                    std::fill(dead_buffer, dead_buffer+c.size_/4, 0x01010101);
                    LOG_DEBUG_MSG(
                        CRC32_MEM(get_region->get_address(), c.size_,
                            "(RDMA GET region (pre-fi_read))"));

                    ret = fi_read(endpoint_,
                        get_region->get_address(), c.size_, get_region->get_desc(),
                        src_addr_,
                        (uint64_t)(remoteAddr), c.rkey_, this);
                    if (ret == -FI_EAGAIN)
                    {
                        LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
                            << "reposting read...\n");
                        hpx::util::detail::yield_k(k,
                            "libfabric::rma_receiver::async_read");
                        continue;
                    }
                    if (ret) throw fabric_error(ret, "fi_read");
                    break;
                }
            }
            ++index;
        }
        char *piggy_back = header_->piggy_back();
        LOG_DEBUG_MSG("rma_receiver " << hexpointer(this)
            << "piggy_back is " << hexpointer(piggy_back)
            << " chunk data is " << hexpointer(header_->chunk_data()));

        // If the main message was not piggy backed
        if (!piggy_back)
        {
            // suspected bug in libfabric : allocated a dummy block before the real one
            // to change the memory address assigned, just in case it is the same as the
            // previous completion. libfabric seems to mis-handle the address and signal
            // a completion without transferring memory

            std::size_t size = header_->size();
            // @TODO : fix this
//            auto dummy = new char[size]; // memory_pool_->allocate_region(size);
            //
            message_region_ = memory_pool_->allocate_region(size);
            // @TODO : fix this
//            delete []dummy; // memory_pool_->deallocate(dummy);
            //
            message_region_->set_message_length(size);
            LOG_EXCLUSIVE(
            uint32_t *dead_buffer = reinterpret_cast<uint32_t*>(message_region_->get_address());
            std::fill(dead_buffer, dead_buffer+size/4, 0xFFFFFFFF);
            LOG_DEBUG_MSG(
                CRC32_MEM(message_region_->get_address(), size,
                    "(RDMA message region (pre-fi_read))"));
            );
            LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
                << "RDMA Get fi_read message :"
                << "client " << hexpointer(endpoint_)
                << "fi_addr " << hexpointer(src_addr_)
                << "local addr " << hexpointer(message_region_->get_address())
                << "local desc " << hexpointer(message_region_->get_desc())
                << "remote addr " << hexpointer(header_->get_message_rdma_addr())
                << "rkey " << hexpointer(header_->get_message_rdma_key())
                << "size " << hexnumber(size)
            );

            ssize_t ret = 0;
            for (std::size_t k = 0; true; ++k)
            {
                ret = fi_read(endpoint_,
                    message_region_->get_address(), size, message_region_->get_desc(),
                    src_addr_,
                    (uint64_t)header_->get_message_rdma_addr(),
                    header_->get_message_rdma_key(), this);
                if (ret == -FI_EAGAIN)
                {
                    LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
                        << "reposting read...\n");
                    hpx::util::detail::yield_k(k,
                        "libfabric::rma_receiver::async_read");
                    continue;
                }
                if (ret) throw fabric_error(ret, "fi_read");
                break;
            }
        }
    }

    void rma_receiver::handle_non_rma()
    {
        typedef pinned_memory_vector<char, header_size> rcv_data_type;
        typedef parcel_buffer<rcv_data_type, std::vector<char>> rcv_buffer_type;

        LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
            << "handle piggy backed sends without zero copy regions");

        HPX_ASSERT(header_);
        char *piggy_back = header_->piggy_back();
        HPX_ASSERT(piggy_back);

        LOG_DEBUG_MSG(
            CRC32_MEM(piggy_back, header_->size(),
                "(Message region recv piggybacked - no rdma)"));

        rcv_data_type wrapped_pointer(piggy_back, header_->size(),
            [this]()
            {
                memory_pool_->deallocate(header_region_);
                header_region_ = nullptr;
                chunks_.clear();
                handler_(this);
            },
            nullptr, nullptr);

        rcv_buffer_type buffer(std::move(wrapped_pointer), nullptr);
        buffer.num_chunks_ = header_->num_chunks();
        buffer.data_size_ = header_->size();
        LOG_DEBUG_MSG("rma_receiver " << hexpointer(this)
            << "calling parcel decode for complete NORMAL parcel");
        std::size_t num_thread = hpx::get_worker_thread_num();
        decode_message_with_chunks(*pp_, std::move(buffer), 1, chunks_, num_thread);
        LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
            << "parcel decode called for complete NORMAL parcel");
    }

    void rma_receiver::handle_read_completion()
    {
        FUNC_START_DEBUG_MSG;
        HPX_ASSERT(rma_count_ > 0);
        // If we haven't read all chunks, we can return and wait
        // for the other incoming read completions
        if (--rma_count_ > 0)
        {
            LOG_DEBUG_MSG("rma_receiver " << hexpointer(this)
                << "Not yet read all RMA regions " << hexpointer(this));
            FUNC_START_DEBUG_MSG;
            return;
        }

        HPX_ASSERT(rma_count_ == 0);
        LOG_DEBUG_MSG("rma_receiver " << hexpointer(this)
            << "all RMA regions now read ");

        std::size_t message_length = header_->size();
        char *message = nullptr;
        if (message_region_)
        {
            message = static_cast<char *>(message_region_->get_address());
            HPX_ASSERT(message);
            HPX_ASSERT(message_region_->get_message_length() == header_->size());
            LOG_DEBUG_MSG("rma_receiver " << hexpointer(this)
                << "No piggy_back RDMA message "
                << "region " << hexpointer(message_region_)
                << "address " << hexpointer(message_region_->get_address())
                << "length " << hexuint32(message_length));

            LOG_DEBUG_MSG(
                CRC32_MEM(message, message_length, "Message region (recv rdma)"));
        }
        else
        {
            HPX_ASSERT(header_->piggy_back());
            message = header_->piggy_back();
            LOG_DEBUG_MSG(CRC32_MEM(message, message_length,
                "Message region (recv piggyback with rdma)"));
        }

        for (auto &r : rma_regions_)
        {
            LOG_DEBUG_MSG(CRC32_MEM(r->get_address(), r->get_message_length(),
                "rdma region (recv) "));
        }

        // wrap the message and chunks into a pinned vector so that they
        // can be passed into the parcel decode functions and when released have
        // the pinned buffers returned to the memory pool
        typedef pinned_memory_vector<char, header_size> rcv_data_type;
        typedef parcel_buffer<rcv_data_type, std::vector<char>> rcv_buffer_type;

        rcv_data_type wrapped_pointer(message, message_length,
            [this, message, message_length]()
            {
                LOG_DEBUG_MSG("rma_receiver " << hexpointer(this)
                    << "wrapped buffer being deleted : rma_receiver ");

                LOG_DEBUG_MSG("rma_receiver " << hexpointer(this) << "Sending ack");
                send_rdma_complete_ack();

                for (auto region: rma_regions_) {
                    memory_pool_->deallocate(region);
                }
                rma_regions_.clear();
                chunks_.clear();

                memory_pool_->deallocate(header_region_);
                header_region_ = nullptr;
                if (message_region_)
                {
                    LOG_DEBUG_MSG(
                        CRC32_MEM(message, message_length, "Message region (rma_receiver delete)"));
                    memory_pool_->deallocate(message_region_);
                    message_region_ = nullptr;
                }
                handler_(this);
            }, nullptr, nullptr);
        //
        rcv_buffer_type buffer(std::move(wrapped_pointer), nullptr);
        LOG_DEBUG_MSG("rma_receiver " << hexpointer(this)
            << "calling parcel decode for complete ZEROCOPY parcel");

        LOG_EXCLUSIVE(
        for (chunk_struct &c : chunks_) {
            LOG_DEBUG_MSG("get : chunk : size " << hexnumber(c.size_)
                    << " type " << decnumber((uint64_t)c.type_)
                    << " rkey " << hexpointer(c.rkey_)
                    << " cpos " << hexpointer(c.data_.cpos_)
                    << " index " << decnumber(c.data_.index_));
        })

        buffer.num_chunks_ = header_->num_chunks();
        buffer.data_size_ = header_->size();
        std::size_t num_thread = hpx::get_worker_thread_num();
        decode_message_with_chunks(*pp_, std::move(buffer), 1, chunks_, num_thread);
        LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
            << "parcel decode called for ZEROCOPY complete parcel");
        FUNC_END_DEBUG_MSG;
    }

    void rma_receiver::send_rdma_complete_ack()
    {

#if HPX_PARCELPORT_LIBFABRIC_IMM_UNSUPPORTED
        LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
            << "RDMA Get tag " << hexuint64(header_->tag())
            << " has completed : posting 8 byte ack to origin");

        int ret = 0;
        for (std::size_t k = 0; true; ++k)
        {
            std::uint64_t tag = header_->tag();
            ret = fi_inject(endpoint_, &tag, sizeof(std::uint64_t), src_addr_);
            if (ret == -FI_EAGAIN)
            {
                LOG_DEVEL_MSG("rma_receiver " << hexpointer(this)
                    << "reposting inject...\n");
                hpx::util::detail::yield_k(k,
                    "libfabric::rma_receiver::send_rdma_complete_ack");
                continue;
            }
            if (ret) throw fabric_error(ret, "fi_inject ack notification error");
            break;
        }

#else
        LOG_DEVEL_MSG("RDMA Get tag " << hexuint64(header_->tag())
            << " has completed : posting zero byte ack to origin");
        std::terminate();
#endif
    }
}}}}
