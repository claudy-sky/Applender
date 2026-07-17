# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  ../include
  ../../makesrna
  ../../../../intern/occt_bridge
)

set(INC_SYS
)

set(SRC
  cad_boolean.cc
  cad_extrude.cc
  cad_idprop.cc

  cad_intern.hh
)

set(LIB
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blentranslation
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::intern::occt_bridge
  PRIVATE bf::windowmanager
)

blender_add_lib(bf_editor_cad "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_dependencies(bf_editor_cad bf_rna)
