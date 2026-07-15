# SPDX-FileCopyrightText: 2006 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# -----------------------------------------------------------------------------
# Shared Thumbnail Extraction Logic

include_directories(
  ../blenlib
  ../blenloader_core
  ../makesdna
  ../../../intern/guardedalloc
)

include_directories(
  SYSTEM
  ${ZLIB_INCLUDE_DIRS}
)

set(SRC
  src/blendthumb.hh
  src/blendthumb_extract.cc
  src/blendthumb_png.cc
)

if(WITH_BUILDINFO)
  list(APPEND SRC
    "$<TARGET_OBJECTS:buildinfoobj>"
  )
endif()

if(APPLE)
  # -----------------------------------------------------------------------------
  # Build `blender-thumbnailer.appex` app extension.
  set(SRC_APPEX
    src/thumbnail_provider.h
    src/thumbnail_provider.mm
  )

  add_executable(blender-thumbnailer MACOSX_BUNDLE ${SRC} ${SRC_APPEX})
  setup_platform_linker_flags(blender-thumbnailer)
  setup_platform_linker_libs(blender-thumbnailer)
  target_link_libraries(blender-thumbnailer
    bf_blenlib
    bf_blenloader_core
    # Avoid linker error about undefined _main symbol.
    "-e _NSExtensionMain"
    "-framework QuickLookThumbnailing"
  )
  # The RPATH here points to the main Blender Resources/lib directory.
  # Avoid duplicating the large `dylibs` (~300MB).
  set_target_properties(blender-thumbnailer PROPERTIES
    INSTALL_RPATH "@loader_path/../../../../Resources/lib"
    # Prevent Xcode from overwriting the signature.
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
  )
  # CMake needs the target defined in the same file as add_custom_command.
  # It needs to be code-signed (ad-hoc in this case)
  # even on developer machine to generate thumbnails.
  # Command taken from XCode build process.
  add_custom_command(
    TARGET blender-thumbnailer POST_BUILD
    COMMAND codesign --deep --force --sign -
      --entitlements "${CMAKE_SOURCE_DIR}/release/darwin/thumbnailer_entitlements.plist"
      --timestamp=none $<TARGET_BUNDLE_DIR:blender-thumbnailer>
  )
endif()
