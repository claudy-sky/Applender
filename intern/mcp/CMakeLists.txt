# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  PUBLIC .
)

set(INC_SYS

)

set(SRC
  intern/MCP_client.cc

  MCP_client.hh
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::dependencies::optional::python
)

blender_add_lib(bf_intern_mcp "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::intern::mcp ALIAS bf_intern_mcp)
