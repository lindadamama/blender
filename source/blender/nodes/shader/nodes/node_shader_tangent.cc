/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"
#include "node_util.hh"

#include "BKE_context.hh"

#include "DEG_depsgraph_query.hh"

#include "RNA_access.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::nodes::node_shader_tangent_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Tangent");
}

static void node_shader_buts_tangent(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  layout->prop(ptr, "direction_type", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);

  if (RNA_enum_get(ptr, "direction_type") == SHD_TANGENT_UVMAP) {
    PointerRNA obptr = CTX_data_pointer_get(C, "active_object");
    Object *object = static_cast<Object *>(obptr.data);

    if (object && object->type == OB_MESH) {
      Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);

      if (depsgraph) {
        Object *object_eval = DEG_get_evaluated(depsgraph, object);
        PointerRNA dataptr = RNA_id_pointer_create(static_cast<ID *>(object_eval->data));
        layout->prop_search(ptr, "uv_map", &dataptr, "uv_layers", "", ICON_GROUP_UVS);
        return;
      }
    }

    layout->prop(ptr, "uv_map", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_GROUP_UVS);
  }
  else {
    layout->prop(
        ptr, "axis", UI_ITEM_R_SPLIT_EMPTY_NAME | UI_ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  }
}

static void node_shader_init_tangent(bNodeTree * /*ntree*/, bNode *node)
{
  NodeShaderTangent *attr = MEM_callocN<NodeShaderTangent>("NodeShaderTangent");
  attr->axis = SHD_TANGENT_AXIS_Z;
  node->storage = attr;
}

static int node_shader_gpu_tangent(GPUMaterial *mat,
                                   bNode *node,
                                   bNodeExecData * /*execdata*/,
                                   GPUNodeStack *in,
                                   GPUNodeStack *out)
{
  NodeShaderTangent *attr = static_cast<NodeShaderTangent *>(node->storage);

  if (attr->direction_type == SHD_TANGENT_UVMAP) {
    return GPU_stack_link(
        mat, node, "node_tangentmap", in, out, GPU_attribute(mat, CD_TANGENT, attr->uv_map));
  }

  GPUNodeLink *orco = GPU_attribute(mat, CD_ORCO, "");

  if (attr->axis == SHD_TANGENT_AXIS_X) {
    GPU_link(mat, "tangent_orco_x", orco, &orco);
  }
  else if (attr->axis == SHD_TANGENT_AXIS_Y) {
    GPU_link(mat, "tangent_orco_y", orco, &orco);
  }
  else {
    GPU_link(mat, "tangent_orco_z", orco, &orco);
  }

  return GPU_stack_link(mat, node, "node_tangent", in, out, orco);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  /* TODO: implement other features */
  return create_node("tangent", NodeItem::Type::Vector3, {{"space", val(std::string("world"))}});
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace blender::nodes::node_shader_tangent_cc

/* node type definition */
void register_node_type_sh_tangent()
{
  namespace file_ns = blender::nodes::node_shader_tangent_cc;

  static blender::bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeTangent", SH_NODE_TANGENT);
  ntype.ui_name = "Tangent";
  ntype.ui_description = "Generate a tangent direction for the Anisotropic BSDF";
  ntype.enum_name_legacy = "TANGENT";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.draw_buttons = file_ns::node_shader_buts_tangent;
  blender::bke::node_type_size_preset(ntype, blender::bke::eNodeSizePreset::Middle);
  ntype.initfunc = file_ns::node_shader_init_tangent;
  ntype.gpu_fn = file_ns::node_shader_gpu_tangent;
  blender::bke::node_type_storage(
      ntype, "NodeShaderTangent", node_free_standard_storage, node_copy_standard_storage);
  ntype.materialx_fn = file_ns::node_shader_materialx;

  blender::bke::node_register_type(ntype);
}
