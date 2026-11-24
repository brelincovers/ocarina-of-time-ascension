// Spatial Audio 2.0: host audio route control and read-only diagnostics.
// This interface NEVER claims binaural/head-tracked playback until a renderer
// explicitly implements and reports it. UI controls are non-realtime.
#ifndef DM_AUDIO_CONTROL_H
#define DM_AUDIO_CONTROL_H
#include <stdint.h>
#include "audio_route.h"

typedef enum {
    DM_AUDIO_MODE_AUTO = 0,          // use validated multichannel output when available
    DM_AUDIO_MODE_STEREO = 1,        // force stereo downmix of six-channel guest PCM
    DM_AUDIO_MODE_SURROUND = 2,      // explicitly prefer 5.1, safely fall back to stereo
    DM_AUDIO_MODE_BINAURAL = 3,      // portable fixed-head 5.1 -> stereo DSP
    DM_AUDIO_MODE_APPLE_SPATIAL = 4, // Apple 5.1 ambience-bed mixer, fixed
    DM_AUDIO_MODE_HEAD_TRACKED = 5   // request Apple head pose on a verified AirPods route
} dm_audio_mode;

// A preference is not evidence of a working multichannel device. This
// describes the reason that the current route is stereo / unavailable.
typedef enum {
    DM_AUDIO_FALLBACK_NONE = 0,
    DM_AUDIO_FALLBACK_WAITING,
    DM_AUDIO_FALLBACK_OUTPUT_UNAVAILABLE,
    DM_AUDIO_FALLBACK_STEREO_SELECTED,
    DM_AUDIO_FALLBACK_DEVICE_CHANNELS,
    DM_AUDIO_FALLBACK_UNVERIFIED_LAYOUT,
    DM_AUDIO_FALLBACK_SURROUND_SETUP_FAILED,
    DM_AUDIO_FALLBACK_VIRTUALIZER_INACTIVE,
    DM_AUDIO_FALLBACK_APPLE_MIXER_UNAVAILABLE
} dm_audio_fallback_reason;

typedef struct {
    dm_audio_mode preference;
    int output_open;
    int playing;                    // actual AudioUnit callback consumer, not silent sink
    unsigned source_channels;
    unsigned effective_channels;    // configured CoreAudio input channels, 0 if absent
    unsigned hardware_channels;     // CoreAudio output stream slots
    int surround_layout_valid;
    int built_in_speakers;
    int apple_speaker_renderer;
    int binaural_active;            // real virtualizer inserted into render callback
    int apple_spatial_configured;   // mixer created (might not have rendered yet)
    int head_tracking_available;   // the active CoreAudio output has a tested hardware model
    int head_tracking_configured;  // Apple accepted the head-pose request on the current mixer
    int personalized_active;       // read back from the active AUSpatialMixer
    int apple_spatial_active;       // Apple mixer successfully rendered PCM in current unit
    uint64_t apple_render_failures;
    dm_audio_fallback_reason fallback_reason;
    uint32_t device_id;
    char device_name[128];
    double device_sample_rate;
    uint32_t device_buffer_frames;
    uint64_t callbacks;
    uint64_t underruns;
    uint64_t zero_padded_bytes;
} dm_audio_snapshot;

// Pure route planner: always base the *effective* mode on the renderer's
// actual configured output, not just this candidate plan.
typedef struct {
    unsigned channels;
    int virtualize_51;
    dm_audio_route_reason reason;
} dm_audio_render_plan;
static inline dm_audio_render_plan dm_audio_choose_route(dm_audio_mode mode,
        unsigned source_channels, unsigned hw_channels, int ordered_51) {
    if (source_channels != 6)
        return (dm_audio_render_plan){source_channels, 0, DM_AUDIO_ROUTE_PASSTHROUGH};
    if (mode == DM_AUDIO_MODE_BINAURAL || mode == DM_AUDIO_MODE_APPLE_SPATIAL || mode == DM_AUDIO_MODE_HEAD_TRACKED)
        return (dm_audio_render_plan){2, 1, DM_AUDIO_ROUTE_STEREO_PREF};
    dm_audio_route_decision d = dm_audio_route_decide(6,
        mode == DM_AUDIO_MODE_AUTO || mode == DM_AUDIO_MODE_SURROUND,
        hw_channels, ordered_51);
    return (dm_audio_render_plan){d.channels, 0, d.reason};
}

// macOS-only entry points. Menus must call these off the real-time audio thread.
dm_audio_mode dm_audio_mode_get(void);
void dm_audio_mode_set(dm_audio_mode mode);
void dm_audio_get_snapshot(dm_audio_snapshot *out);
#endif
