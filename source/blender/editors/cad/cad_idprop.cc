/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Shared helpers for the CAD editor module: persistent storage of the serialized
 * OCCT B-rep on objects (as a byte-string IDProperty) and conversion of
 * tessellation results into object mesh data.
 */

#include <cstring>

#include "DNA_ID.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_array_utils.hh"
#include "BLI_assert.hh"
#include "BLI_offset_indices.hh"

#include "BKE_idprop.hh"
#include "BKE_mesh.hh"

#include "DEG_depsgraph.hh"

#include "OCCT_bridge.hh"

#include "cad_intern.hh"

namespace blender {

void CAD_object_shape_store(Object *ob, const Span<const uint8_t> blob)
{
  BLI_assert(!blob.is_empty());

  IDProperty *idgroup = IDP_EnsureProperties(&ob->id);

  IDPropertyTemplate val = {};
  val.string.str = reinterpret_cast<const char *>(blob.data());
  /* For #IDP_STRING_SUB_BYTE properties `len` is the exact byte count, no
   * terminating null byte is stored or appended (see #IDP_New). */
  val.string.len = int(blob.size());
  val.string.subtype = IDP_STRING_SUB_BYTE;

  IDProperty *prop = IDP_New(IDP_STRING, &val, CAD_IDPROP_KEY);
  /* Replaces an existing property with the same key (keeping its order in the group). */
  IDP_ReplaceInGroup(idgroup, prop);
}

static const IDProperty *cad_object_shape_prop_get(const Object *ob)
{
  const IDProperty *prop = IDP_GetPropertyFromGroup_null(ob->id.properties, CAD_IDPROP_KEY);
  if (prop == nullptr || prop->type != IDP_STRING || prop->subtype != IDP_STRING_SUB_BYTE ||
      prop->len <= 0)
  {
    return nullptr;
  }
  return prop;
}

std::optional<Vector<uint8_t>> CAD_object_shape_load(const Object *ob)
{
  const IDProperty *prop = cad_object_shape_prop_get(ob);
  if (prop == nullptr) {
    return std::nullopt;
  }
  Vector<uint8_t> blob(prop->len);
  memcpy(blob.data(), prop->data.pointer, size_t(prop->len));
  return blob;
}

bool CAD_object_has_shape(const Object *ob)
{
  return cad_object_shape_prop_get(ob) != nullptr;
}

void CAD_object_mesh_replace(Object *ob, const occt::TessellatedMesh &tessellation)
{
  BLI_assert(ob->type == OB_MESH);

  Mesh *mesh_new = BKE_mesh_new_nomain(int(tessellation.verts.size()),
                                       0,
                                       int(tessellation.tris.size()),
                                       int(tessellation.tris.size()) * 3);
  mesh_new->vert_positions_for_write().copy_from(tessellation.verts);
  offset_indices::fill_constant_group_size(3, 0, mesh_new->face_offsets_for_write());
  array_utils::copy(tessellation.tris.as_span().cast<int>(), mesh_new->corner_verts_for_write());

  bke::mesh_smooth_set(*mesh_new, false);
  bke::mesh_calc_edges(*mesh_new, false, false);

  Mesh *mesh_dst = id_cast<Mesh *>(ob->data);
  BKE_mesh_nomain_to_mesh(mesh_new, mesh_dst, ob);

  DEG_id_tag_update(&mesh_dst->id, ID_RECALC_GEOMETRY);
}

}  // namespace blender
