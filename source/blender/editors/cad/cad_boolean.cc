/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Exec-only operator applying an exact OCCT boolean between two CAD solids
 * (objects carrying a serialized B-rep, see `cad_idprop.cc`). The result
 * replaces the active object; the other operand is consumed and removed.
 */

#include <optional>
#include <string>

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_assert.hh"
#include "BLI_math_matrix_c.hh"

#include "BKE_context.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_object.hh"
#include "ED_screen.hh"

#include "OCCT_bridge.hh"

#include "cad_intern.hh"

namespace blender {

enum eCADBooleanOperation {
  CAD_BOOLEAN_UNION = 0,
  CAD_BOOLEAN_SUBTRACT = 1,
  CAD_BOOLEAN_INTERSECT = 2,
};

static occt::BooleanOp cad_boolean_occt_op(const eCADBooleanOperation operation)
{
  switch (operation) {
    case CAD_BOOLEAN_UNION:
      return occt::BooleanOp::Union;
    case CAD_BOOLEAN_SUBTRACT:
      return occt::BooleanOp::Subtract;
    case CAD_BOOLEAN_INTERSECT:
      return occt::BooleanOp::Intersect;
  }
  BLI_assert_unreachable();
  return occt::BooleanOp::Union;
}

/** Exactly two selected objects (the active one and one other), both carrying a B-rep. */
static bool cad_boolean_poll(bContext *C)
{
  if (!ED_operator_objectmode(C)) {
    return false;
  }
  const Object *ob_active = CTX_data_active_object(C);
  if (ob_active == nullptr) {
    return false;
  }

  int selected_num = 0;
  bool active_selected = false;
  bool all_have_shape = true;
  CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
    selected_num++;
    active_selected |= (ob == ob_active);
    all_have_shape &= CAD_object_has_shape(ob);
  }
  CTX_DATA_END;

  return (selected_num == 2) && active_selected && all_have_shape;
}

static wmOperatorStatus cad_boolean_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Object *ob_active = CTX_data_active_object(C);

  Object *ob_other = nullptr;
  CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
    if (ob != ob_active) {
      ob_other = ob;
    }
  }
  CTX_DATA_END;
  if (ob_active == nullptr || ob_other == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "CAD Boolean requires exactly two selected CAD solids");
    return OPERATOR_CANCELLED;
  }

  const std::optional<Vector<uint8_t>> blob_a = CAD_object_shape_load(ob_active);
  const std::optional<Vector<uint8_t>> blob_b = CAD_object_shape_load(ob_other);
  if (!blob_a || !blob_b) {
    BKE_report(op->reports, RPT_ERROR, "Both objects must carry CAD solid data");
    return OPERATOR_CANCELLED;
  }

  occt::OcctShapeHandle shape_a = occt::deserialize(*blob_a);
  occt::OcctShapeHandle shape_b = occt::deserialize(*blob_b);
  if (!shape_a.is_valid() || !shape_b.is_valid()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to deserialize the stored CAD solid data");
    return OPERATOR_CANCELLED;
  }

  /* Work in world space: bake each object's transform into its shape. */
  shape_a = occt::transformed(shape_a, ob_active->object_to_world().ptr());
  shape_b = occt::transformed(shape_b, ob_other->object_to_world().ptr());
  if (!shape_a.is_valid() || !shape_b.is_valid()) {
    BKE_report(op->reports,
               RPT_ERROR,
               "OCCT: unsupported object transform (shear or non-uniform scale)");
    return OPERATOR_CANCELLED;
  }

  const occt::BooleanOp boolean_operation = cad_boolean_occt_op(
      eCADBooleanOperation(RNA_enum_get(op->ptr, "operation")));
  std::string error;
  occt::OcctShapeHandle result = occt::boolean_op(shape_a, shape_b, boolean_operation, &error);
  if (!result.is_valid()) {
    BKE_reportf(op->reports, RPT_ERROR, "OCCT boolean failed: %s", error.c_str());
    return OPERATOR_CANCELLED;
  }

  const Vector<uint8_t> blob_result = occt::serialize(result);
  if (blob_result.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to serialize the boolean result");
    return OPERATOR_CANCELLED;
  }

  const occt::TessellatedMesh tessellation = occt::tessellate(
      result, CAD_TESSELLATE_LINEAR_DEFLECTION, CAD_TESSELLATE_ANGULAR_DEFLECTION);
  CAD_object_mesh_replace(ob_active, tessellation);

  /* v1 simplification: the boolean result lives in world space, so the active object's
   * transform is reset to identity instead of transforming the result back into the
   * active object's local space. */
  float unit[4][4];
  unit_m4(unit);
  BKE_object_apply_mat4(ob_active, unit, false, false);

  CAD_object_shape_store(ob_active, blob_result);

  /* The second operand is consumed by the boolean, remove it from the scene
   * (same pattern as the join operators). */
  ed::object::base_free_and_unlink(bmain, scene, ob_other);

  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&ob_active->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob_active->data);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);

  return OPERATOR_FINISHED;
}

void CAD_OT_boolean(wmOperatorType *ot)
{
  static const EnumPropertyItem cad_boolean_operation_items[] = {
      {CAD_BOOLEAN_UNION, "UNION", 0, "Union", "Combine the two solids"},
      {CAD_BOOLEAN_SUBTRACT,
       "SUBTRACT",
       0,
       "Subtract",
       "Subtract the other selected solid from the active one"},
      {CAD_BOOLEAN_INTERSECT,
       "INTERSECT",
       0,
       "Intersect",
       "Keep the volume common to both solids"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "CAD Boolean";
  ot->description = "Apply a boolean operation between two CAD solids, replacing the active one";
  ot->idname = "CAD_OT_boolean";

  /* API callbacks. */
  ot->exec = cad_boolean_exec;
  ot->poll = cad_boolean_poll;

  /* props */
  RNA_def_enum(ot->srna,
               "operation",
               cad_boolean_operation_items,
               CAD_BOOLEAN_UNION,
               "Operation",
               "Boolean operation to apply");

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

}  // namespace blender
