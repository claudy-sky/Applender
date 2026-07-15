# SPDX-FileCopyrightText: 2006 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

add_subdirectory(atomic)
add_subdirectory(clog)
add_subdirectory(ghost)
add_subdirectory(guardedalloc)
add_subdirectory(libmv)
add_subdirectory(memutil)
add_subdirectory(opensubdiv)
add_subdirectory(profile)
add_subdirectory(mikktspace)
add_subdirectory(eigen)
add_subdirectory(sky)

add_subdirectory(openvdb)

if(WITH_MOD_REMESH)
  add_subdirectory(dualcon)
endif()

if(WITH_IK_SOLVER)
  add_subdirectory(iksolver)
endif()

if(WITH_IK_ITASC)
  add_subdirectory(itasc)
endif()

if(WITH_CYCLES)
  add_subdirectory(cycles)
endif()

if(WITH_BULLET)
  add_subdirectory(rigidbody)
endif()


if(WITH_MOD_FLUID)
  add_subdirectory(mantaflow)
endif()

if(WITH_UV_SLIM)
  add_subdirectory(slim)
endif()

if(WITH_QUADRIFLOW)
  add_subdirectory(quadriflow)
endif()

if(WITH_DRACO)
  add_subdirectory(draco_bridge)
endif()

if (WITH_MESHOPTIMIZER)
  add_subdirectory(meshoptimizer_bridge)
endif()
