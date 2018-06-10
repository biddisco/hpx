//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/runtime/parcelset/v2/fabric_domain.hpp>
#include <hpx/runtime/parcelset/v2/fabric_endpoint.hpp>

#include <hpx/util/assert.hpp>

#include <rdma/fi_cm.h>

#include <iostream>

namespace hpx { namespace parcelset { inline namespace v2 {
    fabric_endpoint::fabric_endpoint(const fabric_domain* domain)
      : domain_(domain),
        endpoint_(nullptr),
        txcq_(nullptr),
        rxcq_(nullptr),
        av_(nullptr),
        msg_prefix_size_(domain->info_->ep_attr->msg_prefix_size)
    {
        fabric_status status;

        // Allocate completion queue for this endpoint
        fi_cq_attr cq_attr = {};
        cq_attr.format = FI_CQ_FORMAT_MSG;
        cq_attr.wait_obj = FI_WAIT_NONE;
        cq_attr.size = domain->info_->tx_attr->size;
        cq_attr.flags = 0;//|= FI_COMPLETION;
        status = fi_cq_open(domain_->domain_, &cq_attr, &txcq_, &txcq_);
        if (!status)
        {
            std::cout << status.what() << '\n';
            throw status;
        }

        cq_attr.size = domain->info_->rx_attr->size;
        status = fi_cq_open(domain_->domain_, &cq_attr, &rxcq_, &rxcq_);
        if (!status)
        {
            std::cout << status.what() << '\n';
            throw status;
        }

        fi_av_attr av_attr = {};
        if (domain->info_->domain_attr->av_type != FI_AV_UNSPEC)
        {
            av_attr.type = domain->info_->domain_attr->av_type;
        }
//             else
//             {
//                 av_attr.type  = FI_AV_MAP;
//                 av_attr.count = N;
//             }
        status = fi_av_open(domain_->domain_, &av_attr, &av_, nullptr);
        if (!status)
        {
            std::cout << status.what() << '\n';
            throw status;
        }

        status = fi_endpoint(domain_->domain_, domain_->info_, &endpoint_, nullptr);
        if (!status)
        {
            std::cout << status.what() << '\n';
            throw status;
        }

        status = fi_ep_bind(endpoint_, &av_->fid, 0);
        if (!status)
        {
            std::cout << status.what() << '\n';
            throw status;
        }

        status = fi_ep_bind(endpoint_, &txcq_->fid, FI_TRANSMIT);
        if (!status)
        {
            std::cout << status.what() << '\n';
            throw status;
        }

        status = fi_ep_bind(endpoint_, &rxcq_->fid, FI_RECV);
        if (!status)
        {
            std::cout << status.what() << '\n';
            throw status;
        }

        status = fi_enable(endpoint_);
        if (!status)
        {
            std::cout << status.what() << '\n';
            throw status;
        }

//         std::size_t addrlen = sizeof(sockaddr_in);
//         HPX_ASSERT(addrlen == domain_->info_->src_addrlen);
//         sockaddr_in addr_in;
//         status = fi_getname(&endpoint_->fid, &addr_in, &addrlen);
//         if (!status)
//         {
//             std::cout << status.what() << '\n';
//             throw status;
//         }
//         char addr[INET_ADDRSTRLEN];
//         inet_ntop(AF_INET, &addr_in.sin_addr, addr, INET_ADDRSTRLEN);
//         std::cout << addr << ':' << ntohs(addr_in.sin_port) << " (" << domain_->provider_<< ")\n";
    }

    fabric_endpoint::~fabric_endpoint()
    {
        close();
    }

    fabric_endpoint::fabric_endpoint(fabric_endpoint&& other)
      : domain_(other.domain_),
        endpoint_(other.endpoint_),
        txcq_(other.txcq_),
        rxcq_(other.rxcq_),
        av_(other.av_),
        msg_prefix_size_(other.msg_prefix_size_)
    {
        other.endpoint_ = nullptr;
        other.txcq_ = nullptr;
        other.rxcq_ = nullptr;
        other.av_ = nullptr;
    }

    fabric_endpoint& fabric_endpoint::operator=(fabric_endpoint&& other)
    {
        close();
        domain_ = other.domain_;
        std::swap(endpoint_, other.endpoint_);
        std::swap(txcq_, other.txcq_);
        std::swap(rxcq_, other.rxcq_);
        std::swap(av_, other.av_);
        msg_prefix_size_ = other.msg_prefix_size_;

        return *this;
    }

    void fabric_endpoint::close()
    {
        if (av_)
        {
            fi_close(&av_->fid);
            av_ = nullptr;
        }
        if (txcq_)
        {
            fi_close(&txcq_->fid);
            txcq_ = nullptr;
        }
        if (rxcq_)
        {
            fi_close(&rxcq_->fid);
            rxcq_ = nullptr;
        }
        if (endpoint_)
        {
            fi_close(&endpoint_->fid);
            endpoint_ = nullptr;
        }
    }
    std::vector<char> fabric_endpoint::get_local_address() const
    {
        std::vector<char> addr(domain_->info_->src_addrlen);
        std::size_t addrlen = addr.size();
        fabric_status status;
        status = fi_getname(&endpoint_->fid, addr.data(), &addrlen);
        if (!status)
        {
            std::cout << status.what() << '\n';
            throw status;
        }
        return addr;
    }

    fi_addr_t& fabric_endpoint::add_address(void* addr)
    {
        detail::opaque_address oaddr{addr, domain_->info_->src_addrlen};
        auto iter = addresses_.find(oaddr);

        if (iter == addresses_.end())
        {
            auto p = addresses_.insert(std::make_pair(oaddr, fi_addr_t{}));
            HPX_ASSERT(p.second);
            fi_addr_t& fi_addr = p.first->second;
            int res = fi_av_insert(av_, addr, 1, &fi_addr, 0, nullptr);
            if (res != 1)
            {
                fabric_status status;
                status = res;
                if (!status)
                    throw status;
            }
            iter = p.first;
        }

        return iter->second;
    }

    bool fabric_endpoint::contains_address(void* addr) const
    {
        detail::opaque_address oaddr{addr, domain_->info_->src_addrlen};
        auto result_iter = addresses_.find(oaddr);

        return result_iter != addresses_.end();
    }

    fi_addr_t const& fabric_endpoint::lookup_address(void* addr) const
    {
        detail::opaque_address oaddr{addr, domain_->info_->src_addrlen};
        auto result_iter = addresses_.find(oaddr);

        if (result_iter == addresses_.end())
            throw fabric_status::eaddrnotavail();

        return result_iter->second;
    }
}}}
