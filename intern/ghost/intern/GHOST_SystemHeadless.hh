/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_SystemHeadless class.
 */

#pragma once

#include "../GHOST_Types.hh"
#include "GHOST_System.hh"
#include "GHOST_WindowNULL.hh"

#include "GHOST_ContextNone.hh"

class GHOST_WindowNULL;

class GHOST_SystemHeadless : public GHOST_System {
 public:
  GHOST_SystemHeadless() : GHOST_System()
  { /* nop */
  }
  ~GHOST_SystemHeadless() override = default;

  bool processEvents(bool /*waitForEvent*/) override
  {
    return false;
  }
  bool setConsoleWindowState(GHOST_TConsoleWindowState /*action*/) override
  {
    return false;
  }
  GHOST_TSuccess getModifierKeys(GHOST_ModifierKeys & /*keys*/) const override
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess getButtons(GHOST_Buttons & /*buttons*/) const override
  {
    return GHOST_kSuccess;
  }
  GHOST_TCapabilityFlag getCapabilities() const override
  {
    return GHOST_TCapabilityFlag(
        GHOST_CAPABILITY_FLAG_ALL &
        /* No windowing functionality supported.
         * In most cases this value doesn't matter for the headless backend.
         *
         * Nevertheless, don't advertise support.
         *
         * NOTE: order the following flags as they they're declared in the source. */
        ~(
            /* Wrap. */
            GHOST_kCapabilityWindowPosition |
            /* Wrap. */
            GHOST_kCapabilityCursorWarp |
            /* Wrap. */
            GHOST_kCapabilityClipboardPrimary |
            /* Wrap. */
            GHOST_kCapabilityClipboardImage |
            /* Wrap. */
            GHOST_kCapabilityDesktopSample |
            /* Wrap. */
            GHOST_kCapabilityInputIME |
            /* Wrap. */
            GHOST_kCapabilityWindowDecorationStyles |
            /* Wrap. */
            GHOST_kCapabilityKeyboardHyperKey |
            /* Wrap. */
            GHOST_kCapabilityCursorRGBA |
            /* Wrap. */
            GHOST_kCapabilityCursorGenerator |
            /* Wrap. */
            GHOST_kCapabilityMultiMonitorPlacement |
            /* Wrap. */
            GHOST_kCapabilityWindowPath)

    );
  }
  char *getClipboard(bool /*selection*/) const override
  {
    return nullptr;
  }
  void putClipboard(const char * /*buffer*/, bool /*selection*/) const override
  { /* nop */
  }
  uint64_t getMilliSeconds() const override
  {
    return 0;
  }
  uint8_t getNumDisplays() const override
  {
    return uint8_t(1);
  }
  GHOST_TSuccess getCursorPosition(int32_t & /*x*/, int32_t & /*y*/) const override
  {
    return GHOST_kFailure;
  }
  GHOST_TSuccess setCursorPosition(int32_t /*x*/, int32_t /*y*/) override
  {
    return GHOST_kFailure;
  }
  void getMainDisplayDimensions(uint32_t & /*width*/, uint32_t & /*height*/) const override
  { /* nop */
  }
  void getAllDisplayDimensions(uint32_t & /*width*/, uint32_t & /*height*/) const override
  { /* nop */
  }
  GHOST_IContext *createOffscreenContext(GHOST_GPUSettings gpu_settings) override
  {
    const GHOST_ContextParams context_params_offscreen =
        GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS_OFFSCREEN(gpu_settings);
    /* This may not be used depending on the build configuration. */
    (void)context_params_offscreen;

    switch (gpu_settings.context_type) {
      default:
        /* Unsupported backend. */
        return nullptr;
    }

    return nullptr;
  }
  GHOST_TSuccess disposeContext(GHOST_IContext *context) override
  {
    delete context;

    return GHOST_kSuccess;
  }

  GHOST_TSuccess init() override
  {
    GHOST_TSuccess success = GHOST_System::init();

    if (success) {
      return GHOST_kSuccess;
    }

    return GHOST_kFailure;
  }

  GHOST_IWindow *createWindow(const char *title,
                              int32_t left,
                              int32_t top,
                              uint32_t width,
                              uint32_t height,
                              GHOST_TWindowState state,
                              GHOST_GPUSettings gpu_settings,
                              const bool /*exclusive*/,
                              const bool /*is_dialog*/,
                              const GHOST_IWindow *parent_window) override
  {
    const GHOST_ContextParams context_params = GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS(
        gpu_settings);
    return new GHOST_WindowNULL(title,
                                left,
                                top,
                                width,
                                height,
                                state,
                                parent_window,
                                gpu_settings.context_type,
                                context_params);
  }

  GHOST_IWindow *getWindowUnderCursor(int32_t /*x*/, int32_t /*y*/) override
  {
    return nullptr;
  }
};
