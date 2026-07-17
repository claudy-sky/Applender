# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  PUBLIC .
)

set(INC_SYS
  ${OpenCASCADE_INCLUDE_DIR}
)

set(SRC
  intern/OCCT_bridge.cc

  OCCT_bridge.hh
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::intern::guardedalloc

  # `OpenCASCADE_INCLUDE_DIR` / `OpenCASCADE_LIBRARIES` are provided by the
  # top-level `find_package(OpenCASCADE)` performed when `WITH_OCCT` is ON.
  #
  # NOTE: if `OpenCASCADE_LIBRARIES` turns out not to be populated by the
  # installed OCCT CMake config, the per-toolkit libraries
  # TKernel;TKBRep;TKPrim;TKBO;TKMesh;TKBinTools;TKG2d;TKG3d;TKGeomBase;
  # TKGeomAlgo;TKTopAlgo;TKShHealing
  # may need to be listed explicitly here. Verify on first build.
  PRIVATE ${OpenCASCADE_LIBRARIES}
)

blender_add_lib(bf_intern_occt_bridge "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::intern::occt_bridge ALIAS bf_intern_occt_bridge)
