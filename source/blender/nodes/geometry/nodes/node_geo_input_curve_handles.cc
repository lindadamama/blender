/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_curve_handles_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>("Relative")
      .default_value(false)
      .supports_field()
      .description(
          "Output the handle positions relative to the corresponding control point "
          "instead of in the local space of the geometry");
  b.add_output<decl::Vector>("Left").field_source_reference_all();
  b.add_output<decl::Vector>("Right").field_source_reference_all();
}

class HandlePositionFieldInput final : public bke::GeometryFieldInput {
  Field<bool> relative_;
  bool left_;

 public:
  HandlePositionFieldInput(Field<bool> relative, bool left)
      : bke::GeometryFieldInput(CPPType::get<float3>(), "Handle"), relative_(relative), left_(left)
  {
  }

  GVArray get_varray_for_context(const bke::GeometryFieldContext &context,
                                 const IndexMask &mask) const final
  {
    const bke::CurvesGeometry *curves_ptr = context.curves_or_strokes();
    if (!curves_ptr) {
      return {};
    }
    const bke::CurvesGeometry &curves = *curves_ptr;
    const bke::AttrDomain domain = context.domain();

    const bke::GeometryFieldContext field_context{context, AttrDomain::Point};
    fn::FieldEvaluator evaluator(field_context, &mask);
    evaluator.add(relative_);
    evaluator.evaluate();
    const VArray<bool> relative = evaluator.get_evaluated<bool>(0);

    const Span<float3> positions = curves.positions();

    const AttributeAccessor attributes = curves.attributes();
    StringRef side = left_ ? "handle_left" : "handle_right";
    VArray<float3> handles = *attributes.lookup_or_default<float3>(
        side, AttrDomain::Point, {0, 0, 0});

    if (relative.is_single()) {
      if (relative.get_internal_single()) {
        Array<float3> output(positions.size());
        for (const int i : positions.index_range()) {
          output[i] = handles[i] - positions[i];
        }
        return attributes.adapt_domain<float3>(
            VArray<float3>::from_container(std::move(output)), AttrDomain::Point, domain);
      }
      return attributes.adapt_domain<float3>(handles, AttrDomain::Point, domain);
    }

    Array<float3> output(positions.size());
    for (const int i : positions.index_range()) {
      if (relative[i]) {
        output[i] = handles[i] - positions[i];
      }
      else {
        output[i] = handles[i];
      }
    }
    return attributes.adapt_domain<float3>(
        VArray<float3>::from_container(std::move(output)), AttrDomain::Point, domain);
  }

  void for_each_field_input_recursive(FunctionRef<void(const FieldInput &)> fn) const final
  {
    relative_.node().for_each_field_input_recursive(fn);
  }

  uint64_t hash() const final
  {
    return get_default_hash(relative_, left_);
  }

  bool is_equal_to(const fn::FieldNode &other) const final
  {
    if (const HandlePositionFieldInput *other_handle =
            dynamic_cast<const HandlePositionFieldInput *>(&other))
    {
      return relative_ == other_handle->relative_ && left_ == other_handle->left_;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(
      const bke::GeometryComponent & /*component*/) const final
  {
    return AttrDomain::Point;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<bool> relative = params.extract_input<Field<bool>>("Relative");
  Field<float3> left_field{std::make_shared<HandlePositionFieldInput>(relative, true)};
  Field<float3> right_field{std::make_shared<HandlePositionFieldInput>(relative, false)};

  params.set_output("Left", std::move(left_field));
  params.set_output("Right", std::move(right_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(
      &ntype, "GeometryNodeInputCurveHandlePositions", GEO_NODE_INPUT_CURVE_HANDLES);
  ntype.ui_name = "Curve Handle Positions";
  ntype.ui_description = "Retrieve the position of each Bézier control point's handles";
  ntype.enum_name_legacy = "INPUT_CURVE_HANDLES";
  ntype.nclass = NODE_CLASS_INPUT;
  blender::bke::node_type_size_preset(ntype, blender::bke::eNodeSizePreset::Middle);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_input_curve_handles_cc
