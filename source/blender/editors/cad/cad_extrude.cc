/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Modal operator that creates a CAD solid by extruding a rectangular profile
 * placed at the 3D cursor, with a live tessellated preview while dragging.
 *
 * NOTE: this first slice deliberately has no gizmo group. The modal drag is
 * the interaction; an arrow gizmo (#GIZMO_GT_arrow_3d) becomes worthwhile once
 * committed solids can be re-edited, which is a later feature.
 */

#include <fmt/format.h>
#include <string>
#include <utility>

#include "MEM_guardedalloc.h"

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "BLI_sys_types.hh"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_cad.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

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
      RNA_float_set(op->ptr, "distance", distance);

      /* Preview failures (e.g. a zero distance mid-drag) keep the last valid mesh. */
      std::string error;
      if (cad_extrude_preview_update(opdata, distance, &error)) {
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

void ED_operatortypes_cad()
{
  WM_operatortype_append(CAD_OT_extrude);
  WM_operatortype_append(CAD_OT_boolean);
}

}  // namespace blender
