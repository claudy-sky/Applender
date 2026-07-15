# SPDX-FileCopyrightText: 2017 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# -----------------------------------------------------------------------------
# Build `msgfmt` executable.

set(INC
)

set(SRC
  msgfmt.cc
)

set(LIB
  PRIVATE bf::blenlib
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::dependencies::zlib
  ${PLATFORM_LINKLIBS})

add_c_and_cxx_flags_custom_test(msgfmt)

add_executable(msgfmt ${SRC})
setup_platform_linker_flags(msgfmt)
blender_target_include_dirs(msgfmt ${INC})
target_link_libraries(msgfmt ${LIB})

# If using an external manifest, copy it to the output directory after building.
if(WITH_WINDOWS_EXTERNAL_MANIFEST)
  add_custom_command(TARGET msgfmt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/tests.exe.manifest  $<TARGET_FILE_DIR:msgfmt>/$<TARGET_FILE_NAME:msgfmt>.manifest
  )
endif()
