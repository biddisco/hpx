//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_V2_FABRIC_ENDPOINT_HPP
#define HPX_PARCELSET_V2_FABRIC_ENDPOINT_HPP

#include <hpx/config.hpp>
#include <hpx/runtime/parcelset/v2/parcelset_fwd.hpp>
#include <hpx/util/assert.hpp>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>

#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace hpx { namespace parcelset { inline namespace v2 {
    struct fabric_domain;

    namespace detail {
        struct opaque_address
        {
            void *addr;
            std::size_t addrlen;

            struct hash
            {
                // computes the hash of an address using a variant
                // of the Fowler-Noll-Vo hash function
                // http://www.isthe.com/chongo/tech/comp/fnv/#FNV-1a
                template <std::size_t Bytes>
                struct select_offset
                {
                    // We only support 32 and 64 bit for now
                    static_assert(Bytes == 8 || Bytes == 4);
                    static constexpr std::size_t value =
                        (Bytes == 8) ? 14695981039346656037ull : 2166136261;
                };

                template <std::size_t Bytes>
                struct select_prime
                {
                    // We only support 32 and 64 bit for now
                    static_assert(Bytes == 8 || Bytes == 4);
                    static constexpr std::size_t value =
                        (Bytes == 8) ? 1099511628211 : 16777619;
                };

                std::size_t operator()(opaque_address const& addr) const
                {
                    constexpr std::size_t offset_basis = select_offset<sizeof(std::size_t)>::value;
                    constexpr std::size_t FNV_prime = select_prime<sizeof(std::size_t)>::value;
                    std::size_t result = offset_basis;

                    char* array = reinterpret_cast<char*>(addr.addr);
                    for (std::size_t i = 0; i != addr.addrlen; ++i)
                    {
                        result = result ^ array[i];
                        result = result * FNV_prime;
                    }
                    return result;
                }
            };

        private:
            int memcmp(opaque_address const& other) const
            {
                HPX_ASSERT(other.addrlen == addrlen);
                return std::memcmp(addr, other.addr, addrlen);
            }
            friend bool operator==(opaque_address const& lhs, opaque_address const& rhs)
            {
                return lhs.memcmp(rhs) == 0;
            }
            friend bool operator!=(opaque_address const& lhs, opaque_address const& rhs)
            {
                return lhs.memcmp(rhs) != 0;
            }
            friend bool operator<(opaque_address const& lhs, opaque_address const& rhs)
            {
                return lhs.memcmp(rhs) < 0;
            }
            friend bool operator>(opaque_address const& lhs, opaque_address const& rhs)
            {
                return lhs.memcmp(rhs) > 0;
            }
            friend bool operator<=(opaque_address const& lhs, opaque_address const& rhs)
            {
                return lhs.memcmp(rhs) <= 0;
            }
            friend bool operator>=(opaque_address const& lhs, opaque_address const& rhs)
            {
                return lhs.memcmp(rhs) >= 0;
            }
        };
    }

    struct fabric_endpoint
    {
        explicit fabric_endpoint(const fabric_domain* domain);
        ~fabric_endpoint();

        fabric_endpoint(fabric_endpoint const&) = delete;
        fabric_endpoint& operator=(fabric_endpoint const&) = delete;
        fabric_endpoint(fabric_endpoint&& other);
        fabric_endpoint& operator=(fabric_endpoint&& other);

        void close();

        std::vector<char> get_local_address() const;

        fi_addr_t& add_address(void* addr);
        bool contains_address(void* addr) const;
        fi_addr_t const& lookup_address(void* addr) const;

        std::unordered_map<detail::opaque_address, fi_addr_t, detail::opaque_address::hash> addresses_;
        const fabric_domain* domain_;
        fid_ep* endpoint_;
        fid_cq* txcq_;
        fid_cq* rxcq_;
        fid_av* av_;
        std::uint32_t msg_prefix_size_;
    };
}}}

#endif
