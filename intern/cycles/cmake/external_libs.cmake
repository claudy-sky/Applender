# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

###########################################################################
# Metal
###########################################################################

if(WITH_CYCLES_DEVICE_METAL)
  find_library(METAL_LIBRARY Metal)

  # This file was added in the 12.0 SDK, use it as a way to detect the version.
  if(METAL_LIBRARY)
    if(EXISTS "${METAL_LIBRARY}/Headers/MTLFunctionStitching.h")
      set(METAL_FOUND ON)
    else()
      message(STATUS "Metal version too old, must be SDK 12.0 or newer")
      set(METAL_FOUND OFF)
    endif()
  endif()

  set_and_warn_library_found("Metal" METAL_FOUND WITH_CYCLES_DEVICE_METAL)
  if(METAL_FOUND)
    message(STATUS "Found Metal: ${METAL_LIBRARY}")
  endif()
endif()

###########################################################################
# Embree SYCL (needed for linking to Embree when built with SYCL support)
###########################################################################

if(EMBREE_SYCL_SUPPORT)
  find_package(SYCL)
  find_package(LevelZero)
endif()
