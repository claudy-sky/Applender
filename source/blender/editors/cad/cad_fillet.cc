/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Modal operators that round (fillet) or bevel (chamfer) *all* edges of an
 * existing CAD solid with a uniform radius / setback, with a live tessellated
 * preview while dragging. Unlike #CAD_OT_extrude these edit the active object
 * in place: the object already exists, so cancelling restores its original
 * mesh and B-rep instead of deleting it.
 *
 * NOTE: v1 deliberately fillets/chamfers every edge (no per-edge selection),
 * which sidesteps the topological-naming problem, mirroring the bridge's
 * #blender::occt::fillet_all_edges / #blender::occt::chamfer_all_edges.
 */

#include <algorithm>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <utility>

#include "MEM_guardedalloc.h"

#include "DNA_object_types.h"

#include "BLI_sys_types.hh"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_report.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "OCCT_bridge.hh"

#include "cad_intern.hh"

namespace blender {

/** Initial radius/distance used for the preview shown before the first mouse move. */
#define CAD_FILLET_INITIAL_RADIUS 0.01f
/** Precision factor applied to the mouse delta while Shift is held. */
#define CAD_FILLET_PRECISION_FACTOR 0.1f
/** Small strictly-positive floor: a zero radius/distance is a no-op OCCT rejects. */
#define CAD_FILLET_MIN_RADIUS 1e-4f

/** Which OCCT edge operation a modal instance drives. */
enum class CADFilletMode {
  Fillet,
  Chamfer,
};

struct CADFilletData {
  /** The immutable source solid (the object's original B-rep), owned for the whole drag. */
  occt::OcctShapeHandle source;
  /** The active object being edited in place (owned by the scene, never deleted here). */
  Object *object = nullptr;
  /** The object's original serialized B-rep, kept so cancel/failure can restore it. */
  Vector<uint8_t> original_blob;
  /** Mouse position at launch; the radius/distance derives from the vertical delta. */
  int init_mval[2] = {0, 0};
  /** World-space size of one pixel at the object origin (drag sensitivity). */
  float pixel_size = 1.0f;
  /** Fillet vs chamfer. */
  CADFilletMode mode = CADFilletMode::Fillet;
};

/** RNA property name that carries the committed value for `mode`. */
static const char *cad_fillet_prop_name(const CADFilletMode mode)
{
  return (mode == CADFilletMode::Fillet) ? "radius" : "distance";
}

/** Apply the mode's OCCT operation to the source at `value`. */
static occt::OcctShapeHandle cad_fillet_apply(const CADFilletData *opdata,
                                              const float value,
                                              std::string *r_error)
{
  if (opdata->mode == CADFilletMode::Fillet) {
    return occt::fillet_all_edges(opdata->source, value, r_error);
  }
  return occt::chamfer_all_edges(opdata->source, value, r_error);
}

/**
 * Recompute the fillet/chamfer for `value` and write the tessellation into the
 * object's mesh. On failure (e.g. a radius larger than the geometry allows) the
 * previous mesh is kept.
 */
static bool cad_fillet_preview_update(CADFilletData *opdata,
                                      const float value,
                                      std::string *r_error)
{
  occt::OcctShapeHandle solid = cad_fillet_apply(opdata, value, r_error);
  if (!solid.is_valid()) {
    return false;
  }
  const occt::TessellatedMesh tessellation = occt::tessellate(
      solid, CAD_TESSELLATE_LINEAR_DEFLECTION, CAD_TESSELLATE_ANGULAR_DEFLECTION);
  CAD_object_mesh_replace(opdata->object, tessellation);
  return true;
}

/** Re-tessellate the untouched source and restore the original B-rep on the object. */
static void cad_fillet_restore_original(CADFilletData *opdata)
{
  const occt::TessellatedMesh tessellation = occt::tessellate(
      opdata->source, CAD_TESSELLATE_LINEAR_DEFLECTION, CAD_TESSELLATE_ANGULAR_DEFLECTION);
  CAD_object_mesh_replace(opdata->object, tessellation);
  CAD_object_shape_store(opdata->object, opdata->original_blob);
}

static void cad_fillet_update_status_text(bContext *C,
                                          const CADFilletMode mode,
                                          const float value)
{
  const char *label = (mode == CADFilletMode::Fillet) ? IFACE_("CAD Fillet Radius") :
                                                        IFACE_("CAD Chamfer Distance");
  const std::string header_status = fmt::format(
      "{}: {:.4f} | {}", label, value, IFACE_("LMB/Enter: Confirm, Esc/RMB: Cancel"));
  ED_area_status_text(CTX_wm_area(C), header_status.c_str());
}

static void cad_fillet_exit(bContext *C, wmOperator *op)
{
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_status_text(area, nullptr);
  }
  MEM_delete(static_cast<CADFilletData *>(op->customdata));
  op->customdata = nullptr;
}

