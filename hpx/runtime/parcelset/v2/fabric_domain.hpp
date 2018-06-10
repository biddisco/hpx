//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_V2_FABRIC_DOMAIN_HPP
#define HPX_PARCELSET_V2_FABRIC_DOMAIN_HPP

#include <hpx/runtime/parcelset/v2/fabric_status.hpp>
#include <hpx/util/assert.hpp>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>

#include <arpa/inet.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace hpx { namespace parcelset { inline namespace v2 {
    enum class fabric_provider : std::uint8_t
    {
        bgq = 0,
        gni = 1,
        mlx = 2,
        netdir = 3,
        psm = 4,
        psm2 = 5,
        sockets = 6,
        shm = 8,
        udp = 9,
        usnic = 10,
        verbs = 11,
        num = verbs + 1,
        invalid = num + 1
    };

    fabric_provider get_fabric_provider(fi_info* info)
    {
        std::string provider(info->fabric_attr->prov_name);

        if (provider == "bgq")
        {
            return fabric_provider::bgq;
        }
        if (provider == "gni")
        {
            return fabric_provider::gni;
        }
        if (provider == "mlx")
        {
            return fabric_provider::mlx;
        }
        if (provider == "netdir")
        {
            return fabric_provider::netdir;
        }
        if (provider == "psm")
        {
            return fabric_provider::psm;
        }
        if (provider == "psm2")
        {
            return fabric_provider::psm2;
        }
        if (provider == "sockets")
        {
            return fabric_provider::sockets;
        }
        if (provider == "shm")
        {
            return fabric_provider::shm;
        }
        if (provider == "udp")
        {
            return fabric_provider::udp;
        }
        if (provider == "usnic")
        {
            return fabric_provider::usnic;
        }
        if (provider == "verbs")
        {
            return fabric_provider::verbs;
        }

        HPX_ASSERT(false);

        return fabric_provider::num;
    }

    const char* fabric_provider_name(fabric_provider prov)
    {
        switch (prov)
        {
            case fabric_provider::bgq:
            {
                return "bgq";
            }
            case fabric_provider::gni:
            {
                return "gni";
            }
            case fabric_provider::mlx:
            {
                return "mlx";
            }
            case fabric_provider::netdir:
            {
                return "netdir";
            }
            case fabric_provider::psm:
            {
                return "psm";
            }
            case fabric_provider::psm2:
            {
                return "psm2";
            }
            case fabric_provider::sockets:
            {
                return "sockets";
            }
            case fabric_provider::shm:
            {
                return "shm";
            }
            case fabric_provider::udp:
            {
                return "udp";
            }
            case fabric_provider::usnic:
            {
                return "usnic";
            }
            case fabric_provider::verbs:
            {
                return "verbs";
            }
            default:
                return "unkown";
        }
    }

    std::ostream& operator<<(std::ostream& os, fabric_provider prov)
    {
        os << fabric_provider_name(prov);
        return os;
    }

    struct fabric_domain
    {
        fabric_domain(fabric_domain const&) = delete;
        fabric_domain& operator=(fabric_domain const&) = delete;

        explicit fabric_domain(fi_info* info, std::uint16_t port)
          : fabric_(nullptr),
            eq_(nullptr),
            domain_(nullptr),
            info_(fi_dupinfo(info)),
            name_(info->fabric_attr->name),
            provider_(get_fabric_provider(info))
        {
            if (info->addr_format == FI_SOCKADDR_IN)
            {
                sockaddr_in *addr =
                    reinterpret_cast<sockaddr_in*>(info_->src_addr);
                addr->sin_port = htons(port);
            }
            if (info->addr_format == FI_SOCKADDR_IN6)
            {
                sockaddr_in6 *addr =
                    reinterpret_cast<sockaddr_in6*>(info_->src_addr);
                addr->sin6_port = htons(port);
            }

            fabric_status status;
            status = fi_fabric(info->fabric_attr, &fabric_, nullptr);
            if (!status)
                throw status;

//             fi_eq_attr eq_attr = {};
//             eq_attr.wait_obj = FI_WAIT_UNSPEC;
//             status = fi_eq_open(fabric_, &eq_attr, &eq_, nullptr);
//             if (!status)
//                 throw status;

            status = fi_domain(fabric_, info, &domain_, nullptr);
            if (!status)
                throw status;

        }

        ~fabric_domain()
        {
            close();
        }

        fabric_domain(fabric_domain&& other)
          : fabric_(other.fabric_),
            eq_(other.eq_),
            domain_(other.domain_),
            info_(other.info_),
            name_(std::move(other.name_)),
            provider_(std::move(other.provider_))
        {
            other.fabric_ = nullptr;
            other.eq_ = nullptr;
            other.domain_ = nullptr;
            other.info_ = nullptr;
        }

        fabric_domain& operator=(fabric_domain&& other)
        {
            close();
            std::swap(fabric_, other.fabric_);
            std::swap(eq_, other.eq_);
            std::swap(domain_, other.domain_);
            std::swap(info_, other.info_);
            name_ = std::move(other.name_);
            provider_ = std::move(other.provider_);
            return *this;
        }

        void close()
        {
            if (fabric_)
            {
                std::cout << "Closing " << this << '\n';
                fi_close(&fabric_->fid);
                fabric_ = nullptr;
            }
            if (eq_)
            {
                fi_close(&eq_->fid);
                eq_ = nullptr;
            }
            if (domain_)
            {
                fi_close(&domain_->fid);
                domain_ = nullptr;
            }
            if (info_)
            {
                fi_freeinfo(info_);
                info_ = nullptr;
            }
        }

        fid_fabric *fabric_;
        fid_eq *eq_;
        fid_domain *domain_;
        fi_info *info_;
        std::string name_;
        fabric_provider provider_;
    };
}}}

#endif
