/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edcad
 *
 * Import/export operators bridging the OCCT B-rep to the neutral CAD exchange
 * formats STEP and IGES. Export writes the active object's stored solid to a
 * file; import reads the root solid from a file into a new mesh object that
 * carries the B-rep (so it can be filleted, booleaned, etc.).
 *
 * These follow Blender's file-select operator idiom (see `io_stl_ops.cc`):
 * `invoke` opens the file browser and `exec` runs once a path is chosen.
 */

#include <optional>
#include <string>

#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BLI_path_utils.hh"
#include "BLI_sys_types.hh"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_fileselect.hh"
#include "ED_object.hh"

#include "OCCT_bridge.hh"

#include "cad_intern.hh"

namespace blender {

/** Exchange format handled by a given operator. */
enum class CADIOFormat {
  Step,
  Iges,
};

/** Active object carries a CAD B-rep (required to have something to export). */
static bool cad_io_export_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob != nullptr && CAD_object_has_shape(ob);
}

/** Create the destination mesh object at the 3D cursor (mirrors cad_extrude's add). */
static Object *cad_io_object_add(bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  const View3D *v3d = CTX_wm_view3d(C);
  const ushort local_view_bits = (v3d && v3d->localvd) ? v3d->local_view_uid : 0;
  return ed::object::add_type(
      C, OB_MESH, DATA_("Solid"), scene->cursor.location, nullptr, false, local_view_bits);
}

