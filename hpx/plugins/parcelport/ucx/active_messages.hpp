//  Copyright (c) 2017 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_POLICIES_UCX_ACTIVE_MESSAGES_HPP
#define HPX_PARCELSET_POLICIES_UCX_ACTIVE_MESSAGES_HPP

#include <hpx/config.hpp>

#if defined(HPX_HAVE_PARCELPORT_UCX)

extern "C" {
#include <ucs/type/status.h>
}

#include <cstddef>

namespace hpx { namespace parcelset { namespace policies { namespace ucx
{
    enum
    {
        connect_message = 0,
        connect_ack_message = 1,
        read_message = 2,
        read_ack_message = 3,
        close_message = 4
    };
}}}}

#endif
#endif
