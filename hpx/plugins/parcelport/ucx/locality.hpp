//  Copyright (c) 2017 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_POLICIES_UCX_LOCALITY_HPP
#define HPX_PARCELSET_POLICIES_UCX_LOCALITY_HPP

#include <hpx/config.hpp>

#if defined(HPX_HAVE_PARCELPORT_UCX)

#include <hpx/runtime/serialization/serialize.hpp>
#include <hpx/runtime/serialization/vector.hpp>

#include <hpx/util/tuple.hpp>

#include <boost/io/ios_state.hpp>

#define NVALGRIND
extern "C" {
#include <uct/api/uct.h>
}

#include <iostream>
#include <string>
#include <vector>

namespace hpx { namespace parcelset { namespace policies { namespace ucx
{
    struct locality
    {
        struct addr
        {
            addr()
              : device_length_(0)
              , iface_length_(0)
            {}

            std::size_t device_length_;
            std::size_t iface_length_;
            std::vector<char> addrs_;

            void set_iface_attr(uct_iface_attr_t const& attr)
            {
                device_length_ = attr.device_addr_len;
                iface_length_ = attr.iface_addr_len;

                addrs_.resize(device_length_ + iface_length_);

                HPX_ASSERT(device_length_ != 0);
                HPX_ASSERT(!addrs_.empty());
                HPX_ASSERT(addrs_.size() == device_length_ + iface_length_);
            }

            const uct_device_addr_t *device_addr() const
            {
                const uct_device_addr_t *device;

                HPX_ASSERT(device_length_ != 0);
                HPX_ASSERT(!addrs_.empty());
                HPX_ASSERT(addrs_.size() == device_length_ + iface_length_);

                device = reinterpret_cast<const uct_device_addr_t *>(
                    addrs_.data());

                return device;
            }

            const uct_iface_addr_t *iface_addr() const
            {
                const uct_iface_addr_t *iface;

                HPX_ASSERT(device_length_ != 0);
                HPX_ASSERT(!addrs_.empty());
                HPX_ASSERT(addrs_.size() == device_length_ + iface_length_);

                iface = reinterpret_cast<const uct_iface_addr_t *>(
                    addrs_.data() + device_length_);

                return iface;
            }

            uct_device_addr_t *device_addr()
            {
                uct_device_addr_t *device;

                HPX_ASSERT(device_length_ != 0);
                HPX_ASSERT(!addrs_.empty());
                HPX_ASSERT(addrs_.size() == device_length_ + iface_length_);

                device = reinterpret_cast<uct_device_addr_t *>(
                    addrs_.data());

                return device;
            }

            uct_iface_addr_t *iface_addr()
            {
                uct_iface_addr_t *iface;

                HPX_ASSERT(device_length_ != 0);
                HPX_ASSERT(!addrs_.empty());
                HPX_ASSERT(addrs_.size() == device_length_ + iface_length_);

                iface = reinterpret_cast<uct_iface_addr_t *>(
                    addrs_.data() + device_length_);

                return iface;
            }

            template <typename Archive>
            void serialize(Archive& ar, unsigned)
            {
                ar & device_length_;
                ar & iface_length_;
                ar & addrs_;
            }
        };

        locality()
        {}

        static const char *type()
        {
            return "ucx";
        }

        explicit operator bool() const HPX_NOEXCEPT
        {
            return !rma_addr_.addrs_.empty() && !am_addr_.addrs_.empty();
        }

        void save(serialization::output_archive & ar) const
        {
            ar & rma_addr_;
            ar & am_addr_;
        }

        void load(serialization::input_archive & ar)
        {
            ar & rma_addr_;
            ar & am_addr_;
        }

        addr& rma_addr()
        {
            return rma_addr_;
        }

        addr const& rma_addr() const
        {
            return rma_addr_;
        }

        addr& am_addr()
        {
            return am_addr_;
        }

        addr const& am_addr() const
        {
            return am_addr_;
        }

    private:
        friend bool operator==(locality const & lhs, locality const & rhs)
        {
            return
                lhs.rma_addr_.addrs_ == rhs.rma_addr_.addrs_ &&
                lhs.am_addr_.addrs_ == rhs.am_addr_.addrs_;
        }

        friend bool operator<(locality const & lhs, locality const & rhs)
        {
            auto lhs_addrs =
                hpx::util::tie(
                    lhs.rma_addr().addrs_,
                    lhs.am_addr().addrs_
                );
            auto rhs_addrs =
                hpx::util::tie(
                    rhs.rma_addr().addrs_,
                    rhs.am_addr().addrs_
                );
            return lhs_addrs < rhs_addrs;
        }

        friend std::ostream & operator<<(std::ostream & os, locality const & loc)
        {
            boost::io::ios_flags_saver ifs(os);

            os << std::hex;

            auto dump = [&os](const char *p, std::size_t length) mutable -> std::ostream&
            {
                if (length == 0)
                {
                    os << "-";
                    return os;
                }

                os << "0x";
                while (length != 0)
                {
                    os << +(*p);
                    ++p;
                    --length;
                }

                return os;
            };

            os << "{";
            os << "rma: { device = ";
            dump((const char *)loc.rma_addr().device_addr(), loc.rma_addr().device_length_);
            os << ", iface = ";
            dump((const char *)loc.rma_addr().iface_addr(), loc.rma_addr().iface_length_);
            os << "}, ";
            os << "am: { device = ";
            dump((const char *)loc.am_addr().device_addr(), loc.am_addr().device_length_);
            os << ", iface = ";
            dump((const char *)loc.am_addr().iface_addr(), loc.am_addr().iface_length_);
            os << "}";
            os << "}";

            return os;
        }

        addr rma_addr_;
        addr am_addr_;
    };
}}}}

#endif
#endif
