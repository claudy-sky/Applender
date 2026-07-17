/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Modal operator that creates a CAD solid by extruding a rectangular profile
 * placed at the 3D cursor, with a live tessellated preview while dragging.
 *
 * The raw modal mouse-delta drag (see #cad_extrude_modal) is the primary
 * interaction. In addition, once the operator finishes, a visible draggable
 * arrow gizmo (#GIZMO_GT_arrow_3d, group #VIEW3D_GGT_cad_extrude) is shown on
 * the new solid as a Shapr3D-style explicit handle: dragging it edits the
 * operator's `distance` redo property and re-runs the operator. This mirrors
 * #MESH_GGT_bisect and keeps the modal drag working unchanged as a fallback.
 */

#include <fmt/format.h>
#include <string>
#include <utility>

#include "MEM_guardedalloc.h"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BLI_sys_types.hh"
#include "BLI_utildefines.hh"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_cad.hh"
#include "ED_gizmo_library.hh"
#include "ED_gizmo_utils.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "UI_resources.hh"

#include "OCCT_bridge.hh"

#include "cad_intern.hh"

namespace blender {

/** Initial extrusion distance used for the preview shown before the first mouse move. */
#define CAD_EXTRUDE_INITIAL_DISTANCE 0.01f
/** Precision factor applied to the mouse delta while Shift is held. */
#define CAD_EXTRUDE_PRECISION_FACTOR 0.1f

struct CADExtrudeData {
  /** The immutable source profile, owned for the whole modal interaction. */
  occt::OcctShapeHandle profile;
  /** The preview mesh object created in `invoke` (owned by the scene). */
  Object *object = nullptr;
  /** Mouse position at launch, distance is derived from the vertical delta. */
  int init_mval[2] = {0, 0};
  /** World-space size of one pixel at the 3D cursor (drag sensitivity). */
  float pixel_size = 1.0f;
};

/**
 * Create the (empty) preview mesh object at the 3D cursor. The profile lives at the
 * origin of its own space, the object transform carries the cursor offset.
 */
static Object *cad_extrude_object_add(bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  const View3D *v3d = CTX_wm_view3d(C);
  const ushort local_view_bits = (v3d && v3d->localvd) ? v3d->local_view_uid : 0;
  return ed::object::add_type(
      C, OB_MESH, DATA_("Solid"), scene->cursor.location, nullptr, false, local_view_bits);
}

/**
 * Recompute the extrusion for `distance` and write the tessellation into the preview
 * object's mesh. On failure (e.g. zero distance) the previous mesh is kept.
 */
static bool cad_extrude_preview_update(CADExtrudeData *opdata,
                                       const float distance,
                                       std::string *r_error)
{
  occt::OcctShapeHandle solid = occt::extrude_profile(opdata->profile, distance, r_error);
  if (!solid.is_valid()) {
    return false;
  }
  const occt::TessellatedMesh tessellation = occt::tessellate(
      solid, CAD_TESSELLATE_LINEAR_DEFLECTION, CAD_TESSELLATE_ANGULAR_DEFLECTION);
  CAD_object_mesh_replace(opdata->object, tessellation);
  return true;
}

static void cad_extrude_update_status_text(bContext *C, const float distance)
{
  const std::string header_status = fmt::format("{}: {:.4f} | {}",
                                                IFACE_("CAD Extrude Distance"),
                                                distance,
                                                IFACE_("LMB/Enter: Confirm, Esc/RMB: Cancel"));
  ED_area_status_text(CTX_wm_area(C), header_status.c_str());
}

static void cad_extrude_exit(bContext *C, wmOperator *op)
{
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_status_text(area, nullptr);
  }
  MEM_delete(static_cast<CADExtrudeData *>(op->customdata));
  op->customdata = nullptr;
}

static void cad_extrude_cancel(bContext *C, wmOperator *op)
{
  CADExtrudeData *opdata = static_cast<CADExtrudeData *>(op->customdata);
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);

  /* Remove the preview object created in `invoke`. */
  ed::object::base_free_and_unlink(bmain, scene, opdata->object);
  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);

  cad_extrude_exit(C, op);

  /* Need to force re-display or we may still view the removed preview. */
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
}

/**
 * Link the extrude arrow-gizmo group into the active View3D so the handle appears on the
 * freshly created solid. Called once the operator has committed (see #VIEW3D_GGT_cad_extrude).
 * The group's own poll (#ED_gizmo_poll_or_unlink_delayed_from_operator) removes it again once
 * #CAD_OT_extrude is no longer the last redo operator.
 */
