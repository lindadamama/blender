/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_subdiv_modifier.hh"

#include "MEM_guardedalloc.h"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_subdiv.hh"

#include "GPU_capabilities.hh"
#include "GPU_context.hh"

using namespace blender::bke;

subdiv::Settings BKE_subsurf_modifier_settings_init(const SubsurfModifierData *smd,
                                                    const bool use_render_params)
{
  const int requested_levels = (use_render_params) ? smd->renderLevels : smd->levels;

  subdiv::Settings settings{};
  settings.is_simple = (smd->subdivType == SUBSURF_TYPE_SIMPLE);
  settings.is_adaptive = !(smd->flags & eSubsurfModifierFlag_UseRecursiveSubdivision);
  settings.level = settings.is_simple ? 1 :
                                        (settings.is_adaptive ? smd->quality : requested_levels);
  settings.use_creases = (smd->flags & eSubsurfModifierFlag_UseCrease);
  settings.vtx_boundary_interpolation = subdiv::vtx_boundary_interpolation_from_subsurf(
      smd->boundary_smooth);
  settings.fvar_linear_interpolation = subdiv::fvar_interpolation_from_uv_smooth(smd->uv_smooth);

  return settings;
}

bool BKE_subsurf_modifier_runtime_init(SubsurfModifierData *smd, const bool use_render_params)
{
  subdiv::Settings settings = BKE_subsurf_modifier_settings_init(smd, use_render_params);

  SubsurfRuntimeData *runtime_data = (SubsurfRuntimeData *)smd->modifier.runtime;
  if (settings.level == 0) {
    /* Modifier is effectively disabled, but still update settings if runtime data
     * was already allocated. */
    if (runtime_data) {
      runtime_data->settings = settings;

      runtime_data->used_cpu = runtime_data->used_gpu = 0;
    }

    return false;
  }

  /* Allocate runtime data if it did not exist yet. */
  if (runtime_data == nullptr) {
    runtime_data = MEM_callocN<SubsurfRuntimeData>(__func__);
    smd->modifier.runtime = runtime_data;
  }
  runtime_data->settings = settings;
  return true;
}

bool BKE_subsurf_modifier_use_custom_loop_normals(const SubsurfModifierData *smd, const Mesh *mesh)
{
  if ((smd->flags & eSubsurfModifierFlag_UseCustomNormals) == 0) {
    return false;
  }
  const std::optional<AttributeMetaData> meta_data = mesh->attributes().lookup_meta_data(
      "custom_normal");
  return meta_data && meta_data->domain == AttrDomain::Corner &&
         meta_data->data_type == AttrType::Int16_2D;
}

bool BKE_subsurf_modifier_has_split_normals(const SubsurfModifierData *smd, const Mesh *mesh)
{
  return BKE_subsurf_modifier_use_custom_loop_normals(smd, mesh) ||
         mesh->normals_domain() == MeshNormalDomain::Corner;
}

static bool is_subdivision_evaluation_possible_on_gpu()
{
  if (GPU_backend_get_type() == GPU_BACKEND_NONE) {
    return false;
  }

  if (GPU_max_compute_shader_storage_blocks() < MAX_GPU_SUBDIV_SSBOS) {
    return false;
  }

  return true;
}

bool BKE_subsurf_modifier_force_disable_gpu_evaluation_for_mesh(const SubsurfModifierData *smd,
                                                                const Mesh *mesh)
{
  if ((U.gpu_flag & USER_GPU_FLAG_SUBDIVISION_EVALUATION) == 0) {
    /* GPU subdivision is explicitly disabled, so we don't force it. */
    return false;
  }

  if (!is_subdivision_evaluation_possible_on_gpu()) {
    /* The GPU type is not compatible with the subdivision. */
    return false;
  }

  /* Deactivate GPU subdivision if sharp edges or custom normals are used as those are
   * complicated to support on GPU, and should really be separate workflows. */
  return BKE_subsurf_modifier_has_split_normals(smd, mesh);
}

bool BKE_subsurf_modifier_can_do_gpu_subdiv(const SubsurfModifierData *smd, const Mesh *mesh)
{
  return (U.gpu_flag & USER_GPU_FLAG_SUBDIVISION_EVALUATION) &&
         is_subdivision_evaluation_possible_on_gpu() &&
         !BKE_subsurf_modifier_has_split_normals(smd, mesh);
}

void (*BKE_subsurf_modifier_free_gpu_cache_cb)(subdiv::Subdiv *subdiv) = nullptr;

subdiv::Subdiv *BKE_subsurf_modifier_subdiv_descriptor_ensure(SubsurfRuntimeData *runtime_data,
                                                              const Mesh *mesh,
                                                              const bool for_draw_code)
{
  if (for_draw_code) {
    runtime_data->used_gpu = 2; /* countdown in frames */

    return runtime_data->subdiv_gpu = subdiv::update_from_mesh(
               runtime_data->subdiv_gpu, &runtime_data->settings, mesh);
  }
  runtime_data->used_cpu = 2;
  return runtime_data->subdiv_cpu = subdiv::update_from_mesh(
             runtime_data->subdiv_cpu, &runtime_data->settings, mesh);
}

int BKE_subsurf_modifier_eval_required_mode(bool is_final_render, bool is_edit_mode)
{
  if (is_final_render) {
    return eModifierMode_Render;
  }

  return eModifierMode_Realtime | (is_edit_mode ? int(eModifierMode_Editmode) : 0);
}
