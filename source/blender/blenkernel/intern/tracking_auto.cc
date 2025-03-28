/* SPDX-FileCopyrightText: 2011 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cstdlib>

#include "MEM_guardedalloc.h"

#include "DNA_movieclip_types.h"
#include "DNA_object_types.h" /* SELECT */

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_task.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "BKE_movieclip.h"
#include "BKE_tracking.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "libmv-capi.h"
#include "tracking_private.h"

struct AutoTrackClip {
  MovieClip *clip;

  /* Dimensions of movie frame, in pixels.
   *
   * NOTE: All frames within a clip are expected to have match3ed dimensions. */
  int width, height;
};

struct AutoTrackTrack {
  /* Index of a clip from `AutoTrackContext::autotrack_clips` this track belongs to. */
  int clip_index;

  MovieTrackingTrack *track;

  /* Options for the region tracker. */
  libmv_TrackRegionOptions track_region_options;

  /* Denotes whether this track will be tracked.
   * Is usually initialized based on track's selection. Non-trackable tracks are still added to the
   * context to provide AutoTrack all knowledge about what is going on in the scene. */
  bool is_trackable;
};

struct AutoTrackMarker {
  libmv_Marker libmv_marker;
};

/* Result of tracking step for a single marker.
 *
 * On success both marker and result are fully initialized to the position on the new frame.
 *
 * On failure marker's frame number is initialized to frame number where it was attempted to be
 * tracked to. The position and other fields of tracked marker are the same as the input. */
struct AutoTrackTrackingResult {
  AutoTrackTrackingResult *next, *prev;

  bool success;
  libmv_Marker libmv_marker;
  libmv_TrackRegionResult libmv_result;
};

struct AutoTrackContext {
  /* --------------------------------------------------------------------
   * Invariant part.
   * Stays unchanged during the tracking process.
   * If not the initialization process, all the fields here should be treated as `const`.
   */

  /* Frame at which tracking process started.
   * NOTE: Measured in scene time frames, */
  int start_scene_frame;

  /* True when tracking backwards (from higher frame number to lower frame number.) */
  bool is_backwards;

  /* Movie clips used during the tracking process. */
  int num_clips;
  AutoTrackClip autotrack_clips[MAX_ACCESSOR_CLIP];

  /* Tracks for which the context has been created for.
   * This is a flat array of all tracks coming from all clips, regardless of whether track is
   * actually being tracked or not. This allows the AutoTrack to see a big picture of hat is going
   * on in the scene, and request information it needs.
   *
   * Indexed by AutoTrackOptions::track_index. */
  int num_all_tracks;
  AutoTrackTrack *all_autotrack_tracks;

  /* Accessor for images of clip. Used by the autotrack context. */
  TrackingImageAccessor *image_accessor;

  /* Image buffers acquired for markers which are using keyframe pattern matching.
   * These image buffers are user-referenced and flagged as persistent so that they don't get
   * removed from the movie cache during tracking. */
  int num_referenced_image_buffers;
  ImBuf **referenced_image_buffers;

  /* --------------------------------------------------------------------
   * Variant part.
   * Denotes tracing state and tracking result.
   */

  /* Auto-track context.
   *
   * NOTE: Is accessed from multiple threads at once. */
  libmv_AutoTrack *autotrack;

  /* Markers from the current frame which will be tracked to the next frame upon the tracking
   * context step.
   *
   * NOTE: This array is re-used across tracking steps, which might make it appear that the array
   * is over-allocated when some tracks has failed to track. */
  int num_autotrack_markers;
  AutoTrackMarker *autotrack_markers;

  /* Tracking results which are to be synchronized from the AutoTrack context to the Blender's
   * DNA to make the results visible for users. */
  ListBase results_to_sync;
  int synchronized_scene_frame;

  SpinLock spin_lock;
};

/* -------------------------------------------------------------------- */
/** \name Marker coordinate system conversion.
 * \{ */

static void normalized_to_libmv_frame(const float normalized[2],
                                      const int frame_dimensions[2],
                                      float result[2])
{
  result[0] = normalized[0] * frame_dimensions[0] - 0.5f;
  result[1] = normalized[1] * frame_dimensions[1] - 0.5f;
}