static void cad_extrude_gizmo_group_ensure(bContext *C)
{
  const View3D *v3d = CTX_wm_view3d(C);
  if (v3d && (v3d->gizmo_flag & V3D_GIZMO_HIDE) == 0) {
    WM_gizmo_group_type_ensure("VIEW3D_GGT_cad_extrude");
  }
}

static wmOperatorStatus cad_extrude_confirm(bContext *C, wmOperator *op)
{
  CADExtrudeData *opdata = static_cast<CADExtrudeData *>(op->customdata);
  const float distance = RNA_float_get(op->ptr, "distance");

  /* Final recompute so the committed shape exactly matches the property value. */
  std::string error;
  occt::OcctShapeHandle solid = occt::extrude_profile(opdata->profile, distance, &error);
  if (!solid.is_valid()) {
    BKE_reportf(op->reports, RPT_ERROR, "OCCT extrude failed: %s", error.c_str());
    cad_extrude_cancel(C, op);
    return OPERATOR_CANCELLED;
  }
  const Vector<uint8_t> blob = occt::serialize(solid);
  if (blob.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to serialize the extruded solid");
    cad_extrude_cancel(C, op);
    return OPERATOR_CANCELLED;
  }

  const occt::TessellatedMesh tessellation = occt::tessellate(
      solid, CAD_TESSELLATE_LINEAR_DEFLECTION, CAD_TESSELLATE_ANGULAR_DEFLECTION);
  CAD_object_mesh_replace(opdata->object, tessellation);
  CAD_object_shape_store(opdata->object, blob);

  WM_event_add_notifier(C, NC_GEOM | ND_DATA, opdata->object->data);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, opdata->object);

  cad_extrude_exit(C, op);
  cad_extrude_gizmo_group_ensure(C);
  return OPERATOR_FINISHED;
}

