#ifndef DM_APPLE_SPATIAL_MIXER_H
#define DM_APPLE_SPATIAL_MIXER_H
// macOS AudioToolbox 5.1 ambience-bed -> stereo AUSpatialMixer bridge.
// Must be created/disposed on the audio control thread, never in the output callback.
// The pull callback is invoked on the audio render thread and must not allocate/lock.
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct dm_apple_spatial dm_apple_spatial;
typedef void (*dm_spatial_pull_s16)(void *context, int16_t *interleaved51, size_t frames);
// Returns NULL when SpatialMixer is unavailable or cannot establish six-channel
// input, stereo output, and ambience-bed routing. The caller must then fall back.
dm_apple_spatial *dm_apple_spatial_create(double sample_rate, dm_spatial_pull_s16 pull,
                                          void *context, int want_head_tracking, int allow_personalized, int built_in_speakers);
int dm_apple_spatial_uses_built_in_speakers(const dm_apple_spatial *mixer);
void dm_apple_spatial_destroy(dm_apple_spatial *mixer);
// Call only while the unit is initialized and with no concurrent destroy.
// Input is pulled synchronously from pull(). Returns false on render failure.
int dm_apple_spatial_render(dm_apple_spatial *mixer, int16_t *stereo_out, size_t frames);
// 1 only if AudioUnit accepted the head-tracking property; says nothing about
// which headset is selected or whether entitlement/profile is active.
int dm_apple_spatial_head_tracking_requested(const dm_apple_spatial *mixer);
// Query on the control thread after initialization. Return true only for an active personalized HRTF.
int dm_apple_spatial_personalized(const dm_apple_spatial *mixer);
#ifdef __cplusplus
}
#endif
#endif
#endif
