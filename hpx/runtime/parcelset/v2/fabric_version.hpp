//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_V2_FABRIC_VERSION_HPP
#define HPX_PARCELSET_V2_FABRIC_VERSION_HPP

#include <rdma/fabric.h>

#include <cstdlib>
#include <iostream>

namespace hpx { namespace parcelset { inline namespace v2 {
    struct fabric_version
    {
        fabric_version()
          : version(fi_version())
        {
        }

        operator std::uint32_t const()
        {
            return version;
        }

        std::uint32_t major() const
        {
            return FI_MAJOR(version);
        }

        std::uint32_t minor() const
        {
            return FI_MINOR(version);
        }

        std::uint32_t version;

        friend std::ostream& operator<<(std::ostream& os, fabric_version const& v)
        {
            os << "libfabric " << v.major() << '.' << v.minor() << " (" << v.version << ')';
            return os;
        }
    };
}}}

#endif
