# Copyright (c) 2014 Thomas Heller
# Copyright (c) 2014 John Biddiscombe
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# This is the default toolchain file to be used with CNK on a BlueGene/Q. It sets
# the appropriate compile flags and compiler such that HPX will compile.
# Note that you still need to provide Boost, hwloc and other utility libraries
# like a custom allocator yourself.
#

#
# This is a toolchain file setup for building HPX on a BGQ using the clang 
# compiler from the bgclang project. Paths are setup for CSCS usage.
#

#
# cmake -DBoost_COMPILER=-clang35 -DBoost_DEBUG=ON -DHWLOC_ROOT=~/apps/bgq/hwloc-1.8.1/ -DBOOST_ROOT=~/apps/bgq/boost_1_56_0/ -DCMAKE_TOOLCHAIN_FILE=~/src/hpx/cmake/toolchains/BGQ.cmake ~/src/hpx
#

set(CMAKE_SYSTEM_NAME Linux)

# Set the Intel Compiler
set(CMAKE_CXX_COMPILER /gpfs/bbp.cscs.ch/home/biddisco/apps/bgclang/current/bin/bgclang++11)
set(CMAKE_C_COMPILER /gpfs/bbp.cscs.ch/home/biddisco/apps/bgclang/current/bin/bgclang)
set(MPI_CXX_COMPILER /gpfs/bbp.cscs.ch/home/biddisco/apps/bgclang/current/mpi/bgclang/bin/mpiclang++11)

#set(CMAKE_Fortran_COMPILER)
#set(MPI_CXX_COMPILER mpiclang++11)
#set(MPI_C_COMPILER mpiclang)
#set(MPI_Fortran_COMPILER)

set(CMAKE_C_FLAGS_INIT "-std=c++11 -stdlib=libstdc++ " CACHE STRING "")
set(CMAKE_C_COMPILE_OBJECT "<CMAKE_C_COMPILER> -fPIC <DEFINES> <FLAGS> -o <OBJECT> -c <SOURCE>" CACHE STRING "")
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_C_COMPILER> -fPIC -dynamic <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>" CACHE STRING "")
set(CMAKE_C_CREATE_SHARED_LIBRARY "<CMAKE_C_COMPILER> -fPIC -shared <CMAKE_SHARED_LIBRARY_CXX_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> <CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS> <SONAME_FLAG><TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES> " CACHE STRING "")

set(CMAKE_CXX_FLAGS_INIT "" CACHE STRING "")
set(CMAKE_CXX_COMPILE_OBJECT "<CMAKE_CXX_COMPILER> -fPIC <DEFINES> <FLAGS> -o <OBJECT> -c <SOURCE>" CACHE STRING "")
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_CXX_COMPILER> -fPIC -dynamic <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>" CACHE STRING "")
set(CMAKE_CXX_CREATE_SHARED_LIBRARY "<CMAKE_CXX_COMPILER> -fPIC -shared <CMAKE_SHARED_LIBRARY_CXX_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> <CMAKE_SHARED_LIBRARY_CREATE_CXX_FLAGS> <SONAME_FLAG><TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES>" CACHE STRING "")

set(CMAKE_Fortran_FLAGS_INIT "" CACHE STRING "")
set(CMAKE_Fortran_COMPILE_OBJECT "<CMAKE_Fortran_COMPILER> -fPIC <DEFINES> <FLAGS> -o <OBJECT> -c <SOURCE>" CACHE STRING "")
set(CMAKE_Fortran_LINK_EXECUTABLE "<CMAKE_Fortran_COMPILER> -fPIC -dynamic <FLAGS> <CMAKE_Fortran_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_Fortran_CREATE_SHARED_LIBRARY "<CMAKE_Fortran_COMPILER> -fPIC -shared <CMAKE_SHARED_LIBRARY_Fortran_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> <CMAKE_SHARED_LIBRARY_CREATE_Fortran_FLAGS> <SONAME_FLAG><TARGET_SONAME> -o <TARGET> <OBJECTS> <LINK_LIBRARIES> " CACHE STRING "")

# Disable searches in the default system paths. We are cross compiling after all
# and cmake might pick up wrong libraries that way
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# We do a cross compilation here ...
set(CMAKE_CROSSCOMPILING ON CACHE BOOL "")

# Disable generic coroutines (and use posix version)
set(HPX_WITH_GENERIC_CONTEXT_COROUTINES OFF CACHE BOOL "diable generic coroutines")

set(HPX_NATIVE_TLS OFF CACHE BOOL "diisable")

# Set our platform name
set(HPX_PLATFORM "BlueGeneQ" CACHE STRING "")

# Always disable the ibverbs parcelport as it is nonfunctional on the BGQ.
set(HPX_PARCELPORT_IBVERBS OFF CACHE BOOL "")

# Always disable the tcp parcelport as it is nonfunctional on the BGQ.
set(HPX_PARCELPORT_TCP OFF CACHE BOOL "")

# Always enable the tcp parcelport as it is currently the only way to communicate on the BGQ.
set(HPX_PARCELPORT_MPI ON CACHE BOOL "")

# We have a bunch of cores on the BGQ ...
set(HPX_MAX_CPU_COUNT "64" CACHE STRING "")

# We default to tbbmalloc as our allocator on the MIC
if(NOT DEFINED HPX_MALLOC)
  set(HPX_MALLOC "system" CACHE STRING "")
endif()

set(BOOST_ROOT /gpfs/bbp.cscs.ch/home/biddisco/apps/bgq/boost_1_56_0 CACHE STRING "")
set(Boost_COMPILER -clang35)

set(HWLOC_ROOT /gpfs/bbp.cscs.ch/home/biddisco/apps/bgq/hwloc_1.8.1 CACHE STRING "")
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

set(HPX_BUILD_EXAMPLES             ON  CACHE BOOL "enabled by default")
