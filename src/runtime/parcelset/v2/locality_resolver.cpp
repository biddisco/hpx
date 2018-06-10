//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/runtime/parcelset/v2/fabric_endpoint.hpp>
#include <hpx/runtime/parcelset/v2/parcelhandler.hpp>

namespace hpx { namespace parcelset { inline namespace v2 {

    std::pair<fabric_endpoint*, fi_addr_t> locality_resolver::resolve(std::size_t locality_id, parcelhandler& ph)
    {
        auto it = endpoints_map_.find(locality_id);
        if (it == endpoints_map_.end())
        {

            auto jt = node_names_.find(locality_id);
            if (jt == node_names_.end())
            {
                jt = node_names_.emplace(std::make_pair(locality_id, ph.resolve_name(locality_id))).first;
            }
            //register_address(jt->second)
        }

        return it->second.next();
    }
}}}
