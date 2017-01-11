# Copyright (c) 2007-2013 Hartmut Kaiser
# Copyright (c) 2014-2015 Thomas Heller
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

include(HPX_AddLibrary)

################################################################################
# Decide whether to use the MPI based parcelport
################################################################################
if(HPX_WITH_PARCELPORT_UCX)
  find_package(UCX)

  if(NOT UCX_FOUND)
    hpx_error("UCX could not be found and HPX_WITH_PARCELPORT_UCX=On, please specify UCX_ROOT to point to your UCX installation")
  endif()

  hpx_add_config_define(HPX_HAVE_PARCELPORT_UCX)

  macro(add_parcelport_ucx_module)
    hpx_debug("add_parcelport_ucx_module")

    include_directories(${UCX_INCLUDE_DIRS})

    message(${UCX_LIBRARIES})

    add_parcelport(ucx
      STATIC
      SOURCES
        "${PROJECT_SOURCE_DIR}/plugins/parcelport/ucx/parcelport_ucx.cpp"
        "${PROJECT_SOURCE_DIR}/plugins/parcelport/ucx/active_messages.cpp"
      HEADERS
        "${PROJECT_SOURCE_DIR}/hpx/plugins/parcelport/ucx/sender.hpp"
        "${PROJECT_SOURCE_DIR}/hpx/plugins/parcelport/ucx/locality.hpp"
      DEPENDENCIES
        ${UCX_LIBRARIES}
      FOLDER "Core/Plugins/Parcelport/UCX")
  endmacro()
else()
  macro(add_parcelport_ucx_module)
  endmacro()
endif()
