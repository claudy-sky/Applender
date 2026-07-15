/* SPDX-FileCopyrightText: 2013 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * Definition of GHOST_Context class.
 */

#include "GHOST_Context.hh"

#include <cstdio>
#include <cstring>

GHOST_IContext *GHOST_IContext::getActiveDrawingContext()
{
  return GHOST_Context::getActiveDrawingContext();
}
