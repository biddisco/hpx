# Copyright (c) 2014 Thomas Heller
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

# This is the default toolchain file to be used with CNK on a BlueGene/Q. It sets
# the appropriate compile flags and compiler such that HPX will compile.
# Note that you still need to provide Boost, hwloc and other utility libraries
# like a custom allocator yourself.

set(CMAKE_SYSTEM_NAME Linux)

# Set the gcc Compiler
set(CMAKE_CXX_COMPILER g++)
set(CMAKE_C_COMPILER gcc)
#set(CMAKE_Fortran_COMPILER)

# Add flags we need for BGAS compilation 
set(CMAKE_CXX_FLAGS_INIT "-D__powerpc__ -D__bgion__ -I/gpfs/bbp.cscs.ch/home/biddisco/src/bgas/rdmahelper" CACHE STRING "Initial compiler flags used to compile for the Bluegene/Q")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-L/gpfs/bbp.cscs.ch/apps/bgas/tools/gcc/gcc-4.8.2/install/lib64 -latomic -lrt"i CACHE STRING "BGAS flags")

#set(CMAKE_C_FLAGS_INIT "" CACHE STRING "Initial compiler flags used to compile for the Bluegene/Q")
#set(CMAKE_Fortran_FLAGS_INIT "" CACHE STRING "Initial compiler flags used to compile for the Bluegene/Q")

# Disable searches in the default system paths. We are cross compiling after all
# and cmake might pick up wrong libraries that way
#set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM ONLY)
#set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
#set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
#set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# We do a cross compilation here ...
set(CMAKE_CROSSCOMPILING OFF

# Set our platform name
set(HPX_PLATFORM "native")

set(HPX_GENERIC_COROUTINE_CONTEXT OFF CACHE BOOL "diable generic coroutines")

# Always disable the ibverbs parcelport as it is nonfunctional on the BGQ.
set(HPX_PARCELPORT_IBVERBS OFF CACHE BOOL "")

# Always disable the tcp parcelport as it is nonfunctional on the BGQ.
set(HPX_PARCELPORT_TCP ON CACHE BOOL "")

# Always enable the tcp parcelport as it is currently the only way to communicate on the BGQ.
set(HPX_PARCELPORT_MPI ON CACHE BOOL "")

# We have a bunch of cores on the A2 processor ...
set(HPX_MAX_CPU_COUNT "64" CACHE STRING "")

# We have no custom malloc yet
if(NOT DEFINED HPX_MALLOC)
  set(HPX_MALLOC "system" CACHE STRING "")
endif()

set(HPX_HIDDEN_VISIBILITY OFF CACHE BOOL "")

