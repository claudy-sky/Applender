/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

namespace blender {

/* `cad_extrude.cc` */

/**
 * Register the CAD (OCCT solid modeling) operator types.
 * Only available when built with `WITH_OCCT`, callers must be `#ifdef` gated.
 */
void ED_operatortypes_cad();

}  // namespace blender
