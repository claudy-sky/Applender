/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

/**
 * Copyright (C) 2001 NaN Technologies B.V.
 */

#include <vector>

#include "GHOST_ISystem.hh"
#include "GHOST_SystemHeadless.hh"

#if defined(__APPLE__)
#  include "GHOST_SystemCocoa.hh"
#endif

#include "CLG_log.h"

static CLG_LogRef LOG = {"ghost.system"};

GHOST_ISystem *GHOST_ISystem::system_ = nullptr;
const char *GHOST_ISystem::system_backend_id_ = nullptr;

GHOST_TBacktraceFn GHOST_ISystem::backtrace_fn_ = nullptr;

bool GHOST_ISystem::use_window_frame_ = true;

GHOST_TSuccess GHOST_ISystem::createSystem(bool verbose, [[maybe_unused]] bool background)
{

  /* When GHOST fails to start, report the back-ends that were attempted.
   * A Verbose argument could be supported in printing isn't always desired. */
  struct GHOST_BackendInfo {
    const char *id = nullptr;
    /** The cause of the failure. */
    std::string failure_msg;
  };
  std::vector<GHOST_BackendInfo> backends_attempted;

  GHOST_TSuccess success;
  if (!system_) {
#if defined(WITH_HEADLESS)
    /* Pass. */
#elif defined(__APPLE__)
    backends_attempted.push_back({"COCOA"});
    CLOG_INFO(&LOG, "Create Cocoa system");
    system_ = new GHOST_SystemCocoa();
#endif

    if (system_) {
      system_backend_id_ = backends_attempted.back().id;
    }
    else if (verbose || CLOG_CHECK(&LOG, CLG_LEVEL_INFO)) {
      bool show_messages = false;
      std::string msg = "Failed to initialize display for back-end(s): [";
      for (int i = 0; i < backends_attempted.size(); i++) {
        const GHOST_BackendInfo &backend_item = backends_attempted[i];
        if (i != 0) {
          msg += ", ";
        }
        msg += "'" + std::string(backend_item.id) + "'";
        if (!backend_item.failure_msg.empty()) {
          show_messages = true;
        }
      }
      msg += "]\n";
      if (show_messages) {
        for (int i = 0; i < backends_attempted.size(); i++) {
          const GHOST_BackendInfo &backend_item = backends_attempted[i];
          msg += "  '";
          msg += backend_item.id;
          msg += "': ";
          msg += backend_item.failure_msg.empty() ? "<unknown>" : backend_item.failure_msg.c_str();
          msg += "\n";
        }
      }
      CLOG_STR_INFO_NOCHECK(&LOG, msg.c_str());
    }
    success = system_ != nullptr ? GHOST_kSuccess : GHOST_kFailure;
  }
  else {
    success = GHOST_kFailure;
  }
  if (success) {
    success = system_->init();
  }
  return success;
}

GHOST_TSuccess GHOST_ISystem::createSystemBackground()
{
  GHOST_TSuccess success;
  if (!system_) {
#if !defined(WITH_HEADLESS)
    /* Try to create a off-screen render surface with the graphical systems. */
    CLOG_INFO(&LOG, "Create background system");
    success = createSystem(false, true);
    if (success) {
      return success;
    }
    /* Try to fall back to headless mode if all else fails. */
#endif
    CLOG_INFO(&LOG, "Create headless system");
    system_ = new GHOST_SystemHeadless();
    success = system_ != nullptr ? GHOST_kSuccess : GHOST_kFailure;
  }
  else {
    success = GHOST_kFailure;
  }
  if (success) {
    success = system_->init();
  }
  return success;
}

GHOST_TSuccess GHOST_ISystem::disposeSystem()
{
  CLOG_DEBUG(&LOG, "Dispose system");
  GHOST_TSuccess success = GHOST_kSuccess;
  if (system_) {
    delete system_;
    system_ = nullptr;
  }
  else {
    success = GHOST_kFailure;
  }
  return success;
}

GHOST_ISystem *GHOST_ISystem::getSystem()
{
  return system_;
}

const char *GHOST_ISystem::getSystemBackend()
{
  return system_backend_id_;
}

GHOST_TBacktraceFn GHOST_ISystem::getBacktraceFn()
{
  return GHOST_ISystem::backtrace_fn_;
}

void GHOST_ISystem::setBacktraceFn(GHOST_TBacktraceFn backtrace_fn)
{
  GHOST_ISystem::backtrace_fn_ = backtrace_fn;
}

bool GHOST_ISystem::getUseWindowFrame()
{
  return GHOST_ISystem::use_window_frame_;
}

void GHOST_ISystem::setUseWindowFrame(bool use_window_frame)
{
  GHOST_ISystem::use_window_frame_ = use_window_frame;
}