/** Ensure the operator's `filepath` ends in `ext`; returns true when it changed it. */
static bool cad_io_check_extension(wmOperator *op, const char *ext)
{
  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);
  if (!BLI_path_extension_check(filepath, ext)) {
    BLI_path_extension_ensure(filepath, FILE_MAX, ext);
    RNA_string_set(op->ptr, "filepath", filepath);
    return true;
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name Export
 * \{ */

static wmOperatorStatus cad_export_exec_impl(bContext *C, wmOperator *op, const CADIOFormat format)
{
  if (!RNA_struct_property_is_set_ex(op->ptr, "filepath", false)) {
    BKE_report(op->reports, RPT_ERROR, "No filename given");
    return OPERATOR_CANCELLED;
  }

  Object *ob = CTX_data_active_object(C);
  const std::optional<Vector<uint8_t>> blob = CAD_object_shape_load(ob);
  if (!blob) {
    BKE_report(op->reports, RPT_ERROR, "Active object does not carry CAD solid data");
    return OPERATOR_CANCELLED;
  }
  occt::OcctShapeHandle shape = occt::deserialize(*blob);
  if (!shape.is_valid()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to deserialize the stored CAD solid data");
    return OPERATOR_CANCELLED;
  }

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  std::string error;
  const bool ok = (format == CADIOFormat::Step) ? occt::export_step(shape, filepath, &error) :
                                                  occt::export_iges(shape, filepath, &error);
  if (!ok) {
    BKE_reportf(op->reports, RPT_ERROR, "OCCT export failed: %s", error.c_str());
    return OPERATOR_CANCELLED;
  }

  BKE_report(op->reports, RPT_INFO, "File exported successfully");
  return OPERATOR_FINISHED;
}

static wmOperatorStatus cad_export_step_invoke(bContext *C,
                                               wmOperator *op,
                                               const wmEvent * /*event*/)
{
  ED_fileselect_ensure_default_filepath(C, op, ".step");
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus cad_export_step_exec(bContext *C, wmOperator *op)
{
  return cad_export_exec_impl(C, op, CADIOFormat::Step);
}

static bool cad_export_step_check(bContext * /*C*/, wmOperator *op)
{
  return cad_io_check_extension(op, ".step");
}

void CAD_OT_export_step(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Export STEP";
  ot->description = "Write the active CAD solid to a STEP file";
  ot->idname = "CAD_OT_export_step";

  ot->invoke = cad_export_step_invoke;
  ot->exec = cad_export_step_exec;
  ot->poll = cad_io_export_poll;
  ot->check = cad_export_step_check;

  ot->flag = OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  /* Only show `.step` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.step;*.stp", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

static wmOperatorStatus cad_export_iges_invoke(bContext *C,
                                               wmOperator *op,
                                               const wmEvent * /*event*/)
{
  ED_fileselect_ensure_default_filepath(C, op, ".iges");
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus cad_export_iges_exec(bContext *C, wmOperator *op)
{
  return cad_export_exec_impl(C, op, CADIOFormat::Iges);
}

static bool cad_export_iges_check(bContext * /*C*/, wmOperator *op)
{
  return cad_io_check_extension(op, ".iges");
}

void CAD_OT_export_iges(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Export IGES";
  ot->description = "Write the active CAD solid to an IGES file";
  ot->idname = "CAD_OT_export_iges";

  ot->invoke = cad_export_iges_invoke;
  ot->exec = cad_export_iges_exec;
  ot->poll = cad_io_export_poll;
  ot->check = cad_export_iges_check;

  ot->flag = OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  /* Only show `.iges` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.iges;*.igs", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Import
 * \{ */

static wmOperatorStatus cad_import_exec_impl(bContext *C, wmOperator *op, const CADIOFormat format)
{
  if (!RNA_struct_property_is_set_ex(op->ptr, "filepath", false)) {
    BKE_report(op->reports, RPT_ERROR, "No filename given");
    return OPERATOR_CANCELLED;
  }

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  std::string error;
  occt::OcctShapeHandle shape = (format == CADIOFormat::Step) ?
                                    occt::import_step(filepath, &error) :
                                    occt::import_iges(filepath, &error);
  if (!shape.is_valid()) {
    BKE_reportf(op->reports, RPT_ERROR, "OCCT import failed: %s", error.c_str());
    return OPERATOR_CANCELLED;
  }

  const Vector<uint8_t> blob = occt::serialize(shape);
  if (blob.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "OCCT: failed to serialize the imported solid");
    return OPERATOR_CANCELLED;
  }

  Object *ob = cad_io_object_add(C);
  const occt::TessellatedMesh tessellation = occt::tessellate(
      shape, CAD_TESSELLATE_LINEAR_DEFLECTION, CAD_TESSELLATE_ANGULAR_DEFLECTION);
  CAD_object_mesh_replace(ob, tessellation);
  CAD_object_shape_store(ob, blob);

  Scene *scene = CTX_data_scene(C);
  DEG_relations_tag_update(CTX_data_main(C));
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob->data);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus cad_import_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus cad_import_step_exec(bContext *C, wmOperator *op)
{
  return cad_import_exec_impl(C, op, CADIOFormat::Step);
}

void CAD_OT_import_step(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Import STEP";
  ot->description = "Read the root solid from a STEP file as a CAD object";
  ot->idname = "CAD_OT_import_step";

  ot->invoke = cad_import_invoke;
  ot->exec = cad_import_step_exec;
  ot->poll = WM_operator_winactive;

  ot->flag = OPTYPE_UNDO | OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  /* Only show `.step` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.step;*.stp", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

static wmOperatorStatus cad_import_iges_exec(bContext *C, wmOperator *op)
{
  return cad_import_exec_impl(C, op, CADIOFormat::Iges);
}

void CAD_OT_import_iges(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Import IGES";
  ot->description = "Read the root solid from an IGES file as a CAD object";
  ot->idname = "CAD_OT_import_iges";

  ot->invoke = cad_import_invoke;
  ot->exec = cad_import_iges_exec;
  ot->poll = WM_operator_winactive;

  ot->flag = OPTYPE_UNDO | OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  /* Only show `.iges` files by default. */
  prop = RNA_def_string(ot->srna, "filter_glob", "*.iges;*.igs", 0, "Extension Filter", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

}  // namespace blender
