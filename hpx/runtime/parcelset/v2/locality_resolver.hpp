//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_V2_LOCALITY_RESOLVER_HPP
#define HPX_PARCELSET_V2_LOCALITY_RESOLVER_HPP

#include <hpx/config.hpp>

#include <hpx/runtime/parcelset/v2/parcelset_fwd.hpp>

#include <rdma/fi_endpoint.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hpx { namespace parcelset { inline namespace v2 {

    // locality_endpoints maintains the remote address information for a given
    // locality.
    // The next member function cycles round robin through the endpoints and the
    // available fi_addr_t information to ensure balanced load.
    struct locality_endpoints
    {
        struct entry
        {
            fabric_endpoint* endpoint_;
            std::vector<fi_addr_t> fi_addrs_;
            std::size_t current_fi_addr_ = 0;
            fi_addr_t next()
            {
                fi_addr_t cur = fi_addrs_[current_fi_addr_];
                current_fi_addr_ = (current_fi_addr_ + 1) % fi_addrs_.size();
                return cur;
            }
        };
        std::vector<entry> endpoints_;
        std::size_t current_endpoint_ = 0;

        void insert_address(fabric_endpoint* endpoint, fi_addr_t fi_addr)
        {
            auto it = std::find_if(endpoints_.begin(), endpoints_.end(),
                [endpoint](entry& e) { return endpoint == e.endpoint_; });
            if (it == endpoints_.end())
            {
                endpoints_.emplace_back(entry{endpoint, std::vector<fi_addr_t>{}, 0});
                it = endpoints_.end() - 1;
            }
            it->fi_addrs_.push_back(fi_addr);
        }

        std::pair<fabric_endpoint*, fi_addr_t> next()
        {
            auto& e = endpoints_[current_endpoint_];
            auto res = std::make_pair(e.endpoint_, e.next());
            current_endpoint_ = (current_endpoint_ + 1) % endpoints_.size();
            return res;
        }
    };

    struct locality_resolver
    {
        std::pair<fabric_endpoint*, fi_addr_t> resolve(std::size_t locality_id, parcelhandler& ph);

        std::unordered_map<std::size_t, std::string> node_names_;
        std::unordered_map<std::size_t, locality_endpoints> endpoints_map_;
    };
}}}


#endif
