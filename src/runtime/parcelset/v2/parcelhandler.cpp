//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/runtime/parcelset/v2/parcelhandler.hpp>

namespace hpx { namespace parcelset { inline namespace v2 {
    std::string parcelhandler::resolve_name(std::size_t locality_id)
    {
        auto res = resolver_.resolve(0, *this);

        // TODO: send resolve message, poll and decode...
        HPX_ASSERT(false);

        return "";
    }
}}}
