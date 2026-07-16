/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Interactions with the underlying platform.
 */

#include "WM_api.hh" /* Own include. */

#if defined(__APPLE__)
/* Pass. */
#endif

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Register File Association
 * \{ */

bool WM_platform_associate_set(bool do_register, bool all_users, char **r_error_msg)
{
  bool result = false;
  *r_error_msg = nullptr;
  /* Pass. */
  UNUSED_VARS(do_register, all_users);
  return result;
}

/** \} */

}  // namespace blender
