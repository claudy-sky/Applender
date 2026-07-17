/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 */

/* Internal for `cad_xxxx.cc` functions. */

#pragma once

#include <cstdint>
#include <optional>

#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct Object;
struct wmOperatorType;

namespace occt {
struct TessellatedMesh;
}

/**
 * Key of the byte-string IDProperty (stored in `Object::id.properties`) that holds the
 * serialized OCCT B-rep blob for objects created by the CAD operators.
 */
#define CAD_IDPROP_KEY "applender_occt_brep"

/**
 * Tessellation quality used for both the live preview and the committed mesh,
 * see #blender::occt::tessellate. The linear deflection is in model units,
 * the angular deflection in radians.
 */
#define CAD_TESSELLATE_LINEAR_DEFLECTION 0.01f
#define CAD_TESSELLATE_ANGULAR_DEFLECTION 0.5f

/* *** cad_idprop.cc *** */

/**
 * Store `blob` (a serialized OCCT B-rep, see #blender::occt::serialize) on `ob` as a
 * byte-string IDProperty with key #CAD_IDPROP_KEY, replacing any existing one.
 */
void CAD_object_shape_store(Object *ob, Span<const uint8_t> blob);
/**
 * Load the serialized B-rep blob stored on `ob` by #CAD_object_shape_store.
 * Returns `std::nullopt` when the property is missing or has an unexpected type.
 */
std::optional<Vector<uint8_t>> CAD_object_shape_load(const Object *ob);
/** True when `ob` carries a CAD B-rep blob usable by #CAD_object_shape_load. */
bool CAD_object_has_shape(const Object *ob);
/**
 * Replace the mesh geometry of `ob` (must be an #OB_MESH object) with the given
 * tessellation and tag the mesh for depsgraph updates.
 */
void CAD_object_mesh_replace(Object *ob, const occt::TessellatedMesh &tessellation);

/* *** cad_extrude.cc *** */

void CAD_OT_extrude(wmOperatorType *ot);

/* *** cad_boolean.cc *** */

void CAD_OT_boolean(wmOperatorType *ot);

}  // namespace blender
