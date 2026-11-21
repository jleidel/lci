# Copyright (c) 2025 The LCI Project Authors
# SPDX-License-Identifier: NCSA

#[=======================================================================[.rst:
FindIBV
----------

Finds libibverbs.

Imported Targets
^^^^^^^^^^^^^^^^

This module provides the following imported targets, if found:

``IBV::IBV``
  The InfiniBand Verbs library

Result Variables
^^^^^^^^^^^^^^^^

``IBV_FOUND``
  Set if libibverbs was found

``IBV_INCLUDE_DIRS``
  Include directories needed to use libibverbs

``IBV_LIBRARIES``
  Libraries needed to link to libibverbs

``IBV_CFLAGS_OTHER``
  Other CFLAGS needed to use libibverbs

Cache Variables
^^^^^^^^^^^^^^^

The following cache variables may also be set:

``IBV_INCLUDE_DIR``
  The include directory for libibverbs

``IBV_LIBRARY``
  Path to the library for libibverbs

#]=======================================================================]

include(FindPackageHandleStandardArgs)
find_package(PkgConfig)

pkg_check_modules(_IBV_PC QUIET libibverbs)

# On macOS (26.2+), Apple ships libibverbs for RDMA over Thunderbolt in the SDK:
# the header is <sysroot>/usr/include/infiniband/verbs.h and the library is
# librdma (a stub re-exporting /usr/lib/rdma/libibverbs.dylib).
set(_IBV_EXTRA_PATHS)
if(APPLE AND CMAKE_OSX_SYSROOT)
  list(APPEND _IBV_EXTRA_PATHS "${CMAKE_OSX_SYSROOT}/usr")
endif()

find_path(
  IBV_INCLUDE_DIR
  NAMES "infiniband/verbs.h"
  PATHS ${_IBV_PC_INCLUDE_DIRS} ${_IBV_EXTRA_PATHS}
  HINTS $ENV{IBV_ROOT} $ENV{LIBIBVERBS_ROOT}
  PATH_SUFFIXES include)
find_library(
  IBV_LIBRARY
  NAMES ibverbs rdma
  PATHS ${_IBV_PC_LIBRARY_DIRS} ${_IBV_EXTRA_PATHS}
  HINTS $ENV{IBV_ROOT} $ENV{LIBIBVERBS_ROOT}
  PATH_SUFFIXES lib)
set(IBV_VERSION ${_IBV_PC_VERSION})

# Apple's librdma only provides arm64/arm64e slices. Make sure the library links
# for the target architecture (e.g., a Rosetta CMake defaults to x86_64; pass
# -DCMAKE_OSX_ARCHITECTURES=arm64).
if(APPLE
   AND IBV_INCLUDE_DIR
   AND IBV_LIBRARY)
  include(CheckCXXSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_INCLUDES ${IBV_INCLUDE_DIR})
  set(CMAKE_REQUIRED_LIBRARIES ${IBV_LIBRARY})
  set(CMAKE_REQUIRED_QUIET ON)
  unset(IBV_LINKS CACHE)
  check_cxx_source_compiles(
    "#include <infiniband/verbs.h>
    int main() { int n = 0; ibv_free_device_list(ibv_get_device_list(&n)); return n; }"
    IBV_LINKS)
  cmake_pop_check_state()
  if(NOT IBV_LINKS)
    message(
      STATUS
        "libibverbs found at ${IBV_LIBRARY} but does not link for the target "
        "architecture (${CMAKE_OSX_ARCHITECTURES}${CMAKE_SYSTEM_PROCESSOR}); "
        "Apple RDMA requires arm64 (-DCMAKE_OSX_ARCHITECTURES=arm64)")
    set(IBV_LIBRARY IBV_LIBRARY-NOTFOUND)
  endif()
endif()

find_package_handle_standard_args(
  IBV
  REQUIRED_VARS IBV_INCLUDE_DIR IBV_LIBRARY
  VERSION_VAR IBV_VERSION)

if(IBV_FOUND)
  set(IBV_INCLUDE_DIRS ${IBV_INCLUDE_DIR})
  set(IBV_LIBRARIES ${IBV_LIBRARY})
  set(IBV_CFLAGS_OTHER ${_IBV_PC_CFLAGS_OTHER})
  if(_IBV_PC_FOUND)
    set(Fabric_IBV_PC_Requires libibverbs)
  else()
    get_filename_component(_IBV_LIB_NAME "${IBV_LIBRARY}" NAME_WE)
    if(_IBV_LIB_NAME STREQUAL "librdma")
      set(Fabric_IBV_PC_Libs "-lrdma")
    else()
      set(Fabric_IBV_PC_Libs "-libverbs")
    endif()
  endif()

  if(NOT TARGET IBV::IBV)
    add_library(IBV::IBV UNKNOWN IMPORTED)
    set_target_properties(
      IBV::IBV
      PROPERTIES IMPORTED_LOCATION "${IBV_LIBRARY}"
                 INTERFACE_INCLUDE_DIRECTORIES "${IBV_INCLUDE_DIR}"
                 INTERFACE_COMPILE_OPTIONS "${_IBV_PC_CFLAGS_OTHER}")
  endif()

  mark_as_advanced(IBV_INCLUDE_DIR IBV_LIBRARY)
endif()
