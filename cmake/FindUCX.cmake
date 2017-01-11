# Copyright (c) 2014-2017 Thomas Heller
# Copyright (c) 2007-2012 Hartmut Kaiser
# Copyright (c) 2010-2011 Matt Anderson
# Copyright (c) 2011      Bryce Lelbach
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

find_package(PkgConfig)
pkg_check_modules(PC_UCX QUIET ucx)

find_path(UCX_INCLUDE_DIR uct/api/uct.h
  HINTS
  ${UCX_ROOT} ENV UCX_ROOT
  ${PC_UCX_INCLUDEDIR}
  ${PC_UCX_INCLUDE_DIRS}
  PATH_SUFFIXES include)

find_library(UCX_UCM_LIBRARY NAMES ucm libucm
  HINTS
    ${UCX_ROOT} ENV UCX_ROOT
    ${PC_UCX_LIBDIR}
    ${PC_UCX_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64)

find_library(UCX_UCP_LIBRARY NAMES ucp libucp
  HINTS
    ${UCX_ROOT} ENV UCX_ROOT
    ${PC_UCX_LIBDIR}
    ${PC_UCX_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64)

find_library(UCX_UCS_LIBRARY NAMES ucs libucs
  HINTS
    ${UCX_ROOT} ENV UCX_ROOT
    ${PC_UCX_LIBDIR}
    ${PC_UCX_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64)

find_library(UCX_UCT_LIBRARY NAMES uct libuct
  HINTS
    ${UCX_ROOT} ENV UCX_ROOT
    ${PC_UCX_LIBDIR}
    ${PC_UCX_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64)

set(UCX_LIBRARIES
  ${UCX_UCM_LIBRARY}
  ${UCX_UCP_LIBRARY}
  ${UCX_UCT_LIBRARY}
  ${UCX_UCS_LIBRARY}
  CACHE INTERNAL "")
set(UCX_INCLUDE_DIRS ${UCX_INCLUDE_DIR} CACHE INTERNAL "")

find_package_handle_standard_args(UCX DEFAULT_MSG
  UCX_UCM_LIBRARY
  UCX_UCP_LIBRARY
  UCX_UCS_LIBRARY
  UCX_UCT_LIBRARY
  UCX_INCLUDE_DIR)

foreach(v UCX_ROOT)
  get_property(_type CACHE ${v} PROPERTY TYPE)
  if(_type)
    set_property(CACHE ${v} PROPERTY ADVANCED 1)
    if("x${_type}" STREQUAL "xUNINITIALIZED")
      set_property(CACHE ${v} PROPERTY TYPE PATH)
    endif()
  endif()
endforeach()

mark_as_advanced(
  UCX_ROOT
  UCX_UCM_LIBRARY
  UCX_UCP_LIBRARY
  UCX_UCS_LIBRARY
  UCX_UCT_LIBRARY
  UCX_INCLUDE_DIR)
