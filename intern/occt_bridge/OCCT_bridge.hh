/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_occt_bridge
 *
 * Thin C++ facade over OCCT (Open CASCADE Technology) for CAD solid
 * modeling. Compiled only when `WITH_OCCT` is enabled.
 *
 * HARD RULE: this header must not include any OCCT header. OCCT headers
 * are macro-heavy (`Handle`, `Standard_EXPORT`, ...) and would leak into
 * every translation unit that includes this facade. All OCCT types and
 * calls are quarantined in `intern/OCCT_bridge.cc` behind the pimpl
 * `OcctShapeImpl`.
 *
 * Error convention: functions that take a `std::string *r_error` write a
 * human-readable message into it (when non-null) and return an invalid
 * (empty) handle on failure. Functions without an `r_error` simply return
 * an invalid handle / empty result on failure.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender::occt {

/** Pimpl wrapping a `TopoDS_Shape`; complete only inside `OCCT_bridge.cc`. */
struct OcctShapeImpl;

/**
 * Move-only owning handle over an OCCT shape (`TopoDS_Shape`).
 *
 * A default-constructed handle is empty/invalid. Handles returned by the
 * factory functions below own their shape; ownership transfers on move,
 * leaving the moved-from handle invalid. Copying is disabled -- OCCT
 * shapes are internally reference-counted, but this facade deliberately
 * keeps single-owner semantics to make lifetimes obvious to callers.
 */
class OcctShapeHandle {
 public:
  /** Construct an empty/invalid handle. */
  OcctShapeHandle();
  /** Internal: take ownership of a complete impl (bridge implementation use only). */
  explicit OcctShapeHandle(std::unique_ptr<OcctShapeImpl> impl);
  ~OcctShapeHandle();

  OcctShapeHandle(OcctShapeHandle &&other) noexcept;
  OcctShapeHandle &operator=(OcctShapeHandle &&other) noexcept;

  OcctShapeHandle(const OcctShapeHandle &) = delete;
  OcctShapeHandle &operator=(const OcctShapeHandle &) = delete;

  /** True when this handle owns a non-null OCCT shape. */
  bool is_valid() const;

  /** Internal: pimpl access for the bridge implementation (null when invalid). */
  OcctShapeImpl *impl()
  {
    return impl_.get();
  }
  const OcctShapeImpl *impl() const
  {
    return impl_.get();
  }

 private:
  std::unique_ptr<OcctShapeImpl> impl_;
};

/**
 * True when OCCT support is compiled in. This facade is only built with
 * `WITH_OCCT`, so a linked-in bridge always reports true; callers reached
 * through optional linkage use this as the single availability check.
 */
bool is_available();

/**
 * Create a planar rectangular profile face on the XY plane, centered at
 * the origin, spanning `size_x` x `size_y`. Returns an invalid handle when
 * either size is not strictly positive or construction fails.
 */
OcctShapeHandle make_rect_profile(float size_x, float size_y);

/**
 * Extrude a planar profile face into a solid prism along +Z by `distance`
 * (negative extrudes along -Z). On failure writes a message to `r_error`
 * (when non-null) and returns an invalid handle.
 */
OcctShapeHandle extrude_profile(const OcctShapeHandle &profile,
                                float distance,
                                std::string *r_error);

/** Boolean operation kinds for #boolean_op. */
enum class BooleanOp {
  Union,
  Subtract,
  Intersect,
};

/**
 * Boolean operation `a` (op) `b`, returning a new shape; the operands are
 * not modified. On failure writes a message to `r_error` (when non-null)
 * and returns an invalid handle.
 */
OcctShapeHandle boolean_op(const OcctShapeHandle &a,
                           const OcctShapeHandle &b,
                           BooleanOp op,
                           std::string *r_error);

/**
 * Round ALL edges of `shape` with a uniform `radius`, returning a new
 * shape; the operand is not modified. v1 deliberately fillets every edge
 * (no per-edge selection) to sidestep the topological-naming problem. On
 * failure -- including a non-positive `radius` -- writes a message to
 * `r_error` (when non-null) and returns an invalid handle.
 */
OcctShapeHandle fillet_all_edges(const OcctShapeHandle &shape, float radius, std::string *r_error);

/**
 * Chamfer ALL edges of `shape` with a uniform setback `distance`,
 * returning a new shape; the operand is not modified. On failure --
 * including a non-positive `distance` -- writes a message to `r_error`
 * (when non-null) and returns an invalid handle.
 */
OcctShapeHandle chamfer_all_edges(const OcctShapeHandle &shape,
                                  float distance,
                                  std::string *r_error);

/**
 * Return a transformed copy of `shape`. `m` is a 4x4 matrix in Blender's
 * column-major convention (`m[col][row]`, translation in `m[3][0..2]`).
 * Note: OCCT rigid transforms reject shear and non-uniform scale; such a
 * matrix yields an invalid handle (see the implementation for details).
 */
OcctShapeHandle transformed(const OcctShapeHandle &shape, const float m[4][4]);

/** Triangulated approximation of a shape, ready for Blender mesh conversion. */
struct TessellatedMesh {
  Vector<float3> verts;
  /** Triangles as 0-based indices into `verts`. */
  Vector<int3> tris;
};

/**
 * Tessellate `shape` into triangles. `linear_deflection` is the maximum
 * chord distance between the triangulation and the true surface (in model
 * units); `angular_deflection` is the maximum angle between adjacent
 * facet normals (radians). Returns an empty mesh for an invalid handle or
 * on failure.
 */
TessellatedMesh tessellate(const OcctShapeHandle &shape,
                           float linear_deflection,
                           float angular_deflection);

/**
 * Serialize a shape to OCCT's binary BRep format, suitable for storage in
 * a data-block. Returns an empty vector for an invalid handle or on
 * failure. Round-trips through #deserialize.
 */
Vector<uint8_t> serialize(const OcctShapeHandle &shape);

/**
 * Reconstruct a shape from a blob produced by #serialize. Returns an
 * invalid handle when the blob is empty or malformed.
 */
OcctShapeHandle deserialize(Span<const uint8_t> blob);

/**
 * Write `shape` to a STEP file at `path`. Returns false and writes a
 * message to `r_error` (when non-null) on failure.
 */
bool export_step(const OcctShapeHandle &shape, const std::string &path, std::string *r_error);

/**
 * Write `shape` to an IGES file at `path`. Returns false and writes a
 * message to `r_error` (when non-null) on failure.
 */
bool export_iges(const OcctShapeHandle &shape, const std::string &path, std::string *r_error);

/**
 * Read the first/root solid from a STEP file at `path`. On failure writes
 * a message to `r_error` (when non-null) and returns an invalid handle.
 */
OcctShapeHandle import_step(const std::string &path, std::string *r_error);

/**
 * Read the first/root solid from an IGES file at `path`. On failure writes
 * a message to `r_error` (when non-null) and returns an invalid handle.
 */
OcctShapeHandle import_iges(const std::string &path, std::string *r_error);

}  // namespace blender::occt
