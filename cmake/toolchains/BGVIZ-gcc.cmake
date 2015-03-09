# Copyright (c) 2014 John Biddiscombe
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

# This is the default toolchain file to be used with CNK on a BlueGene/Q. It sets
# the appropriate compile flags and compiler such that HPX will compile.
# Note that you still need to provide Boost, hwloc and other utility libraries
# like a custom allocator yourself.

#
# Usage : cmake -DCMAKE_TOOLCHAIN_FILE=~/src/hpx/cmake/toolchains/BGION-gcc.cmake ~/src/hpx
#

set(CMAKE_SYSTEM_NAME Linux)

# Set the gcc Compiler
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_C_COMPILER gcc)
#set(CMAKE_Fortran_COMPILER)

# Add flags we need for BGAS compilation 
set(CMAKE_CXX_FLAGS_INIT 
  ""
  CACHE STRING "Initial compiler flags used to compile for BGVIZ"
)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-latomic -lrt" CACHE STRING "BGVIZ flags")

set(CMAKE_C_FLAGS_INIT "" CACHE STRING "BGAS flags")

# We do not perform cross compilation here ...
set(CMAKE_CROSSCOMPILING OFF)

# Set our platform name
set(HPX_PLATFORM "native")

# Disable generic coroutines (and use posix version)
set(HPX_WITH_GENERIC_CONTEXT_COROUTINES ON CACHE BOOL "diable generic coroutines")

# BGAS nodes support ibverbs
set(HPX_PARCELPORT_IBVERBS ON CACHE BOOL "")

# Always disable the tcp parcelport as it is nonfunctional on the BGQ.
set(HPX_PARCELPORT_TCP ON CACHE BOOL "")

# Always enable the tcp parcelport as it is currently the only way to communicate on the BGQ.
set(HPX_PARCELPORT_MPI ON CACHE BOOL "")

# We have a bunch of cores on the A2 processor ...
set(HPX_MAX_CPU_COUNT "12" CACHE STRING "")

# We have no custom malloc yet
if(NOT DEFINED HPX_MALLOC)
  set(HPX_MALLOC "system" CACHE STRING "")
endif()

set(HPX_HIDDEN_VISIBILITY OFF CACHE BOOL "")

#
# Convenience setup for jb @ bbpbg2.cscs.ch
#
set(BOOST_ROOT "/gpfs/bbp.cscs.ch/home/biddisco/apps/viz/boost_1_56_0")
set(HWLOC_ROOT "/gpfs/bbp.cscs.ch/home/biddisco/apps/viz/hwloc-1.8.1")
set(HPX_WITH_HWLOC ON CACHE BOOL "Use hwloc")

set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Default build")

#
# Testing flags
#
set(BUILD_TESTING                  ON  CACHE BOOL "Testing enabled by default")
set(HPX_BUILD_TESTS                ON  CACHE BOOL "Testing enabled by default")
set(HPX_BUILD_TESTS_BENCHMARKS     ON  CACHE BOOL "Testing enabled by default")
set(HPX_BUILD_TESTS_REGRESSIONS    ON  CACHE BOOL "Testing enabled by default")
set(HPX_BUILD_TESTS_UNIT           ON  CACHE BOOL "Testing enabled by default")
set(HPX_BUILD_TESTS_EXTERNAL_BUILD OFF CACHE BOOL "Turn off build of cmake build tests")
set(DART_TESTING_TIMEOUT           45  CACHE STRING "Life is too short")

#HPX_STATIC_LINKING
