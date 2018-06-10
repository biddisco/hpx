//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_V2_PARCELHANDLER_HPP
#define HPX_PARCELSET_V2_PARCELHANDLER_HPP

#include <hpx/runtime/parcelset/v2/locality_resolver.hpp>
#include <hpx/util/assert.hpp>

#include <cstdint>
#include <string>

namespace hpx { namespace parcelset { inline namespace v2 {
    struct parcelhandler
    {
        static constexpr std::uint16_t base_port = 7910;

        std::string resolve_name(std::size_t locality_id);

        std::size_t locality_id_;
        locality_resolver resolver_;
    };
}}}

#endif