static void normalized_relative_to_libmv_frame(const float normalized[2],
                                               const float origin[2],
                                               const int frame_dimensions[2],
                                               float result[2])
{
  result[0] = (normalized[0] + origin[0]) * frame_dimensions[0] - 0.5f;
  result[1] = (normalized[1] + origin[1]) * frame_dimensions[1] - 0.5f;
}

static void libmv_frame_to_normalized(const float frame_coord[2],
                                      const int frame_dimensions[2],
                                      float result[2])
{
  result[0] = (frame_coord[0] + 0.5f) / frame_dimensions[0];
  result[1] = (frame_coord[1] + 0.5f) / frame_dimensions[1];
}

static void libmv_frame_to_normalized_relative(const float frame_coord[2],
                                               const float origin[2],
                                               const int frame_dimensions[2],
                                               float result[2])
{
  result[0] = (frame_coord[0] - origin[0]) / frame_dimensions[0];
  result[1] = (frame_coord[1] - origin[1]) / frame_dimensions[1];
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Conversion of markers between Blender's DNA and Libmv.
 * \{ */

static libmv_Marker dna_marker_to_libmv_marker(/*const*/ MovieTrackingTrack &track,
                                               const MovieTrackingMarker &marker,
                                               const int clip,
                                               const int track_index,
                                               const int frame_width,
                                               const int frame_height,
                                               const bool backwards)
{
  libmv_Marker libmv_marker{};

  const int frame_dimensions[2] = {frame_width, frame_height};
  libmv_marker.clip = clip;
  libmv_marker.frame = marker.framenr;
  libmv_marker.track = track_index;

  normalized_to_libmv_frame(marker.pos, frame_dimensions, libmv_marker.center);
  for (int i = 0; i < 4; i++) {
    normalized_relative_to_libmv_frame(
        marker.pattern_corners[i], marker.pos, frame_dimensions, libmv_marker.patch[i]);
  }

  normalized_relative_to_libmv_frame(
      marker.search_min, marker.pos, frame_dimensions, libmv_marker.search_region_min);

  normalized_relative_to_libmv_frame(
      marker.search_max, marker.pos, frame_dimensions, libmv_marker.search_region_max);

  /* NOTE: All the markers does have 1.0 weight.
   * Might support in the future, but will require more elaborated process which will involve
   * F-Curve evaluation. */
  libmv_marker.weight = 1.0f;

  if (marker.flag & MARKER_TRACKED) {
    libmv_marker.source = LIBMV_MARKER_SOURCE_TRACKED;
  }
  else {
    libmv_marker.source = LIBMV_MARKER_SOURCE_MANUAL;
  }
  libmv_marker.status = LIBMV_MARKER_STATUS_UNKNOWN;
  libmv_marker.model_type = LIBMV_MARKER_MODEL_TYPE_POINT;
  libmv_marker.model_id = 0;

  /* NOTE: We currently don't support reference marker from different clip. */
  libmv_marker.reference_clip = clip;

  if (track.pattern_match == TRACK_MATCH_KEYFRAME) {
    const MovieTrackingMarker *keyframe_marker = tracking_get_keyframed_marker(
        &track, marker.framenr, backwards);
    libmv_marker.reference_frame = keyframe_marker->framenr;
  }
  else {
    libmv_marker.reference_frame = backwards ? marker.framenr - 1 : marker.framenr;
  }

  libmv_marker.disabled_channels =
      ((track.flag & TRACK_DISABLE_RED) ? LIBMV_MARKER_CHANNEL_R : 0) |
      ((track.flag & TRACK_DISABLE_GREEN) ? LIBMV_MARKER_CHANNEL_G : 0) |
      ((track.flag & TRACK_DISABLE_BLUE) ? LIBMV_MARKER_CHANNEL_B : 0);

  return libmv_marker;
}

static MovieTrackingMarker libmv_marker_to_dna_marker(const libmv_Marker &libmv_marker,
                                                      const int frame_width,
                                                      const int frame_height)
{
  MovieTrackingMarker marker{};

  const int frame_dimensions[2] = {frame_width, frame_height};
  marker.framenr = libmv_marker.frame;

  libmv_frame_to_normalized(libmv_marker.center, frame_dimensions, marker.pos);
  for (int i = 0; i < 4; i++) {
    libmv_frame_to_normalized_relative(
        libmv_marker.patch[i], libmv_marker.center, frame_dimensions, marker.pattern_corners[i]);
  }

  libmv_frame_to_normalized_relative(
      libmv_marker.search_region_min, libmv_marker.center, frame_dimensions, marker.search_min);

  libmv_frame_to_normalized_relative(
      libmv_marker.search_region_max, libmv_marker.center, frame_dimensions, marker.search_max);

  marker.flag = 0;
  if (libmv_marker.source == LIBMV_MARKER_SOURCE_TRACKED) {
    marker.flag |= MARKER_TRACKED;
  }
  else {
    marker.flag &= ~MARKER_TRACKED;
  }

  return marker;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name General helpers.
 *
 * TODO(sergey): Should be moved to `tracking_util.cc`.
 * \{ */

/* Returns false if marker crossed margin area from frame bounds. */
static bool tracking_check_marker_margin(const libmv_Marker &libmv_marker,
                                         const int margin,
                                         const int frame_width,
                                         const int frame_height)
{
  float patch_min[2], patch_max[2];
  float margin_left, margin_top, margin_right, margin_bottom;

  INIT_MINMAX2(patch_min, patch_max);
  minmax_v2v2_v2(patch_min, patch_max, libmv_marker.patch[0]);
  minmax_v2v2_v2(patch_min, patch_max, libmv_marker.patch[1]);
  minmax_v2v2_v2(patch_min, patch_max, libmv_marker.patch[2]);
  minmax_v2v2_v2(patch_min, patch_max, libmv_marker.patch[3]);

  margin_left = max_ff(libmv_marker.center[0] - patch_min[0], margin);
  margin_top = max_ff(patch_max[1] - libmv_marker.center[1], margin);
  margin_right = max_ff(patch_max[0] - libmv_marker.center[0], margin);
  margin_bottom = max_ff(libmv_marker.center[1] - patch_min[1], margin);

  if (libmv_marker.center[0] < margin_left ||
      libmv_marker.center[0] > frame_width - margin_right ||
      libmv_marker.center[1] < margin_bottom || libmv_marker.center[1] > frame_height - margin_top)
  {
    return false;
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Auto-Track Context Initialization
 * \{ */

static bool autotrack_is_marker_usable(const MovieTrackingMarker &marker)
{
  if (marker.flag & MARKER_DISABLED) {
    return false;
  }
  return true;
}

static bool autotrack_is_track_trackable(const AutoTrackContext *context,
                                         const AutoTrackTrack *autotrack_track)
{
  /*const*/ MovieTrackingTrack *track = autotrack_track->track;
  if (TRACK_SELECTED(track) && (track->flag & (TRACK_LOCKED | TRACK_HIDDEN)) == 0) {
    const AutoTrackClip *autotrack_clip = &context->autotrack_clips[autotrack_track->clip_index];
    MovieClip *clip = autotrack_clip->clip;
    const int clip_frame_number = BKE_movieclip_remap_scene_to_clip_frame(
        clip, context->start_scene_frame);

    const MovieTrackingMarker *marker = BKE_tracking_marker_get(track, clip_frame_number);
    return autotrack_is_marker_usable(*marker);
  }
  return false;
}

static void autotrack_context_init_clips(AutoTrackContext *context,
                                         MovieClip *clip,
                                         MovieClipUser *user)
{
  /* NOTE: Currently only tracking within a single clip. */

  context->num_clips = 1;

  context->autotrack_clips[0].clip = clip;
  BKE_movieclip_get_size(
      clip, user, &context->autotrack_clips[0].width, &context->autotrack_clips[0].height);
}

/* Initialize flat list of tracks for quick index-based access for the specified clip.
 * All the tracks from this clip are added at the end of the array of already-collected tracks.
 *
 * NOTE: Clips should be initialized first. */
static void autotrack_context_init_tracks_for_clip(AutoTrackContext *context, int clip_index)
{
  BLI_assert(clip_index >= 0);
  BLI_assert(clip_index < context->num_clips);

  const AutoTrackClip *autotrack_clip = &context->autotrack_clips[clip_index];
  MovieClip *clip = autotrack_clip->clip;
  const MovieTrackingObject *tracking_object = BKE_tracking_object_get_active(&clip->tracking);

  const int num_clip_tracks = BLI_listbase_count(&tracking_object->tracks);
  if (num_clip_tracks == 0) {
    return;
  }

  context->all_autotrack_tracks = static_cast<AutoTrackTrack *>(
      MEM_reallocN(context->all_autotrack_tracks,
                   (context->num_all_tracks + num_clip_tracks) * sizeof(AutoTrackTrack)));

  LISTBASE_FOREACH (MovieTrackingTrack *, track, &tracking_object->tracks) {
    AutoTrackTrack *autotrack_track = &context->all_autotrack_tracks[context->num_all_tracks++];
    autotrack_track->clip_index = clip_index;
    autotrack_track->track = track;
    autotrack_track->is_trackable = autotrack_is_track_trackable(context, autotrack_track);

    tracking_configure_tracker(
        track, nullptr, context->is_backwards, &autotrack_track->track_region_options);
  }
}

/* Initialize flat list of tracks for quick index-based access for all clips used for tracking.
 *
 * NOTE: Clips should be initialized first. */
static void autotrack_context_init_tracks(AutoTrackContext *context)
{
  BLI_assert(context->num_clips >= 1);

  for (int clip_index = 0; clip_index < context->num_clips; ++clip_index) {
    autotrack_context_init_tracks_for_clip(context, clip_index);
  }
}

/* NOTE: Clips should be initialized first. */
static void autotrack_context_init_image_accessor(AutoTrackContext *context)
{
  BLI_assert(context->num_clips >= 1);

  /* Planarize arrays of clips and tracks, storing pointers to their base "objects".
   * This allows image accessor to be independent, but adds some overhead here. Could be solved
   * by either more strongly coupling accessor API with the AutoTrack, or by giving some functors
   * to the accessor to access clip/track from their indices. */

  MovieClip *clips[MAX_ACCESSOR_CLIP];
  for (int i = 0; i < context->num_clips; ++i) {
    clips[i] = context->autotrack_clips[i].clip;
  }

  MovieTrackingTrack **tracks = MEM_calloc_arrayN<MovieTrackingTrack *>(
      context->num_all_tracks, "image accessor init tracks");
  for (int i = 0; i < context->num_all_tracks; ++i) {
    tracks[i] = context->all_autotrack_tracks[i].track;
  }

  context->image_accessor = tracking_image_accessor_new(clips, 1, tracks, context->num_all_tracks);

  MEM_freeN(tracks);
}

/* Count markers which are usable to be passed to the AutoTrack context. */
static size_t autotrack_count_all_usable_markers(AutoTrackContext *context)
{
  size_t num_usable_markers = 0;
  for (int track_index = 0; track_index < context->num_all_tracks; ++track_index) {
    const MovieTrackingTrack *track = context->all_autotrack_tracks[track_index].track;
    for (int marker_index = 0; marker_index < track->markersnr; ++marker_index) {
      const MovieTrackingMarker &marker = track->markers[marker_index];
      if (!autotrack_is_marker_usable(marker)) {
        continue;
      }
      num_usable_markers++;
    }
  }
  return num_usable_markers;
}

static int autotrack_count_trackable_markers(AutoTrackContext *context)
{
  int num_trackable_markers = 0;
  for (int track_index = 0; track_index < context->num_all_tracks; ++track_index) {
    const AutoTrackTrack *autotrack_track = &context->all_autotrack_tracks[track_index];
    if (!autotrack_track->is_trackable) {
      continue;
    }
    num_trackable_markers++;
  }
  return num_trackable_markers;
}

/* Provide Libmv side of auto track all information about given tracks.
 * Information from all clips is passed to the auto tracker.
 *
 * NOTE: Clips and all tracks are to be initialized before calling this. */
static void autotrack_context_init_autotrack(AutoTrackContext *context)
{
  context->autotrack = libmv_autoTrackNew(context->image_accessor->libmv_accessor);

  /* Count number of markers to be put to a context. */
  const size_t num_trackable_markers = autotrack_count_all_usable_markers(context);
  if (num_trackable_markers == 0) {
    return;
  }

  /* Allocate memory for all the markers. */
  libmv_Marker *libmv_markers = MEM_calloc_arrayN<libmv_Marker>(num_trackable_markers,
                                                                "libmv markers array");

  /* Fill in markers array. */
  int num_filled_libmv_markers = 0;
  for (int track_index = 0; track_index < context->num_all_tracks; ++track_index) {
    const AutoTrackTrack *autotrack_track = &context->all_autotrack_tracks[track_index];
    /*const*/ MovieTrackingTrack &track = *autotrack_track->track;
    for (int marker_index = 0; marker_index < track.markersnr; ++marker_index) {
      /*const*/ MovieTrackingMarker &marker = track.markers[marker_index];
      if (!autotrack_is_marker_usable(marker)) {
        continue;
      }
      const AutoTrackClip *autotrack_clip = &context->autotrack_clips[autotrack_track->clip_index];
      libmv_markers[num_filled_libmv_markers++] = dna_marker_to_libmv_marker(
          track,
          marker,
          autotrack_track->clip_index,
          track_index,
          autotrack_clip->width,
          autotrack_clip->height,
          context->is_backwards);
    }
  }

  /* Add all markers to autotrack. */
  libmv_autoTrackSetMarkers(context->autotrack, libmv_markers, num_trackable_markers);

  /* Free temporary memory. */
  MEM_freeN(libmv_markers);
}

static void autotrack_context_init_markers(AutoTrackContext *context)
{
  /* Count number of trackable tracks. */
  context->num_autotrack_markers = autotrack_count_trackable_markers(context);
  if (context->num_autotrack_markers == 0) {
    return;
  }

  /* Allocate required memory. */
  context->autotrack_markers = MEM_calloc_arrayN<AutoTrackMarker>(context->num_autotrack_markers,
                                                                  "auto track options");

  /* Fill in all the markers. */
  int autotrack_marker_index = 0;
  for (int track_index = 0; track_index < context->num_all_tracks; ++track_index) {
    const AutoTrackTrack *autotrack_track = &context->all_autotrack_tracks[track_index];
    if (!autotrack_track->is_trackable) {
      continue;
    }

    const AutoTrackClip *autotrack_clip = &context->autotrack_clips[autotrack_track->clip_index];
    MovieClip *clip = autotrack_clip->clip;
    const int clip_frame_number = BKE_movieclip_remap_scene_to_clip_frame(
        clip, context->start_scene_frame);

    /*const*/ MovieTrackingTrack &track = *context->all_autotrack_tracks[track_index].track;
    const MovieTrackingMarker &marker = *BKE_tracking_marker_get(&track, clip_frame_number);

    AutoTrackMarker *autotrack_marker = &context->autotrack_markers[autotrack_marker_index++];
    autotrack_marker->libmv_marker = dna_marker_to_libmv_marker(track,
                                                                marker,
                                                                autotrack_track->clip_index,
                                                                track_index,
                                                                autotrack_clip->width,
                                                                autotrack_clip->height,
                                                                context->is_backwards);
  }
}

AutoTrackContext *BKE_autotrack_context_new(MovieClip *clip,
                                            MovieClipUser *user,
                                            const bool is_backwards)
{
  AutoTrackContext *context = MEM_callocN<AutoTrackContext>("autotrack context");

  context->start_scene_frame = user->framenr;
  context->is_backwards = is_backwards;
  context->synchronized_scene_frame = context->start_scene_frame;

  autotrack_context_init_clips(context, clip, user);
  autotrack_context_init_tracks(context);
  autotrack_context_init_image_accessor(context);
  autotrack_context_init_autotrack(context);
  autotrack_context_init_markers(context);

  BLI_spin_init(&context->spin_lock);

  return context;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Context tracking start.
 *
 * Called from possible job once before performing tracking steps.
 * \{ */

static void reference_keyframed_image_buffers(AutoTrackContext *context)
{
  /* NOTE: This is potentially over-allocating, but it simplifies memory manipulation.
   * In practice this is unlikely to be noticed in the profiler as the memory footprint of this
   * data is way less of what the tracking process will use. */
  context->referenced_image_buffers = MEM_calloc_arrayN<ImBuf *>(context->num_autotrack_markers,
                                                                 __func__);

  context->num_referenced_image_buffers = 0;

  for (int i = 0; i < context->num_autotrack_markers; ++i) {
    const AutoTrackMarker *autotrack_marker = &context->autotrack_markers[i];
    const int clip_index = autotrack_marker->libmv_marker.clip;
    const int track_index = autotrack_marker->libmv_marker.track;

    const AutoTrackClip *autotrack_clip = &context->autotrack_clips[clip_index];
    const AutoTrackTrack *autotrack_track = &context->all_autotrack_tracks[track_index];
    const MovieTrackingTrack *track = autotrack_track->track;

    if (track->pattern_match != TRACK_MATCH_KEYFRAME) {
      continue;
    }

    const int scene_frame = BKE_movieclip_remap_clip_to_scene_frame(
        autotrack_clip->clip, autotrack_marker->libmv_marker.reference_frame);

    MovieClipUser user_at_keyframe;
    BKE_movieclip_user_set_frame(&user_at_keyframe, scene_frame);
    user_at_keyframe.render_size = MCLIP_PROXY_RENDER_SIZE_FULL;
    user_at_keyframe.render_flag = 0;

    /* Keep reference to the image buffer so that we can manipulate its flags later on.
     * Also request the movie cache to not remove the image buffer from the cache. */
    ImBuf *ibuf = BKE_movieclip_get_ibuf(autotrack_clip->clip, &user_at_keyframe);
    ibuf->userflags |= IB_PERSISTENT;

    context->referenced_image_buffers[context->num_referenced_image_buffers++] = ibuf;
  }
}

void BKE_autotrack_context_start(AutoTrackContext *context)
{
  reference_keyframed_image_buffers(context);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Threaded context step (tracking process).
 * \{ */

/* NOTE: This is a TLS in a sense that this struct is never accessed from multiple threads, and
 * that threads are re-using the struct as much as possible. */
struct AutoTrackTLS {
  ListBase results; /* Elements of `AutoTrackTrackingResult`. */
};

static void autotrack_context_step_cb(void *__restrict userdata,
                                      const int marker_index,
                                      const TaskParallelTLS *__restrict tls)
{
  AutoTrackContext *context = static_cast<AutoTrackContext *>(userdata);
  AutoTrackTLS *autotrack_tls = (AutoTrackTLS *)tls->userdata_chunk;

  const AutoTrackMarker &autotrack_marker = context->autotrack_markers[marker_index];
  const libmv_Marker &libmv_current_marker = autotrack_marker.libmv_marker;

  const int frame_delta = context->is_backwards ? -1 : 1;
  const int clip_index = libmv_current_marker.clip;
  const int track_index = libmv_current_marker.track;

  const AutoTrackClip &autotrack_clip = context->autotrack_clips[clip_index];
  const AutoTrackTrack &autotrack_track = context->all_autotrack_tracks[track_index];
  const MovieTrackingTrack &track = *autotrack_track.track;

  /* Check whether marker is going outside of allowed frame margin. */
  if (!tracking_check_marker_margin(
          libmv_current_marker, track.margin, autotrack_clip.width, autotrack_clip.height))
  {
    return;
  }

  const int new_marker_frame = libmv_current_marker.frame + frame_delta;

  AutoTrackTrackingResult *autotrack_result = MEM_callocN<AutoTrackTrackingResult>(
      "autotrack result");
  autotrack_result->libmv_marker = libmv_current_marker;
  autotrack_result->libmv_marker.frame = new_marker_frame;

  /* Update reference frame. */
  libmv_Marker libmv_reference_marker;
  if (track.pattern_match == TRACK_MATCH_KEYFRAME) {
    autotrack_result->libmv_marker.reference_frame = libmv_current_marker.reference_frame;
    libmv_autoTrackGetMarker(context->autotrack,
                             clip_index,
                             autotrack_result->libmv_marker.reference_frame,
                             track_index,
                             &libmv_reference_marker);
  }
  else {
    BLI_assert(track.pattern_match == TRACK_MATCH_PREVIOUS_FRAME);
    autotrack_result->libmv_marker.reference_frame = libmv_current_marker.frame;
    libmv_reference_marker = libmv_current_marker;
  }

  /* Perform actual tracking. */
  autotrack_result->success = libmv_autoTrackMarker(context->autotrack,
                                                    &autotrack_track.track_region_options,
                                                    &autotrack_result->libmv_marker,
                                                    &autotrack_result->libmv_result);

  /* If tracking failed restore initial position.
   * This is how Blender side is currently expecting failed track to be handled. Without this the
   * marker is left in an arbitrary position which did not provide good correlation. */
  if (!autotrack_result->success) {
    autotrack_result->libmv_marker = libmv_current_marker;
    autotrack_result->libmv_marker.frame = new_marker_frame;
  }

  BLI_addtail(&autotrack_tls->results, autotrack_result);
}

static void autotrack_context_reduce(const void *__restrict /*userdata*/,
                                     void *__restrict chunk_join,
                                     void *__restrict chunk)
{
  AutoTrackTLS *autotrack_tls = (AutoTrackTLS *)chunk;
  if (BLI_listbase_is_empty(&autotrack_tls->results)) {
    /* Nothing to be joined from. */
    return;
  }

  AutoTrackTLS *autotrack_tls_join = (AutoTrackTLS *)chunk_join;
  BLI_movelisttolist(&autotrack_tls_join->results, &autotrack_tls->results);
}

bool BKE_autotrack_context_step(AutoTrackContext *context)
{
  if (context->num_autotrack_markers == 0) {
    return false;
  }

  AutoTrackTLS tls;
  BLI_listbase_clear(&tls.results);

  TaskParallelSettings settings;
  BLI_parallel_range_settings_defaults(&settings);
  settings.use_threading = (context->num_autotrack_markers > 1);
  settings.userdata_chunk = &tls;
  settings.userdata_chunk_size = sizeof(AutoTrackTLS);
  settings.func_reduce = autotrack_context_reduce;

  BLI_task_parallel_range(
      0, context->num_autotrack_markers, context, autotrack_context_step_cb, &settings);

  /* Prepare next tracking step by updating the AutoTrack context with new markers and moving
   * tracked markers as an input for the next iteration. */
  context->num_autotrack_markers = 0;
  LISTBASE_FOREACH (AutoTrackTrackingResult *, autotrack_result, &tls.results) {
    if (!autotrack_result->success) {
      continue;
    }

    /* Insert tracking results to the AutoTrack context to make them usable for the next frame
     * tracking iteration. */
    libmv_autoTrackAddMarker(context->autotrack, &autotrack_result->libmv_marker);

    /* Update the list of markers which will be tracked on the next iteration. */
    context->autotrack_markers[context->num_autotrack_markers++].libmv_marker =
        autotrack_result->libmv_marker;
  }

  BLI_spin_lock(&context->spin_lock);
  BLI_movelisttolist(&context->results_to_sync, &tls.results);
  BLI_spin_unlock(&context->spin_lock);

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Context data synchronization.
 *
 * Used to copy tracking result to Blender side, while the tracking is still happening in its
 * thread.
 *
 * \{ */

void BKE_autotrack_context_sync(AutoTrackContext *context)
{
  const int frame_delta = context->is_backwards ? -1 : 1;

  BLI_spin_lock(&context->spin_lock);
  ListBase results_to_sync = context->results_to_sync;
  BLI_listbase_clear(&context->results_to_sync);
  BLI_spin_unlock(&context->spin_lock);

  LISTBASE_FOREACH_MUTABLE (AutoTrackTrackingResult *, autotrack_result, &results_to_sync) {
    const libmv_Marker &libmv_marker = autotrack_result->libmv_marker;
    const int clip_index = libmv_marker.clip;
    const int track_index = libmv_marker.track;
    const AutoTrackClip &autotrack_clip = context->autotrack_clips[clip_index];
    const MovieClip *clip = autotrack_clip.clip;
    const AutoTrackTrack &autotrack_track = context->all_autotrack_tracks[track_index];
    MovieTrackingTrack *track = autotrack_track.track;

    const int start_clip_frame = BKE_movieclip_remap_scene_to_clip_frame(
        clip, context->start_scene_frame);
    const int first_result_frame = start_clip_frame + frame_delta;

    /* Insert marker which corresponds to the tracking result. */
    MovieTrackingMarker marker = libmv_marker_to_dna_marker(
        autotrack_result->libmv_marker, autotrack_clip.width, autotrack_clip.height);
    if (!autotrack_result->success) {
      marker.flag |= MARKER_DISABLED;
    }
    BKE_tracking_marker_insert(track, &marker);

    /* Insert disabled marker at the end of tracked segment.
     * When tracking forward the disabled marker is added at the next frame from the result,
     * when tracking backwards the marker is added at the previous frame. */
    tracking_marker_insert_disabled(track, &marker, context->is_backwards, false);

    if (marker.framenr == first_result_frame) {
      MovieTrackingMarker *prev_marker = BKE_tracking_marker_get_exact(
          track, marker.framenr - frame_delta);
      BLI_assert(prev_marker != nullptr);

      tracking_marker_insert_disabled(track, prev_marker, !context->is_backwards, false);
    }

    /* Update synchronized frame to the latest tracked fame from the current results. */
    const int marker_scene_frame = BKE_movieclip_remap_clip_to_scene_frame(clip, marker.framenr);
    if (context->is_backwards) {
      context->synchronized_scene_frame = min_ii(context->synchronized_scene_frame,
                                                 marker_scene_frame);
    }
    else {
      context->synchronized_scene_frame = max_ii(context->synchronized_scene_frame,
                                                 marker_scene_frame);
    }

    MEM_freeN(autotrack_result);
  }

  for (int clip_index = 0; clip_index < context->num_clips; clip_index++) {
    MovieTracking *tracking = &context->autotrack_clips[clip_index].clip->tracking;
    BKE_tracking_dopesheet_tag_update(tracking);
  }
}

void BKE_autotrack_context_sync_user(AutoTrackContext *context, MovieClipUser *user)
{
  /* TODO(sergey): Find a way to avoid this function,
   * somehow making all needed logic in #BKE_autotrack_context_sync(). */

  user->framenr = context->synchronized_scene_frame;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Finalization.
 * \{ */

void BKE_autotrack_context_finish(AutoTrackContext *context)
{
  for (int clip_index = 0; clip_index < context->num_clips; clip_index++) {
    const AutoTrackClip *autotrack_clip = &context->autotrack_clips[clip_index];
    MovieClip *clip = autotrack_clip->clip;
    const MovieTrackingObject *tracking_object = BKE_tracking_object_get_active(&clip->tracking);
    const int start_clip_frame = BKE_movieclip_remap_scene_to_clip_frame(
        clip, context->start_scene_frame);

    LISTBASE_FOREACH (MovieTrackingPlaneTrack *, plane_track, &tracking_object->plane_tracks) {
      if (plane_track->flag & PLANE_TRACK_AUTOKEY) {
        continue;
      }
      for (int track_index = 0; track_index < context->num_all_tracks; track_index++) {
        const AutoTrackTrack *autotrack_track = &context->all_autotrack_tracks[track_index];
        if (!autotrack_track->is_trackable) {
          continue;
        }
        MovieTrackingTrack *track = autotrack_track->track;
        if (BKE_tracking_plane_track_has_point_track(plane_track, track)) {
          BKE_tracking_track_plane_from_existing_motion(plane_track, start_clip_frame);
          break;
        }
      }
    }
  }
}

static void release_keyframed_image_buffers(AutoTrackContext *context)
{
  for (int i = 0; i < context->num_referenced_image_buffers; ++i) {
    ImBuf *ibuf = context->referenced_image_buffers[i];

    /* Restore flag. It is not expected that anyone else is setting this flag on image buffers from
     * movie clip, so can simply clear the flag. */
    ibuf->userflags &= ~IB_PERSISTENT;
    IMB_freeImBuf(ibuf);
  }

  MEM_freeN(context->referenced_image_buffers);
}

void BKE_autotrack_context_free(AutoTrackContext *context)
{
  if (context->autotrack != nullptr) {
    libmv_autoTrackDestroy(context->autotrack);
  }

  if (context->image_accessor != nullptr) {
    tracking_image_accessor_destroy(context->image_accessor);
  }

  release_keyframed_image_buffers(context);

  MEM_SAFE_FREE(context->all_autotrack_tracks);
  MEM_SAFE_FREE(context->autotrack_markers);

  BLI_freelistN(&context->results_to_sync);

  BLI_spin_end(&context->spin_lock);

  MEM_freeN(context);
}

/** \} */
