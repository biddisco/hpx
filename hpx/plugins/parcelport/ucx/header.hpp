//  Copyright (c) 2017 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_POLICIES_UCX_HEADER_HPP
#define HPX_PARCELSET_POLICIES_UCX_HEADER_HPP

#include <hpx/config.hpp>

#if defined(HPX_HAVE_PARCELPORT_UCX)

extern "C" {
#include <ucs/type/status.h>
}

namespace hpx { namespace parcelset { namespace policies { namespace ucx
{
    struct header
    {
        // @FIXME: include zero copy chunk rkeys etc...
        typedef std::uint64_t value_type;
        enum data_pos
        {
            pos_size             = 0 * sizeof(value_type),
            pos_numbytes         = 1 * sizeof(value_type),
            pos_numchunks_first  = 2 * sizeof(value_type),
            pos_numchunks_second = 3 * sizeof(value_type),
            pos_piggy_back_flag  = 4 * sizeof(value_type),
            pos_piggy_back_data  = 4 * sizeof(value_type) + 1
        };

        explicit header(uct_md_h pd, std::size_t header_size, std::size_t rpack_length)
          : max_size_(header_size)
          , size_(0)
          , pd_(pd)
          , rkey_(rpack_length)
        {
            // allocate data
            data_ = malloc(max_size_);
            // register it with our PD...
            ucs_status_t status = uct_md_mem_reg(pd_, data_, max_size_, UCT_MD_MEM_FLAG_NONBLOCK, &uct_mem_);
//             ucs_status_t status = uct_md_mem_alloc(
//                 pd_, &max_size_, &data_, UCT_MD_MEM_FLAG_NONBLOCK, "header memory", &uct_mem_);
            if (status != UCS_OK)
            {
                throw std::runtime_error("header failing to allocate data");
            }

            uct_md_mkey_pack(pd_, uct_mem_, rkey_.data());
        }

        ~header()
        {
            if (data_ != nullptr)
                free(data_);

            if (uct_mem_ != nullptr)
                uct_md_mem_dereg(pd_, uct_mem_);
        }

        template <typename Buffer>
        void reset(Buffer const& buffer)
        {
            set<pos_size>(static_cast<value_type>(buffer.size_));
            set<pos_numbytes>(static_cast<value_type>(buffer.data_size_));
            set<pos_numchunks_first>(static_cast<value_type>(buffer.num_chunks_.first));
            set<pos_numchunks_second>(static_cast<value_type>
                (buffer.num_chunks_.second));

            if (buffer.data_.size() <= (max_size_ - pos_piggy_back_data))
            {
                data()[pos_piggy_back_flag] = 1;
                std::memcpy(data() + pos_piggy_back_data, &buffer.data_[0],
                    buffer.data_.size());
                size_ = pos_piggy_back_data + buffer.data_.size();
            }
            else
            {
                data()[pos_piggy_back_flag] = 0;
                size_ = pos_piggy_back_data + sizeof(std::uint64_t) + rkey_.size();
            }
        }

        std::pair<void *, std::size_t> rkey()
        {
            return std::make_pair((void *)rkey_.data(), rkey_.size());
        }

        void reset(std::size_t size)
        {
            size_ = size;
        }

        std::size_t length()
        {
            return size_;
        }

        const char *data() const
        {
            return reinterpret_cast<const char *>(data_);
        }

        char *data()
        {
            return reinterpret_cast<char *>(data_);
        }

        value_type size() const
        {
            return get<pos_size>();
        }

        value_type numbytes() const
        {
            return get<pos_numbytes>();
        }

        std::pair<value_type, value_type> num_chunks() const
        {
            return std::make_pair(get<pos_numchunks_first>(),
                get<pos_numchunks_second>());
        }

        char * piggy_back()
        {
            if(data()[pos_piggy_back_flag])
                return &data()[pos_piggy_back_data];
            return nullptr;
        }

        std::size_t max_size_;
        std::size_t size_;
        uct_md_h pd_;
        void *data_;
        uct_mem_h uct_mem_;
        std::vector<char> rkey_;

        template <std::size_t Pos, typename T>
        void set(T const & t)
        {
            std::memcpy(data() + Pos, &t, sizeof(t));
        }

        template <std::size_t Pos>
        value_type get() const
        {
            value_type res;
            std::memcpy(&res, data() + Pos, sizeof(res));
            return res;
        }
    };
}}}}

#endif
#endif
