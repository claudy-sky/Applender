# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

set(INC
  ..
)

set(INC_SYS
)

set(SRC_BASE
  device.cpp
  denoise.cpp
  graphics_interop.cpp
  kernel.cpp
  memory.cpp
  queue.cpp
)

set(SRC_CPU
  cpu/device.cpp
  cpu/device.h
  cpu/device_impl.cpp
  cpu/device_impl.h
  cpu/kernel.cpp
  cpu/kernel.h
  cpu/kernel_function.h
)

set(SRC_DUMMY
  dummy/device.cpp
  dummy/device.h
)

set(SRC_MULTI
  multi/device.cpp
  multi/device.h
)

set(SRC_METAL
  metal/bvh.mm
  metal/bvh.h
  metal/device.mm
  metal/device.h
  metal/device_impl.mm
  metal/device_impl.h
  metal/graphics_interop.mm
  metal/graphics_interop.h
  metal/kernel.mm
  metal/kernel.h
  metal/queue.mm
  metal/queue.h
  metal/util.mm
  metal/util.h
)

set(SRC_HEADERS
  device.h
  denoise.h
  graphics_interop.h
  memory.h
  kernel.h
  queue.h
)

set(SRC
  ${SRC_BASE}
  ${SRC_CPU}
  ${SRC_DUMMY}
  ${SRC_MULTI}
  ${SRC_HEADERS}
)

set(LIB
  PUBLIC cycles_kernel
  PUBLIC cycles_util
  PRIVATE bf::dependencies::optional::openimagedenoise
  PRIVATE bf::dependencies::optional::osl
)

if(WITH_CYCLES_DEVICE_METAL)
  list(APPEND LIB
    PRIVATE ${METAL_LIBRARY}
  )
  list(APPEND SRC
    ${SRC_METAL}
  )
endif()

include_directories(${INC})
include_directories(SYSTEM ${INC_SYS})

cycles_add_library(cycles_device "${LIB}" ${SRC})

source_group("cpu" FILES ${SRC_CPU})
source_group("dummy" FILES ${SRC_DUMMY})
source_group("multi" FILES ${SRC_MULTI})
source_group("metal" FILES ${SRC_METAL})
source_group("common" FILES ${SRC_BASE} ${SRC_HEADERS})
