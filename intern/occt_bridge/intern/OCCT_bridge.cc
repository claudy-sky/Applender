/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_occt_bridge
 *
 * Implementation of the `blender::occt` facade. All OCCT headers and
 * types are quarantined in this translation unit (see `OCCT_bridge.hh`).
 *
 * Written against the OCCT 7.8 documented API, but this tree cannot be
 * compiled against OCCT in the authoring environment. The following call
 * sites MUST be verified against the real OCCT 7.8 headers on first build:
 *
 * - `BRepMesh_IncrementalMesh` constructor: the 5-argument overload
 *   `(shape, theLinDeflection, isRelative, theAngDeflection, isInParallel)`
 *   used in #tessellate. OCCT 7.8 also offers an
 *   `IMeshTools_Parameters`-based overload if this one has changed.
 * - `BinTools::Write(shape, std::ostream &)` and
 *   `BinTools::Read(shape, std::istream &)` overloads used in #serialize /
 *   #deserialize (newer OCCT adds trailing `Message_ProgressRange` and
 *   with-triangulation parameters with defaults; confirm the plain forms
 *   still exist).
 * - `Poly_Triangulation` accessors: `Node(i)` / `Triangle(i)` with 1-based
 *   indexing (OCCT >= 7.6 API; the pre-7.6 `Nodes()` / `Triangles()` array
 *   accessors were removed). Confirm no further changes in 7.8.
 * - `gp_Trsf::SetValues` raises `Standard_ConstructionError` for matrices
 *   with shear or non-uniform scale; #transformed relies on that behavior
 *   (see the note there about `gp_GTrsf` / `BRepBuilderAPI_GTransform`).
 * - `BRepBuilderAPI_MakeFace(wire, onlyPlane)` overload used in
 *   #make_rect_profile.
 */

#include "OCCT_bridge.hh"

#include <cmath>
#include <cstring>
#include <exception>
#include <sstream>
#include <utility>

#include "BLI_map.hh"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <BinTools.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

