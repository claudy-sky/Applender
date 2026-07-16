# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
)

set(INC_SYS
)

set(SRC
  mcp_py_api.cc

  mcp_py_api.hh
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::intern::mcp
  PRIVATE bf::dependencies::optional::python
)

blender_add_lib(bf_python_mcp "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
