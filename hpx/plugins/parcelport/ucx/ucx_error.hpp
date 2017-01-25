//  Copyright (c) 2017 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_POLICIES_UCX_ERROR_HPP
#define HPX_PARCELSET_POLICIES_UCX_ERROR_HPP

#include <hpx/config.hpp>

#if defined(HPX_HAVE_PARCELPORT_UCX)

#define NVALGRIND
extern "C" {
#include <ucs/type/status.h>
#include <ucs/config/global_opts.h>
#include <uct/api/uct.h>
}

#include <stdexcept>
#include <string>

namespace hpx { namespace parcelset { namespace policies { namespace ucx
{
    struct ucx_error
      : std::runtime_error
    {
        ucx_error(std::string const& file, int line, ucs_status_t status)
          : std::runtime_error(
                file + ":" + std::to_string(line) + ": " + ucs_status_string(status)
            )
        {
        }
    };
}}}}

#define HPX_PARCELPORT_UCX_THROW_IF(status, expected)                           \
    if (status != expected)                                                     \
    {                                                                           \
        throw ucx_error(__FILE__, __LINE__, status);                            \
    }                                                                           \
/**/

#endif
#endif