static void cad_fillet_cancel(bContext *C, wmOperator *op)
{
  CADFilletData *opdata = static_cast<CADFilletData *>(op->customdata);
  if (opdata == nullptr) {
    return;
  }
  /* The object already existed, so restore its original mesh + B-rep instead of deleting it. */
  cad_fillet_restore_original(opdata);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, opdata->object->data);

  cad_fillet_exit(C, op);

  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
}

static wmOperatorStatus cad_fillet_confirm(bContext *C, wmOperator *op)
{
  CADFilletData *opdata = static_cast<CADFilletData *>(op->customdata);
  const float value = RNA_float_get(op->ptr, cad_fillet_prop_name(opdata->mode));

  /* Final recompute so the committed shape exactly matches the property value. */
  std::string error;
  occt::OcctShapeHandle solid = cad_fillet_apply(opdata, value, &error);
  if (!solid.is_valid()) {
    cad_fillet_restore_original(opdata);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, opdata->object->data);
    BKE_reportf(op->reports, RPT_ERROR, "OCCT edge operation failed: %s", error.c_str());
    cad_fillet_exit(C, op);
    return OPERATOR_CANCELLED;
  }
  const Vector<uint8_t> blob = occt::serialize(solid);
  if (blob.is_empty()) {
    cad_fillet_restore_original(opdata);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, opdata->object->data);
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to serialize the result solid");
    cad_fillet_exit(C, op);
    return OPERATOR_CANCELLED;
  }

  const occt::TessellatedMesh tessellation = occt::tessellate(
      solid, CAD_TESSELLATE_LINEAR_DEFLECTION, CAD_TESSELLATE_ANGULAR_DEFLECTION);
  CAD_object_mesh_replace(opdata->object, tessellation);
  CAD_object_shape_store(opdata->object, blob);

  WM_event_add_notifier(C, NC_GEOM | ND_DATA, opdata->object->data);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, opdata->object);

  cad_fillet_exit(C, op);
  return OPERATOR_FINISHED;
}

