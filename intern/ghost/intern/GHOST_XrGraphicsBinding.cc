/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include <algorithm>
#include <list>
#include <sstream>

#ifdef WITH_METAL_BACKEND
#  include "GHOST_XrGraphicsBindingMetal.hh"
#endif

#include "GHOST_IXrGraphicsBinding.hh"

std::unique_ptr<GHOST_IXrGraphicsBinding> GHOST_XrGraphicsBindingCreateFromType(
    GHOST_TXrGraphicsBinding type, GHOST_Context &context)
{
  switch (type) {
#ifdef WITH_METAL_BACKEND
    case GHOST_kXrGraphicsMetal:
      return std::make_unique<GHOST_XrGraphicsBindingMetal>(context);
#endif
    default:
      return nullptr;
  }

  (void)context; /* Might be unused. */
}
