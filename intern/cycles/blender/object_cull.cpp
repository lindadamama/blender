/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cstdlib>

#include "scene/camera.h"

#include "blender/object_cull.h"
#include "blender/util.h"

CCL_NAMESPACE_BEGIN

BlenderObjectCulling::BlenderObjectCulling(Scene *scene, BL::Scene &b_scene)
    : use_scene_camera_cull_(false),
      use_camera_cull_(false),
      camera_cull_margin_(0.0f),
      use_scene_distance_cull_(false),
      use_distance_cull_(false),
      distance_cull_margin_(0.0f)
{
  if (b_scene.render().use_simplify()) {
    PointerRNA cscene = RNA_pointer_get(&b_scene.ptr, "cycles");

    const bool cam_supported = (scene->camera->get_camera_type() == CAMERA_PERSPECTIVE) ||
                               (scene->camera->get_camera_type() == CAMERA_ORTHOGRAPHIC);

    use_scene_camera_cull_ = cam_supported && !b_scene.render().use_multiview() &&
                             get_boolean(cscene, "use_camera_cull");
    use_scene_distance_cull_ = cam_supported && !b_scene.render().use_multiview() &&
                               get_boolean(cscene, "use_distance_cull");

    camera_cull_margin_ = get_float(cscene, "camera_cull_margin");
    distance_cull_margin_ = get_float(cscene, "distance_cull_margin");

    if (distance_cull_margin_ == 0.0f) {
      use_scene_distance_cull_ = false;
    }
  }
}

void BlenderObjectCulling::init_object(Scene *scene, BL::Object &b_ob)
{
  if (!use_scene_camera_cull_ && !use_scene_distance_cull_) {
    return;
  }

  PointerRNA cobject = RNA_pointer_get(&b_ob.ptr, "cycles");

  use_camera_cull_ = use_scene_camera_cull_ && get_boolean(cobject, "use_camera_cull");
  use_distance_cull_ = use_scene_distance_cull_ && get_boolean(cobject, "use_distance_cull");

  if (use_camera_cull_ || use_distance_cull_) {
    /* Need to have proper projection matrix. */
    scene->camera->update(scene);
  }
}

bool BlenderObjectCulling::test(Scene *scene, BL::Object &b_ob, Transform &tfm)
{
  if (!use_camera_cull_ && !use_distance_cull_) {
    return false;
  }

  /* Compute world space bounding box corners. */
  float3 bb[8];
  BL::Array<float, 24> boundbox = b_ob.bound_box();
  for (int i = 0; i < 8; ++i) {
    const float3 p = make_float3(boundbox[3 * i + 0], boundbox[3 * i + 1], boundbox[3 * i + 2]);
    bb[i] = transform_point(&tfm, p);
  }

  const bool camera_culled = use_camera_cull_ && test_camera(scene, bb);
  const bool distance_culled = use_distance_cull_ && test_distance(scene, bb);

  return ((camera_culled && distance_culled) || (camera_culled && !use_distance_cull_) ||
          (distance_culled && !use_camera_cull_));
}

/* TODO(sergey): Not really optimal, consider approaches based on k-DOP in order
 * to reduce number of objects which are wrongly considered visible.
 */
bool BlenderObjectCulling::test_camera(Scene *scene, const float3 bb[8])
{
  Camera *cam = scene->camera;
  const ProjectionTransform &worldtondc = cam->worldtondc;
  float3 bb_min = make_float3(FLT_MAX, FLT_MAX, FLT_MAX);
  float3 bb_max = make_float3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
  bool all_behind = true;
  for (int i = 0; i < 8; ++i) {
    float3 p = bb[i];
    const float4 b = make_float4(p, 1.0f);
    const float4 c = make_float4(
        dot(worldtondc.x, b), dot(worldtondc.y, b), dot(worldtondc.z, b), dot(worldtondc.w, b));
    p = make_float3(c / c.w);
    if (c.z < 0.0f) {
      p.x = 1.0f - p.x;
      p.y = 1.0f - p.y;
    }
    if (c.z >= -camera_cull_margin_) {
      all_behind = false;
    }
    bb_min = min(bb_min, p);
    bb_max = max(bb_max, p);
  }
  if (all_behind) {
    return true;
  }
  return (bb_min.x >= 1.0f + camera_cull_margin_ || bb_min.y >= 1.0f + camera_cull_margin_ ||
          bb_max.x <= -camera_cull_margin_ || bb_max.y <= -camera_cull_margin_);
}

bool BlenderObjectCulling::test_distance(Scene *scene, const float3 bb[8])
{
  const float3 camera_position = transform_get_column(&scene->camera->get_matrix(), 3);
  float3 bb_min = make_float3(FLT_MAX, FLT_MAX, FLT_MAX);
  float3 bb_max = make_float3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

  /* Find min & max points for x & y & z on bounding box */
  for (int i = 0; i < 8; ++i) {
    const float3 p = bb[i];
    bb_min = min(bb_min, p);
    bb_max = max(bb_max, p);
  }

  const float3 closest_point = max(min(bb_max, camera_position), bb_min);
  return (len_squared(camera_position - closest_point) >
          distance_cull_margin_ * distance_cull_margin_);
}

CCL_NAMESPACE_END