/** Shared invoke for both operators; `mode` selects fillet vs chamfer. */
static wmOperatorStatus cad_fillet_invoke_impl(bContext *C,
                                               wmOperator *op,
                                               const wmEvent *event,
                                               const CADFilletMode mode)
{
  const RegionView3D *rv3d = CTX_wm_region_view3d(C);
  Object *ob = CTX_data_active_object(C);

  const std::optional<Vector<uint8_t>> blob = CAD_object_shape_load(ob);
  if (!blob) {
    BKE_report(op->reports, RPT_ERROR, "Active object does not carry CAD solid data");
    return OPERATOR_CANCELLED;
  }
  occt::OcctShapeHandle source = occt::deserialize(*blob);
  if (!source.is_valid()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to deserialize the stored CAD solid data");
    return OPERATOR_CANCELLED;
  }

  CADFilletData *opdata = MEM_new<CADFilletData>(__func__);
  opdata->source = std::move(source);
  opdata->object = ob;
  opdata->original_blob = *blob;
  opdata->init_mval[0] = event->mval[0];
  opdata->init_mval[1] = event->mval[1];
  /* The size of a pixel under the object origin maps mouse movement to the radius/distance. */
  opdata->pixel_size = rv3d ? ED_view3d_pixel_size(rv3d, ob->object_to_world().location()) : 1.0f;
  opdata->mode = mode;
  op->customdata = opdata;

  /* Small initial preview so the effect is visible before the first mouse move. */
  const char *prop_name = cad_fillet_prop_name(mode);
  RNA_float_set(op->ptr, prop_name, CAD_FILLET_INITIAL_RADIUS);
  std::string error;
  if (!cad_fillet_preview_update(opdata, CAD_FILLET_INITIAL_RADIUS, &error)) {
    /* The mesh is untouched on a failed preview; restore is a safe no-op that also frees. */
    BKE_reportf(op->reports, RPT_ERROR, "OCCT edge operation failed: %s", error.c_str());
    cad_fillet_cancel(C, op);
    return OPERATOR_CANCELLED;
  }
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob->data);

  cad_fillet_update_status_text(C, mode, CAD_FILLET_INITIAL_RADIUS);

  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus cad_fillet_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  CADFilletData *opdata = static_cast<CADFilletData *>(op->customdata);

  switch (event->type) {
    case MOUSEMOVE: {
      /* Vertical screen-space drag maps to the radius/distance. The scale is the
       * world-space size of a pixel at the object origin, Shift enables precision.
       * The value is clamped to a small strictly-positive floor (>= 0). */
      const float sensitivity = (event->modifier & KM_SHIFT) ? CAD_FILLET_PRECISION_FACTOR : 1.0f;
      const float raw = float(event->mval[1] - opdata->init_mval[1]) * opdata->pixel_size *
                        sensitivity;
      const float value = std::max(raw, CAD_FILLET_MIN_RADIUS);

      /* Preview failures (e.g. a radius exceeding the geometry) keep the last valid mesh. Only
       * commit the property once the preview succeeded, so confirming re-applies the last
       * successfully previewed value instead of a rejected one (mirrors cad_extrude). */
      std::string error;
      if (cad_fillet_preview_update(opdata, value, &error)) {
        RNA_float_set(op->ptr, cad_fillet_prop_name(opdata->mode), value);
        WM_event_add_notifier(C, NC_GEOM | ND_DATA, opdata->object->data);
      }
      cad_fillet_update_status_text(C, opdata->mode, value);
      break;
    }
    case LEFTMOUSE:
    case EVT_RETKEY:
    case EVT_PADENTER: {
      if (event->val == KM_PRESS) {
        return cad_fillet_confirm(C, op);
      }
      break;
    }
    case RIGHTMOUSE:
    case EVT_ESCKEY: {
      if (event->val == KM_PRESS) {
        cad_fillet_cancel(C, op);
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

/** Object mode + active object carries a CAD B-rep (mirrors cad_boolean's poll). */
static bool cad_fillet_poll(bContext *C)
{
  if (!ED_operator_objectmode(C)) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  return ob != nullptr && CAD_object_has_shape(ob);
}

static wmOperatorStatus cad_fillet_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return cad_fillet_invoke_impl(C, op, event, CADFilletMode::Fillet);
}

static wmOperatorStatus cad_chamfer_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return cad_fillet_invoke_impl(C, op, event, CADFilletMode::Chamfer);
}

/**
 * Non-modal execution (Adjust-Last-Operation / F9 redo panel and Python EXEC): apply the
 * operation to the active object in place, at the property value. On redo, Blender rolls the
 * operator's effect back before re-running exec, so the loaded blob is the pre-operation
 * original -- re-filleting it is not a double application.
 */
static wmOperatorStatus cad_fillet_exec_impl(bContext *C, wmOperator *op, const CADFilletMode mode)
{
  Object *ob = CTX_data_active_object(C);
  const std::optional<Vector<uint8_t>> blob = CAD_object_shape_load(ob);
  if (!blob) {
    BKE_report(op->reports, RPT_ERROR, "Active object does not carry CAD solid data");
    return OPERATOR_CANCELLED;
  }
  occt::OcctShapeHandle source = occt::deserialize(*blob);
  if (!source.is_valid()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to deserialize the stored CAD solid data");
    return OPERATOR_CANCELLED;
  }

  const float value = RNA_float_get(op->ptr, cad_fillet_prop_name(mode));
  std::string error;
  occt::OcctShapeHandle solid = (mode == CADFilletMode::Fillet) ?
                                    occt::fillet_all_edges(source, value, &error) :
                                    occt::chamfer_all_edges(source, value, &error);
  if (!solid.is_valid()) {
    BKE_reportf(op->reports, RPT_ERROR, "OCCT edge operation failed: %s", error.c_str());
    return OPERATOR_CANCELLED;
  }
  const Vector<uint8_t> result_blob = occt::serialize(solid);
  if (result_blob.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to serialize the result solid");
    return OPERATOR_CANCELLED;
  }

  const occt::TessellatedMesh tessellation = occt::tessellate(
      solid, CAD_TESSELLATE_LINEAR_DEFLECTION, CAD_TESSELLATE_ANGULAR_DEFLECTION);
  CAD_object_mesh_replace(ob, tessellation);
  CAD_object_shape_store(ob, result_blob);

  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob->data);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus cad_fillet_exec(bContext *C, wmOperator *op)
{
  return cad_fillet_exec_impl(C, op, CADFilletMode::Fillet);
}

static wmOperatorStatus cad_chamfer_exec(bContext *C, wmOperator *op)
{
  return cad_fillet_exec_impl(C, op, CADFilletMode::Chamfer);
}

void CAD_OT_fillet(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "CAD Fillet";
  ot->description = "Round all edges of the active CAD solid with a uniform radius";
  ot->idname = "CAD_OT_fillet";

  /* API callbacks. */
  ot->invoke = cad_fillet_invoke;
  ot->modal = cad_fillet_modal;
  ot->exec = cad_fillet_exec;
  ot->cancel = cad_fillet_cancel;
  ot->poll = cad_fillet_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* props */
  RNA_def_float_distance(ot->srna,
                         "radius",
                         0.1f,
                         0.0f,
                         1e6f,
                         "Radius",
                         "Fillet radius applied uniformly to every edge",
                         0.0f,
                         100.0f);
}

void CAD_OT_chamfer(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "CAD Chamfer";
  ot->description = "Bevel all edges of the active CAD solid with a uniform setback distance";
  ot->idname = "CAD_OT_chamfer";

  /* API callbacks. */
  ot->invoke = cad_chamfer_invoke;
  ot->modal = cad_fillet_modal;
  ot->exec = cad_chamfer_exec;
  ot->cancel = cad_fillet_cancel;
  ot->poll = cad_fillet_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* props */
  RNA_def_float_distance(ot->srna,
                         "distance",
                         0.1f,
                         0.0f,
                         1e6f,
                         "Distance",
                         "Chamfer setback distance applied uniformly to every edge",
                         0.0f,
                         100.0f);
}

}  // namespace blender