/** Non-modal execution (Python, redo panel): commit directly with the `distance` property. */
static wmOperatorStatus cad_extrude_exec(bContext *C, wmOperator *op)
{
  const float distance = RNA_float_get(op->ptr, "distance");

  occt::OcctShapeHandle profile = occt::make_rect_profile(1.0f, 1.0f);
  if (!profile.is_valid()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to create the rectangle profile");
    return OPERATOR_CANCELLED;
  }
  std::string error;
  occt::OcctShapeHandle solid = occt::extrude_profile(profile, distance, &error);
  if (!solid.is_valid()) {
    BKE_reportf(op->reports, RPT_ERROR, "OCCT extrude failed: %s", error.c_str());
    return OPERATOR_CANCELLED;
  }
  const Vector<uint8_t> blob = occt::serialize(solid);
  if (blob.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to serialize the extruded solid");
    return OPERATOR_CANCELLED;
  }

  Object *ob = cad_extrude_object_add(C);
  const occt::TessellatedMesh tessellation = occt::tessellate(
      solid, CAD_TESSELLATE_LINEAR_DEFLECTION, CAD_TESSELLATE_ANGULAR_DEFLECTION);
  CAD_object_mesh_replace(ob, tessellation);
  CAD_object_shape_store(ob, blob);

  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob->data);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);

  cad_extrude_gizmo_group_ensure(C);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus cad_extrude_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const RegionView3D *rv3d = CTX_wm_region_view3d(C);
  const Scene *scene = CTX_data_scene(C);

  occt::OcctShapeHandle profile = occt::make_rect_profile(1.0f, 1.0f);
  if (!profile.is_valid()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to create the rectangle profile");
    return OPERATOR_CANCELLED;
  }

  CADExtrudeData *opdata = MEM_new<CADExtrudeData>(__func__);
  opdata->profile = std::move(profile);
  opdata->object = cad_extrude_object_add(C);
  opdata->init_mval[0] = event->mval[0];
  opdata->init_mval[1] = event->mval[1];
  /* The size of a pixel under the cursor in 3D space maps mouse movement to distance. */
  opdata->pixel_size = rv3d ? ED_view3d_pixel_size(rv3d, scene->cursor.location) : 1.0f;
  op->customdata = opdata;

  /* Small initial preview so the new solid is visible before the first mouse move. */
  RNA_float_set(op->ptr, "distance", CAD_EXTRUDE_INITIAL_DISTANCE);
  std::string error;
  if (!cad_extrude_preview_update(opdata, CAD_EXTRUDE_INITIAL_DISTANCE, &error)) {
    BKE_reportf(op->reports, RPT_ERROR, "OCCT extrude failed: %s", error.c_str());
    cad_extrude_cancel(C, op);
    return OPERATOR_CANCELLED;
  }

  cad_extrude_update_status_text(C, CAD_EXTRUDE_INITIAL_DISTANCE);

  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus cad_extrude_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  CADExtrudeData *opdata = static_cast<CADExtrudeData *>(op->customdata);

  switch (event->type) {
    case MOUSEMOVE: {
      /* Vertical screen-space drag maps to the extrusion distance. The scale is the
       * world-space size of a pixel at the 3D cursor, Shift enables precision. */
      const float sensitivity = (event->modifier & KM_SHIFT) ? CAD_EXTRUDE_PRECISION_FACTOR :
                                                               1.0f;
      const float distance = float(event->mval[1] - opdata->init_mval[1]) * opdata->pixel_size *
                             sensitivity;

      /* Preview failures (e.g. a zero distance mid-drag) keep the last valid mesh. Only
       * commit the property once the preview succeeded, so confirming always re-extrudes
       * the last successfully previewed distance instead of a rejected one. */
      std::string error;
      if (cad_extrude_preview_update(opdata, distance, &error)) {
        RNA_float_set(op->ptr, "distance", distance);
        WM_event_add_notifier(C, NC_GEOM | ND_DATA, opdata->object->data);
      }
      cad_extrude_update_status_text(C, distance);
      break;
    }
    case LEFTMOUSE:
    case EVT_RETKEY:
    case EVT_PADENTER: {
      if (event->val == KM_PRESS) {
        return cad_extrude_confirm(C, op);
      }
      break;
    }
    case RIGHTMOUSE:
    case EVT_ESCKEY: {
      if (event->val == KM_PRESS) {
        cad_extrude_cancel(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    }
    default: {
      break;
    }
  }

  return OPERATOR_RUNNING_MODAL;
}

void CAD_OT_extrude(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "CAD Extrude";
  ot->description = "Create a CAD solid by extruding a rectangular profile at the 3D cursor";
  ot->idname = "CAD_OT_extrude";

  /* API callbacks. */
  ot->invoke = cad_extrude_invoke;
  ot->modal = cad_extrude_modal;
  ot->exec = cad_extrude_exec;
  ot->cancel = cad_extrude_cancel;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* props */
  RNA_def_float_distance(ot->srna,
                         "distance",
                         1.0f,
                         -1e6f,
                         1e6f,
                         "Distance",
                         "Extrusion distance along the profile's +Z axis",
                         -100.0f,
                         100.0f);
}

/* -------------------------------------------------------------------- */
/** \name Extrude Distance Gizmo (redo affordance)
 *
 * A visible arrow handle shown on the just-created solid after #CAD_OT_extrude finishes.
 * Dragging it edits the operator's `distance` redo property and re-runs the operator via
 * #ED_undo_operator_repeat. Structurally this mirrors #MESH_GGT_bisect: the gizmo group is
 * driven entirely from the last-redo operator (#WM_operator_last_redo), so it does not touch
 * the running modal operator's private state and the raw modal drag stays the fallback.
 * \{ */

/** Per-instance data for one #VIEW3D_GGT_cad_extrude gizmo group. */
struct CADExtrudeGizmoGroup {
  /** The single draggable arrow handle. */
  wmGizmo *arrow;
  /** Context captured at setup, needed to repeat the operator from the property setter. */
  bContext *context;
  /** The extrude operator whose `distance` redo property the arrow edits. */
  wmOperator *op;
  /** Cached lookup of the `distance` property on `op->ptr`. */
  PropertyRNA *prop_distance;
};

static void cad_extrude_gizmo_distance_get(const wmGizmo *gz,
                                           wmGizmoProperty * /*gz_prop*/,
                                           void *value_p)
{
  const CADExtrudeGizmoGroup *ggd = static_cast<CADExtrudeGizmoGroup *>(
      gz->parent_gzgroup->customdata);
  *static_cast<float *>(value_p) = RNA_property_float_get(ggd->op->ptr, ggd->prop_distance);
}

static void cad_extrude_gizmo_distance_set(const wmGizmo *gz,
                                           wmGizmoProperty * /*gz_prop*/,
                                           const void *value_p)
{
  CADExtrudeGizmoGroup *ggd = static_cast<CADExtrudeGizmoGroup *>(gz->parent_gzgroup->customdata);
  RNA_property_float_set(ggd->op->ptr, ggd->prop_distance, *static_cast<const float *>(value_p));

  /* Writing the RNA property does not re-run the operator on its own, so repeat it explicitly
   * (see #MESH_GGT_bisect). Guard against repeating a stale operator. */
  if (ggd->op == WM_operator_last_redo(ggd->context)) {
    ED_undo_operator_repeat(ggd->context, ggd->op);
  }
}

/** Place the arrow at the active object's origin, oriented along its local +Z (extrusion axis). */
static void cad_extrude_gizmo_reposition(const bContext *C, CADExtrudeGizmoGroup *ggd)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  const Object *ob = BKE_view_layer_active_object_get(view_layer);
  if (ob == nullptr) {
    WM_gizmo_set_flag(ggd->arrow, WM_GIZMO_HIDDEN, true);
    return;
  }
  WM_gizmo_set_flag(ggd->arrow, WM_GIZMO_HIDDEN, false);
  WM_gizmo_set_matrix_location(ggd->arrow, ob->object_to_world().location());
  WM_gizmo_set_matrix_rotation_from_z_axis(ggd->arrow, ob->object_to_world().ptr()[2]);
}

