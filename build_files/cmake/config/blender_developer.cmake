# SPDX-FileCopyrightText: 2019-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Configuration for developers, with faster builds, error checking and tests.
#
# Example usage:
#   cmake -C../blender/build_files/cmake/config/blender_developer.cmake  ../blender
#

set(WITH_ASSERT_ABORT                 ON  CACHE BOOL "" FORCE)
set(WITH_ASSERT_RELEASE               ON  CACHE BOOL "" FORCE)
set(WITH_BUILDINFO                    OFF CACHE BOOL "" FORCE)
set(WITH_COMPILER_ASAN                ON  CACHE BOOL "" FORCE)
set(WITH_STRSIZE_DEBUG                ON  CACHE BOOL "" FORCE)
set(WITH_CYCLES_NATIVE_ONLY           ON  CACHE BOOL "" FORCE)
set(WITH_GTESTS                       ON  CACHE BOOL "" FORCE)
set(WITH_LIBMV_SCHUR_SPECIALIZATIONS  OFF CACHE BOOL "" FORCE)
set(WITH_PYTHON_SAFETY                ON  CACHE BOOL "" FORCE)
# For Ninja & Makefile: Generate compile_commands.json. Used by Clang tools (that many common IDEs
# rely on) to obtain include paths, compile flags, etc. from the build system.
set(CMAKE_EXPORT_COMPILE_COMMANDS     ON  CACHE BOOL "" FORCE)