namespace blender::occt {

/* Complete definition of the pimpl forward-declared in the header. */
struct OcctShapeImpl {
  TopoDS_Shape shape;
};

/* -------------------------------------------------------------------- */
/** \name Handle
 * \{ */

OcctShapeHandle::OcctShapeHandle() = default;

OcctShapeHandle::OcctShapeHandle(std::unique_ptr<OcctShapeImpl> impl) : impl_(std::move(impl)) {}

OcctShapeHandle::~OcctShapeHandle() = default;

OcctShapeHandle::OcctShapeHandle(OcctShapeHandle &&other) noexcept = default;

OcctShapeHandle &OcctShapeHandle::operator=(OcctShapeHandle &&other) noexcept = default;

bool OcctShapeHandle::is_valid() const
{
  return impl_ && !impl_->shape.IsNull();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Utilities
 * \{ */

namespace {

/** Wrap a shape in an owning handle; a null shape yields an invalid handle. */
OcctShapeHandle make_handle(const TopoDS_Shape &shape)
{
  if (shape.IsNull()) {
    return OcctShapeHandle();
  }
  return OcctShapeHandle(std::make_unique<OcctShapeImpl>(OcctShapeImpl{shape}));
}

void set_error(std::string *r_error, const std::string &message)
{
  if (r_error) {
    *r_error = message;
  }
}

/** Human-readable message from an OCCT exception, with a fallback. */
std::string failure_message(const Standard_Failure &err, const char *fallback)
{
  const char *msg = err.GetMessageString();
  if (msg && msg[0] != '\0') {
    return msg;
  }
  return fallback;
}

}  // namespace

/** \} */

/* -------------------------------------------------------------------- */
/** \name Facade Functions
 * \{ */

bool is_available()
{
  /* This translation unit is only compiled with `WITH_OCCT`; linked in
   * means available. */
  return true;
}

OcctShapeHandle make_rect_profile(const float size_x, const float size_y)
{
  if (!(size_x > 0.0f) || !(size_y > 0.0f)) {
    return OcctShapeHandle();
  }
  const double hx = double(size_x) * 0.5;
  const double hy = double(size_y) * 0.5;
  try {
    /* Counter-clockwise (seen from +Z) rectangle on the XY plane centered
     * at the origin, so the resulting face normal points along +Z. */
    BRepBuilderAPI_MakePolygon polygon;
    polygon.Add(gp_Pnt(-hx, -hy, 0.0));
    polygon.Add(gp_Pnt(hx, -hy, 0.0));
    polygon.Add(gp_Pnt(hx, hy, 0.0));
    polygon.Add(gp_Pnt(-hx, hy, 0.0));
    polygon.Close();
    if (!polygon.IsDone()) {
      return OcctShapeHandle();
    }
    const TopoDS_Wire wire = polygon.Wire();
    /* `onlyPlane=true`: the wire is planar by construction, find its plane. */
    BRepBuilderAPI_MakeFace face_maker(wire, true);
    if (!face_maker.IsDone()) {
      return OcctShapeHandle();
    }
    return make_handle(face_maker.Face());
  }
  catch (const Standard_Failure &) {
    return OcctShapeHandle();
  }
  /* Here and below: catch `std::exception` too (e.g. `std::bad_alloc` or
   * standard-library exceptions escaping OCCT), mapped to the same failure
   * convention. `Standard_Failure` must stay first: stock OCCT 7.8 derives
   * it from `Standard_Transient`, not `std::exception`, but the order
   * matters (and stays correct) for configurations where it does. */
  catch (const std::exception &) {
    return OcctShapeHandle();
  }
}

OcctShapeHandle extrude_profile(const OcctShapeHandle &profile,
                                const float distance,
                                std::string *r_error)
{
  if (!profile.is_valid()) {
    set_error(r_error, "Extrude: invalid profile shape");
    return OcctShapeHandle();
  }
  if (distance == 0.0f) {
    set_error(r_error, "Extrude: distance must be non-zero");
    return OcctShapeHandle();
  }
  try {
    BRepPrimAPI_MakePrism prism(profile.impl()->shape, gp_Vec(0.0, 0.0, double(distance)));
    if (!prism.IsDone()) {
      set_error(r_error, "Extrude: prism construction failed");
      return OcctShapeHandle();
    }
    return make_handle(prism.Shape());
  }
  catch (const Standard_Failure &err) {
    set_error(r_error, failure_message(err, "Extrude: OCCT exception"));
    return OcctShapeHandle();
  }
  catch (const std::exception &err) {
    set_error(r_error, err.what());
    return OcctShapeHandle();
  }
}

OcctShapeHandle boolean_op(const OcctShapeHandle &a,
                           const OcctShapeHandle &b,
                           const BooleanOp op,
                           std::string *r_error)
{
  if (!a.is_valid() || !b.is_valid()) {
    set_error(r_error, "Boolean: invalid operand shape");
    return OcctShapeHandle();
  }
  const TopoDS_Shape &shape_a = a.impl()->shape;
  const TopoDS_Shape &shape_b = b.impl()->shape;
  try {
    switch (op) {
      case BooleanOp::Union: {
        BRepAlgoAPI_Fuse fuse(shape_a, shape_b);
        if (!fuse.IsDone()) {
          set_error(r_error, "Boolean: union operation failed");
          return OcctShapeHandle();
        }
        return make_handle(fuse.Shape());
      }
      case BooleanOp::Subtract: {
        BRepAlgoAPI_Cut cut(shape_a, shape_b);
        if (!cut.IsDone()) {
          set_error(r_error, "Boolean: subtract operation failed");
          return OcctShapeHandle();
        }
        return make_handle(cut.Shape());
      }
      case BooleanOp::Intersect: {
        BRepAlgoAPI_Common common(shape_a, shape_b);
        if (!common.IsDone()) {
          set_error(r_error, "Boolean: intersect operation failed");
          return OcctShapeHandle();
        }
        return make_handle(common.Shape());
      }
    }
    set_error(r_error, "Boolean: unknown operation");
    return OcctShapeHandle();
  }
  catch (const Standard_Failure &err) {
    set_error(r_error, failure_message(err, "Boolean: OCCT exception"));
    return OcctShapeHandle();
  }
  catch (const std::exception &err) {
    set_error(r_error, err.what());
    return OcctShapeHandle();
  }
}

OcctShapeHandle fillet_all_edges(const OcctShapeHandle &shape,
                                 const float radius,
                                 std::string *r_error)
{
  if (!shape.is_valid()) {
    set_error(r_error, "Fillet: invalid shape");
    return OcctShapeHandle();
  }
  if (!(radius > 0.0f)) {
    set_error(r_error, "Fillet: radius must be positive");
    return OcctShapeHandle();
  }
  const TopoDS_Shape &occt_shape = shape.impl()->shape;
  try {
    BRepFilletAPI_MakeFillet mk(occt_shape);
    /* Collect every edge of the shape and add each with a uniform radius.
     * v1 fillets all edges (no per-edge selection) to sidestep the
     * topological-naming problem. */
    TopTools_IndexedMapOfShape edge_map;
    TopExp::MapShapes(occt_shape, TopAbs_EDGE, edge_map);
    if (edge_map.Extent() == 0) {
      set_error(r_error, "Fillet: shape has no edges");
      return OcctShapeHandle();
    }
    for (int i = 1; i <= edge_map.Extent(); i++) {
      mk.Add(double(radius), TopoDS::Edge(edge_map.FindKey(i)));
    }
    mk.Build();
    if (!mk.IsDone()) {
      set_error(r_error, "Fillet: build failed");
      return OcctShapeHandle();
    }
    return make_handle(mk.Shape());
  }
  catch (const Standard_Failure &err) {
    set_error(r_error, failure_message(err, "Fillet: OCCT exception"));
    return OcctShapeHandle();
  }
  catch (const std::exception &err) {
    set_error(r_error, err.what());
    return OcctShapeHandle();
  }
}

OcctShapeHandle chamfer_all_edges(const OcctShapeHandle &shape,
                                  const float distance,
                                  std::string *r_error)
{
  if (!shape.is_valid()) {
    set_error(r_error, "Chamfer: invalid shape");
    return OcctShapeHandle();
  }
  if (!(distance > 0.0f)) {
    set_error(r_error, "Chamfer: distance must be positive");
    return OcctShapeHandle();
  }
  const TopoDS_Shape &occt_shape = shape.impl()->shape;
  try {
    BRepFilletAPI_MakeChamfer mk(occt_shape);
    TopTools_IndexedMapOfShape edge_map;
    TopExp::MapShapes(occt_shape, TopAbs_EDGE, edge_map);
    if (edge_map.Extent() == 0) {
      set_error(r_error, "Chamfer: shape has no edges");
      return OcctShapeHandle();
    }
    for (int i = 1; i <= edge_map.Extent(); i++) {
      /* VERIFY: BRepFilletAPI_MakeChamfer::Add(dist, edge) -- the symmetric
       * single-distance overload that takes only an edge exists in OCCT 7.8
       * (it internally picks a relative face). Other overloads take an
       * explicit face: Add(dist, edge, face) / Add(d1, d2, edge, face). The
       * bare Add(dist, edge) is the intended v1 call. */
      mk.Add(double(distance), TopoDS::Edge(edge_map.FindKey(i)));
    }
    mk.Build();
    if (!mk.IsDone()) {
      set_error(r_error, "Chamfer: build failed");
      return OcctShapeHandle();
    }
    return make_handle(mk.Shape());
  }
  catch (const Standard_Failure &err) {
    set_error(r_error, failure_message(err, "Chamfer: OCCT exception"));
    return OcctShapeHandle();
  }
  catch (const std::exception &err) {
    set_error(r_error, err.what());
    return OcctShapeHandle();
  }
}

OcctShapeHandle transformed(const OcctShapeHandle &shape, const float m[4][4])
{
  if (!shape.is_valid()) {
    return OcctShapeHandle();
  }
  try {
    /* Matrix convention mapping -- a classic silent-corruption spot, keep
     * this comment in sync with the code below.
     *
     * Blender stores a 4x4 matrix column-major: `m[col][row]`. So the
     * element in mathematical row `i`, column `j` of the transform is
     * `m[j][i]`, and the translation vector is `m[3][0..2]`.
     *
     * `gp_Trsf::SetValues` takes the upper 3x4 of the matrix ROW-MAJOR:
     *   a11 a12 a13 a14   (first row: rotation row 0, then translation x)
     *   a21 a22 a23 a24
     *   a31 a32 a33 a34
     * i.e. `a(i+1)(j+1) = m[j][i]` and `a(i+1)4 = m[3][i]`.
     *
     * NOTE: `gp_Trsf` only represents similarity transforms; `SetValues`
     * raises `Standard_ConstructionError` for shear or non-uniform scale.
     * Such matrices would need `gp_GTrsf` + `BRepBuilderAPI_GTransform`,
     * which is out of scope for this first slice -- callers get an invalid
     * handle instead (caught below). */
    gp_Trsf trsf;
    trsf.SetValues(double(m[0][0]),
                   double(m[1][0]),
                   double(m[2][0]),
                   double(m[3][0]),
                   double(m[0][1]),
                   double(m[1][1]),
                   double(m[2][1]),
                   double(m[3][1]),
                   double(m[0][2]),
                   double(m[1][2]),
                   double(m[2][2]),
                   double(m[3][2]));

    /* `copy=true`: always produce an independent copy, never share
     * modified geometry with the input shape. */
    BRepBuilderAPI_Transform xform(shape.impl()->shape, trsf, true);
    if (!xform.IsDone()) {
      return OcctShapeHandle();
    }
    return make_handle(xform.Shape());
  }
  catch (const Standard_Failure &) {
    return OcctShapeHandle();
  }
  catch (const std::exception &) {
    return OcctShapeHandle();
  }
}

TessellatedMesh tessellate(const OcctShapeHandle &shape,
                           const float linear_deflection,
                           const float angular_deflection)
{
  TessellatedMesh mesh;
  if (!shape.is_valid()) {
    return mesh;
  }
  /* Guard against degenerate deflection values that would make the mesher
   * fail or run away. */
  const double lin_defl = (linear_deflection > 0.0f) ? double(linear_deflection) : 0.1;
  const double ang_defl = (angular_deflection > 0.0f) ? double(angular_deflection) : 0.5;

  const TopoDS_Shape &occt_shape = shape.impl()->shape;
  try {
    /* Attaches `Poly_Triangulation` data to the shape's faces.
     * Arguments: (shape, linear deflection, is-relative, angular
     * deflection, parallel). Verify this 5-argument overload against the
     * installed OCCT 7.8 headers on first build (see file-top comment). */
    BRepMesh_IncrementalMesh mesher(occt_shape, lin_defl, false, ang_defl, true);
    (void)mesher;

    /* OCCT triangulates each face independently, so nodes along shared
     * B-rep edges and corners are duplicated per face. Weld them by
     * quantizing each shape-space position onto a grid and mapping every
     * quantized position to a single mesh vertex, so the committed mesh is
     * a connected surface instead of disconnected per-face shells.
     *
     * Epsilon choice: coordinates are multiplied by 1e6 and rounded, i.e.
     * positions are welded on a 1e-6 model-unit grid. That is several
     * orders of magnitude below any practical linear deflection (see
     * `lin_defl` above), so only nodes that sample the same B-rep
     * vertex/edge collapse, while distinct tessellation nodes stay apart. */
    constexpr double weld_scale = 1e6;
    Map<VecBase<int64_t, 3>, int> vert_by_position;
    /* Per-face scratch map from the face's 1-based node index to the
     * welded mesh vertex index. */
    Vector<int> node_to_vert;

    for (TopExp_Explorer explorer(occt_shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
      const TopoDS_Face &face = TopoDS::Face(explorer.Current());
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, loc);
      if (triangulation.IsNull()) {
        continue;
      }

      const gp_Trsf &trsf = loc.Transformation();

      /* OCCT >= 7.6 `Poly_Triangulation` API: `Node(i)` / `Triangle(i)`
       * with 1-based indices (pre-7.6 `Nodes()` array accessors are gone). */
      const int nb_nodes = triangulation->NbNodes();
      node_to_vert.reinitialize(nb_nodes);
      for (int i = 1; i <= nb_nodes; i++) {
        gp_Pnt point = triangulation->Node(i);
        /* Face triangulations are stored in the face's local frame;
         * transform into shape space by the face location. */
        point.Transform(trsf);
        const VecBase<int64_t, 3> key(int64_t(std::llround(point.X() * weld_scale)),
                                      int64_t(std::llround(point.Y() * weld_scale)),
                                      int64_t(std::llround(point.Z() * weld_scale)));
        node_to_vert[i - 1] = vert_by_position.lookup_or_add_cb(key, [&]() {
          mesh.verts.append(float3(float(point.X()), float(point.Y()), float(point.Z())));
          return int(mesh.verts.size() - 1);
        });
      }

      /* A `TopAbs_REVERSED` face uses the reversed surface normal; flip
       * the triangle winding so the tessellation faces outward. */
      const bool reversed = (face.Orientation() == TopAbs_REVERSED);

      const int nb_triangles = triangulation->NbTriangles();
      for (int i = 1; i <= nb_triangles; i++) {
        Standard_Integer n1, n2, n3;
        triangulation->Triangle(i).Get(n1, n2, n3);
        if (reversed) {
          std::swap(n2, n3);
        }
        /* Remap the 1-based per-face node indices to welded vertices. */
        mesh.tris.append(
            int3(node_to_vert[n1 - 1], node_to_vert[n2 - 1], node_to_vert[n3 - 1]));
      }
    }
  }
  catch (const Standard_Failure &) {
    mesh.verts.clear();
    mesh.tris.clear();
  }
  catch (const std::exception &) {
    mesh.verts.clear();
    mesh.tris.clear();
  }
  return mesh;
}

Vector<uint8_t> serialize(const OcctShapeHandle &shape)
{
  Vector<uint8_t> result;
  if (!shape.is_valid()) {
    return result;
  }
  try {
    std::ostringstream stream(std::ios::binary);
    /* Binary BRep format; `Standard_OStream` is `std::ostream`. Verify the
     * plain 2-argument overload on first build (see file-top comment). */
    BinTools::Write(shape.impl()->shape, stream);
    const std::string data = stream.str();
    result.resize(int64_t(data.size()));
    std::memcpy(result.data(), data.data(), data.size());
  }
  catch (const Standard_Failure &) {
    result.clear();
  }
  catch (const std::exception &) {
    result.clear();
  }
  return result;
}

OcctShapeHandle deserialize(const Span<const uint8_t> blob)
{
  if (blob.is_empty()) {
    return OcctShapeHandle();
  }
  try {
    std::istringstream stream(
        std::string(reinterpret_cast<const char *>(blob.data()), size_t(blob.size())),
        std::ios::binary);
    TopoDS_Shape occt_shape;
    BinTools::Read(occt_shape, stream);
    return make_handle(occt_shape);
  }
  catch (const Standard_Failure &) {
    return OcctShapeHandle();
  }
  catch (const std::exception &) {
    return OcctShapeHandle();
  }
}

bool export_step(const OcctShapeHandle &shape, const std::string &path, std::string *r_error)
{
  if (!shape.is_valid()) {
    set_error(r_error, "Export STEP: invalid shape");
    return false;
  }
  try {
    STEPControl_Writer writer;
    /* `STEPControl_AsIs` writes whatever geometry the shape already is
     * (solid/shell/etc.) without forcing a target representation. */
    const IFSelect_ReturnStatus transfer_status = writer.Transfer(shape.impl()->shape,
                                                                  STEPControl_AsIs);
    if (transfer_status != IFSelect_RetDone) {
      set_error(r_error, "Export STEP: transfer failed");
      return false;
    }
    const IFSelect_ReturnStatus write_status = writer.Write(path.c_str());
    if (write_status != IFSelect_RetDone) {
      set_error(r_error, "Export STEP: write failed");
      return false;
    }
    return true;
  }
  catch (const Standard_Failure &err) {
    set_error(r_error, failure_message(err, "Export STEP: OCCT exception"));
    return false;
  }
  catch (const std::exception &err) {
    set_error(r_error, err.what());
    return false;
  }
}

bool export_iges(const OcctShapeHandle &shape, const std::string &path, std::string *r_error)
{
  if (!shape.is_valid()) {
    set_error(r_error, "Export IGES: invalid shape");
    return false;
  }
  try {
    /* VERIFY: IGESControl_Writer API differs from STEPControl_Writer. In
     * OCCT 7.8 the default-constructed writer initializes IGES units/mode
     * from Interface_Static; `AddShape` returns a Standard_Boolean, and
     * `ComputeModel()` must be called before `Write()`. */
    IGESControl_Writer writer;
    if (!writer.AddShape(shape.impl()->shape)) {
      set_error(r_error, "Export IGES: AddShape failed");
      return false;
    }
    writer.ComputeModel();
    /* VERIFY: `IGESControl_Writer::Write(const char *)` returns a
     * Standard_Boolean (true on success), NOT an IFSelect_ReturnStatus like
     * the STEP writer. */
    if (!writer.Write(path.c_str())) {
      set_error(r_error, "Export IGES: write failed");
      return false;
    }
    return true;
  }
  catch (const Standard_Failure &err) {
    set_error(r_error, failure_message(err, "Export IGES: OCCT exception"));
    return false;
  }
  catch (const std::exception &err) {
    set_error(r_error, err.what());
    return false;
  }
}

OcctShapeHandle import_step(const std::string &path, std::string *r_error)
{
  try {
    STEPControl_Reader reader;
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
      set_error(r_error, "Import STEP: could not read file");
      return OcctShapeHandle();
    }
    reader.TransferRoots();
    /* `OneShape` collapses all transferred roots into a single shape (a
     * compound when there are several); it may be null when nothing was
     * transferred. */
    const TopoDS_Shape occt_shape = reader.OneShape();
    if (occt_shape.IsNull()) {
      set_error(r_error, "Import STEP: file contained no shape");
      return OcctShapeHandle();
    }
    return make_handle(occt_shape);
  }
  catch (const Standard_Failure &err) {
    set_error(r_error, failure_message(err, "Import STEP: OCCT exception"));
    return OcctShapeHandle();
  }
  catch (const std::exception &err) {
    set_error(r_error, err.what());
    return OcctShapeHandle();
  }
}

OcctShapeHandle import_iges(const std::string &path, std::string *r_error)
{
  try {
    IGESControl_Reader reader;
    if (reader.ReadFile(path.c_str()) != IFSelect_RetDone) {
      set_error(r_error, "Import IGES: could not read file");
      return OcctShapeHandle();
    }
    reader.TransferRoots();
    const TopoDS_Shape occt_shape = reader.OneShape();
    if (occt_shape.IsNull()) {
      set_error(r_error, "Import IGES: file contained no shape");
      return OcctShapeHandle();
    }
    return make_handle(occt_shape);
  }
  catch (const Standard_Failure &err) {
    set_error(r_error, failure_message(err, "Import IGES: OCCT exception"));
    return OcctShapeHandle();
  }
  catch (const std::exception &err) {
    set_error(r_error, err.what());
    return OcctShapeHandle();
  }
}

/** \} */

}  // namespace blender::occt
