/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup creator
 */

#ifndef WITH_PYTHON_MODULE

#  include <cerrno>
#  include <cstdlib>

#  if (defined(__APPLE__) && (defined(__i386__) || defined(__x86_64__)))
#    define OSX_SSE_FPE
#    include <xmmintrin.h>
#  endif

#  include "BLI_fileops.hh"
#  include "BLI_path_utils.hh"
#  include "BLI_string.hh"
#  include "BLI_system.hh"
#  include BLI_SYSTEM_PID_H

#  include "BKE_appdir.hh" /* #BKE_tempdir_session_purge. */
#  include "BKE_blender.hh"
#  include "BKE_blender_version.h"
#  include "BKE_global.hh"
#  include "BKE_main.hh"
#  include "BKE_report.hh"
#  include "BKE_wm_runtime.hh"

#  include <csignal>

#  ifdef WITH_PYTHON
#    include "BPY_extern_python.hh" /* #BPY_python_backtrace. */
#  endif

#  include "creator_intern.h" /* Own include. */

namespace blender {

#  if defined(OSX_SSE_FPE)
/**
 * Set breakpoints here when running in debug mode, useful to catch floating point errors.
 */
static void sig_handle_fpe(int /*sig*/)
{
  fprintf(stderr, "debug: SIGFPE trapped\n");
}
#  endif

/* Handling `Ctrl-C` event in the console. */
static void sig_handle_blender_esc(int sig)
{
  /* Forces render loop to read queue, not sure if its needed. */
  G.is_break = true;

  if (sig == 2) {
    static int count = 0;
    if (count) {
      printf("\nBlender killed\n");
      exit(2);
    }
    printf("\nSent an internal break event. Press ^C again to kill Blender\n");
    count++;
  }
}

static void crashlog_file_generate(const char *filepath, const void *os_info)
{
  /* Might be called after WM/Main exit, so needs to be careful about nullptr-checking before
   * de-referencing. */

  wmWindowManager *wm = G_MAIN ? static_cast<wmWindowManager *>(G_MAIN->wm.first) : nullptr;

  FILE *fp;
  char header[512];
  if (!app_state.signal.use_console_crash_handler) {
    printf("Writing: %s\n", filepath);
  }
  fflush(stdout);

#  ifndef BUILD_DATE
  SNPRINTF(header, "# " BLEND_VERSION_FMT ", Unknown revision\n", BLEND_VERSION_ARG);
#  else
  SNPRINTF(header,
           "# " BLEND_VERSION_FMT ", Commit date: %s %s, Hash %s\n",
           BLEND_VERSION_ARG,
           build_commit_date,
           build_commit_time,
           build_hash);
#  endif

  /* Open the crash log. */
  errno = 0;
  if (app_state.signal.use_console_crash_handler) {
    fp = stderr;
  }
  else {
    fp = BLI_fopen(filepath, "wb");
    if (fp == nullptr) {
      fprintf(stderr,
              "Unable to save '%s': %s , falling back to console\n",
              filepath,
              errno ? strerror(errno) : "Unknown error opening file");
      fp = stderr;
    }
  }

  if (wm) {
    BKE_report_write_file_fp(fp, &wm->runtime->reports, header);
  }

  fputs("\n# backtrace\n", fp);
  BLI_system_backtrace_with_os_info(fp, os_info);

#  ifdef WITH_PYTHON
  /* Generate python back-trace if Python is currently active. */
  BPY_python_backtrace(fp);
#  endif
  if (fp != stderr) {
    fclose(fp);
  }
}

static void sig_cleanup_and_terminate(int signum)
{
  /* Delete content of temp directory. */
  BKE_tempdir_session_purge();

  /* Really crash. */
  signal(signum, SIG_DFL);
  kill(getpid(), signum);
}
static void sig_handle_crash_fn(int signum)
{
  char filepath_crashlog[FILE_MAX];
  BKE_blender_globals_crash_path_get(filepath_crashlog);
  crashlog_file_generate(filepath_crashlog, nullptr);
  sig_cleanup_and_terminate(signum);
}

static void sig_handle_abort(int /*signum*/)
{
  /* Delete content of temp directory. */
  BKE_tempdir_session_purge();
}

void main_signal_setup()
{
  if (app_state.signal.use_crash_handler) {
    /* After parsing arguments. */
    signal(SIGSEGV, sig_handle_crash_fn);
  }

  if (app_state.signal.use_abort_handler) {
    signal(SIGABRT, sig_handle_abort);
  }
}

void main_signal_setup_background()
{
  /* for all platforms, even windows has it! */
  BLI_assert(G.background);

  /* Support pressing `Ctrl-C` to close Blender in background-mode.
   * Useful to be able to cancel a render operation. */
  signal(SIGINT, sig_handle_blender_esc);
}

void main_signal_setup_fpe()
{
#  if defined(OSX_SSE_FPE)
  /* Zealous but makes float issues a heck of a lot easier to find!
   * Set breakpoints on #sig_handle_fpe. */
  signal(SIGFPE, sig_handle_fpe);

  /* OSX uses SSE for floating point by default, so here
   * use SSE instructions to throw floating point exceptions. */
  _MM_SET_EXCEPTION_MASK(_MM_MASK_MASK &
                         ~(_MM_MASK_OVERFLOW | _MM_MASK_INVALID | _MM_MASK_DIV_ZERO));
#  endif
}

}  // namespace blender

#endif /* WITH_PYTHON_MODULE */
