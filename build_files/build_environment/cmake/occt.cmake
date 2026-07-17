# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# NOTE: unlike the always-built dependencies, OCCT is only built when the
# dependency builder is configured with `-DWITH_OCCT=ON` (see the include
# site in `CMakeLists.txt`). Because of that the source download is
# requested here rather than in `download.cmake`, which runs for every
# dependency unconditionally.
#
# `PACKAGE_USE_UPSTREAM_SOURCES` must remain ON (its default) for OCCT:
# Blender's lib-source mirror does not carry the OCCT tarball, so
# configuring with `-DPACKAGE_USE_UPSTREAM_SOURCES=OFF` rewrites the URI
# to the mirror and the download will 404.
download_source(OCCT)

# NOTE: module names beyond Draw/ApplicationFramework/Visualization/DataExchange
# follow OCCT's `BUILD_MODULE_<name>` pattern, but the exact set needed should be
# re-verified on the first real build. No `add_dependencies()` on `external_tbb`
# is needed since `USE_TBB=OFF`.
set(OCCT_EXTRA_ARGS
  -DBUILD_LIBRARY_TYPE=Static
  -DBUILD_MODULE_Draw=OFF
  -DBUILD_MODULE_ApplicationFramework=OFF
  -DBUILD_MODULE_Visualization=OFF
  # Keep DataExchange: needed later for STEP import/export.
  # Can be turned OFF to shrink the build if STEP support is not wanted.
  -DBUILD_MODULE_DataExchange=ON
  -DBUILD_DOC_Overview=OFF
  -DUSE_TK=OFF
  -DUSE_FREETYPE=OFF
  -DUSE_TBB=OFF
  -DUSE_OPENGL=OFF
  -DUSE_XLIB=OFF
  -DBUILD_CPP_STANDARD=C++17
  -DCMAKE_DEBUG_POSTFIX=_d
)

ExternalProject_Add(external_occt
  URL file://${PACKAGE_DIR}/${OCCT_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${OCCT_HASH_TYPE}=${OCCT_HASH}
  PREFIX ${BUILD_DIR}/occt
  CMAKE_GENERATOR ${PLATFORM_ALT_GENERATOR}
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LIBDIR}/occt
    ${DEFAULT_CMAKE_FLAGS}
    ${OCCT_EXTRA_ARGS}

  INSTALL_DIR ${LIBDIR}/occt
)

if(WIN32)
  ExternalProject_Add_Step(external_occt after_install
    COMMAND
      ${CMAKE_COMMAND} -E copy_directory
      ${LIBDIR}/occt/
      ${HARVEST_TARGET}/occt/
    DEPENDEES install
  )
else()
  # OCCT headers are `.hxx` with a handful of plain `.h` headers.
  harvest(external_occt occt/include occt/include "*.hxx")
  harvest(external_occt occt/include occt/include "*.h")
  harvest(external_occt occt/lib occt/lib "*.a")
  # TODO(on-device): OCCT installs its CMake package config under
  # `lib/cmake/opencascade` -- confirm the exact install sub-path on the
  # first real build and adjust if needed.
  harvest(external_occt occt/lib/cmake/opencascade occt/lib/cmake/opencascade "*.cmake")
endif()
