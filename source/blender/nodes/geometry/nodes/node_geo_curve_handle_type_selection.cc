/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_handle_type_selection_cc {

NODE_STORAGE_FUNCS(NodeGeometryCurveSelectHandles)

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Bool>("Selection").field_source();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "mode", UI_ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout->prop(ptr, "handle_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryCurveSelectHandles *data = MEM_callocN<NodeGeometryCurveSelectHandles>(__func__);

  data->handle_type = GEO_NODE_CURVE_HANDLE_AUTO;
  data->mode = GEO_NODE_CURVE_HANDLE_LEFT | GEO_NODE_CURVE_HANDLE_RIGHT;
  node->storage = data;
}

static HandleType handle_type_from_input_type(const GeometryNodeCurveHandleType type)
{
  switch (type) {
    case GEO_NODE_CURVE_HANDLE_AUTO:
      return BEZIER_HANDLE_AUTO;
    case GEO_NODE_CURVE_HANDLE_ALIGN:
      return BEZIER_HANDLE_ALIGN;
    case GEO_NODE_CURVE_HANDLE_FREE:
      return BEZIER_HANDLE_FREE;
    case GEO_NODE_CURVE_HANDLE_VECTOR:
      return BEZIER_HANDLE_VECTOR;
  }
  BLI_assert_unreachable();
  return BEZIER_HANDLE_AUTO;
}

static void select_by_handle_type(const bke::CurvesGeometry &curves,
                                  const HandleType type,
                                  const GeometryNodeCurveHandleMode mode,
                                  const MutableSpan<bool> r_selection)
{
  const OffsetIndices points_by_curve = curves.points_by_curve();
  VArray<int8_t> curve_types = curves.curve_types();
  VArray<int8_t> left = curves.handle_types_left();
  VArray<int8_t> right = curves.handle_types_right();

  for (const int i_curve : curves.curves_range()) {
    const IndexRange points = points_by_curve[i_curve];
    if (curve_types[i_curve] != CURVE_TYPE_BEZIER) {
      r_selection.slice(points).fill(false);
    }
    else {
      for (const int i_point : points) {
        r_selection[i_point] = (mode & GEO_NODE_CURVE_HANDLE_LEFT && left[i_point] == type) ||
                               (mode & GEO_NODE_CURVE_HANDLE_RIGHT && right[i_point] == type);
      }
    }
  }
}

class HandleTypeFieldInput final : public bke::CurvesFieldInput {
  HandleType type_;
  GeometryNodeCurveHandleMode mode_;

 public:
  HandleTypeFieldInput(HandleType type, GeometryNodeCurveHandleMode mode)
      : bke::CurvesFieldInput(CPPType::get<bool>(), "Handle Type Selection node"),
        type_(type),
        mode_(mode)
  {
    category_ = Category::Generated;
  }

  GVArray get_varray_for_context(const bke::CurvesGeometry &curves,
                                 const AttrDomain domain,
                                 const IndexMask &mask) const final
  {
    if (domain != AttrDomain::Point) {
      return {};
    }
    Array<bool> selection(mask.min_array_size());
    select_by_handle_type(curves, type_, mode_, selection);
    return VArray<bool>::from_container(std::move(selection));
  }

  uint64_t hash() const final
  {
    return get_default_hash(int(mode_), int(type_));
  }

  bool is_equal_to(const fn::FieldNode &other) const final
  {
    if (const HandleTypeFieldInput *other_handle_selection =
            dynamic_cast<const HandleTypeFieldInput *>(&other))
    {
      return mode_ == other_handle_selection->mode_ && type_ == other_handle_selection->type_;
    }
    return false;
  }

  std::optional<AttrDomain> preferred_domain(const bke::CurvesGeometry & /*curves*/) const final
  {
    return AttrDomain::Point;
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const NodeGeometryCurveSelectHandles &storage = node_storage(params.node());
  const HandleType handle_type = handle_type_from_input_type(
      (GeometryNodeCurveHandleType)storage.handle_type);
  const GeometryNodeCurveHandleMode mode = (GeometryNodeCurveHandleMode)storage.mode;

  Field<bool> selection_field{std::make_shared<HandleTypeFieldInput>(handle_type, mode)};
  params.set_output("Selection", std::move(selection_field));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(
      &ntype, "GeometryNodeCurveHandleTypeSelection", GEO_NODE_CURVE_HANDLE_TYPE_SELECTION);
  ntype.ui_name = "Handle Type Selection";
  ntype.ui_description = "Provide a selection based on the handle types of Bézier control points";
  ntype.enum_name_legacy = "CURVE_HANDLE_TYPE_SELECTION";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.initfunc = node_init;
  blender::bke::node_type_storage(ntype,
                                  "NodeGeometryCurveSelectHandles",
                                  node_free_standard_storage,
                                  node_copy_standard_storage);
  ntype.draw_buttons = node_layout;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curve_handle_type_selection_cc