static bool cad_extrude_gizmo_poll(const bContext *C, wmGizmoGroupType *gzgt)
{
  /* Show only while #CAD_OT_extrude is the last redo operator, auto-unlink otherwise. */
  return ED_gizmo_poll_or_unlink_delayed_from_operator(C, gzgt, "CAD_OT_extrude");
}

static void cad_extrude_gizmo_setup(const bContext *C, wmGizmoGroup *gzgroup)
{
  wmOperator *op = WM_operator_last_redo(C);
  if (op == nullptr || !STREQ(op->type->idname, "CAD_OT_extrude")) {
    return;
  }

  CADExtrudeGizmoGroup *ggd = MEM_new<CADExtrudeGizmoGroup>(__func__);
  gzgroup->customdata = ggd;
  gzgroup->customdata_free = [](void *data) {
    MEM_delete(static_cast<CADExtrudeGizmoGroup *>(data));
  };

  ggd->context = const_cast<bContext *>(C);
  ggd->op = op;
  ggd->prop_distance = RNA_struct_find_property(op->ptr, "distance");

  ggd->arrow = WM_gizmo_new("GIZMO_GT_arrow_3d", gzgroup, nullptr);
  RNA_enum_set(ggd->arrow->ptr, "transform", ED_GIZMO_ARROW_XFORM_FLAG_CONSTRAINED);
  ui::theme::get_color_3fv(TH_GIZMO_PRIMARY, ggd->arrow->color);
  ui::theme::get_color_3fv(TH_GIZMO_HI, ggd->arrow->color_hi);

  cad_extrude_gizmo_reposition(C, ggd);

  wmGizmoPropertyFnParams params{};
  params.value_get_fn = cad_extrude_gizmo_distance_get;
  params.value_set_fn = cad_extrude_gizmo_distance_set;
  params.range_get_fn = nullptr;
  params.user_data = nullptr;
  WM_gizmo_target_property_def_func(ggd->arrow, "offset", &params);
}

static void cad_extrude_gizmo_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  CADExtrudeGizmoGroup *ggd = static_cast<CADExtrudeGizmoGroup *>(gzgroup->customdata);
  if (ggd == nullptr) {
    return;
  }
  /* A redo replaces the operator instance; re-fetch it (see #MESH_GGT_bisect). */
  if (ggd->op->next) {
    ggd->op = WM_operator_last_redo(ggd->context);
    if (ggd->op != nullptr) {
      ggd->prop_distance = RNA_struct_find_property(ggd->op->ptr, "distance");
    }
  }
  if (ggd->op != nullptr) {
    cad_extrude_gizmo_reposition(C, ggd);
  }
}

void VIEW3D_GGT_cad_extrude(wmGizmoGroupType *gzgt)
{
  gzgt->name = "CAD Extrude Widget";
  gzgt->idname = "VIEW3D_GGT_cad_extrude";

  gzgt->flag = WM_GIZMOGROUPTYPE_3D;

  gzgt->gzmap_params.spaceid = SPACE_VIEW3D;
  gzgt->gzmap_params.regionid = RGN_TYPE_WINDOW;

  gzgt->poll = cad_extrude_gizmo_poll;
  gzgt->setup = cad_extrude_gizmo_setup;
  gzgt->draw_prepare = cad_extrude_gizmo_draw_prepare;
}

/** \} */

void ED_operatortypes_cad()
{
  /* Operators. */
  WM_operatortype_append(CAD_OT_extrude);
  WM_operatortype_append(CAD_OT_boolean);
  WM_operatortype_append(CAD_OT_fillet);
  WM_operatortype_append(CAD_OT_chamfer);
  WM_operatortype_append(CAD_OT_export_step);
  WM_operatortype_append(CAD_OT_export_iges);
  WM_operatortype_append(CAD_OT_import_step);
  WM_operatortype_append(CAD_OT_import_iges);

  /* Gizmo groups. */
  WM_gizmogrouptype_append(VIEW3D_GGT_cad_extrude);
}

}  // namespace blender
