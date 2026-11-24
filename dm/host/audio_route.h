// Pure channel-layout policy. Hardware probing and mixer selection live in
// hle_audio.c. This must remain testable without CoreAudio or an Apple device.
#ifndef DM_AUDIO_ROUTE_H
#define DM_AUDIO_ROUTE_H

typedef enum {
    DM_AUDIO_ROUTE_SURROUND,      // discrete 5.1 is supported and selected
    DM_AUDIO_ROUTE_STEREO_PREF,   // user requested stereo only
    DM_AUDIO_ROUTE_HW_CHANNELS,   // output has fewer than six slots
    DM_AUDIO_ROUTE_HW_LAYOUT,     // six+ slots but speaker order isn't verified
    DM_AUDIO_ROUTE_PASSTHROUGH    // guest didn't request six channels
} dm_audio_route_reason;

typedef struct {
    unsigned channels;
    dm_audio_route_reason reason;
} dm_audio_route_decision;

static inline dm_audio_route_decision dm_audio_route_decide(unsigned guest_channels,
        int enable_surround, unsigned hardware_channels, int surround_layout_ok) {
    if (guest_channels != 6)
        return (dm_audio_route_decision){guest_channels, DM_AUDIO_ROUTE_PASSTHROUGH};
    if (!enable_surround)
        return (dm_audio_route_decision){2, DM_AUDIO_ROUTE_STEREO_PREF};
    if (hardware_channels < 6)
        return (dm_audio_route_decision){2, DM_AUDIO_ROUTE_HW_CHANNELS};
    if (!surround_layout_ok)
        return (dm_audio_route_decision){2, DM_AUDIO_ROUTE_HW_LAYOUT};
    return (dm_audio_route_decision){6, DM_AUDIO_ROUTE_SURROUND};
}

// The six Nintendo input channels and the AudioUnit 5.1 input layout have
// precisely this order: L, R, C, LFE, Ls, Rs. A device that merely *contains*
// those speaker labels in a different order is NOT equivalent to this layout.
// These semantic IDs decouple the pure verification test from Apple SDK types.
typedef enum {
    DM_AUDIO_SPEAKER_LEFT = 1,
    DM_AUDIO_SPEAKER_RIGHT,
    DM_AUDIO_SPEAKER_CENTER,
    DM_AUDIO_SPEAKER_LFE,
    DM_AUDIO_SPEAKER_LEFT_SURROUND,
    DM_AUDIO_SPEAKER_RIGHT_SURROUND,
    DM_AUDIO_SPEAKER_UNKNOWN
} dm_audio_speaker;

static inline int dm_audio_layout_is_ordered_51(const dm_audio_speaker *speakers,
                                                 unsigned count) {
    static const dm_audio_speaker expected[] = {
        DM_AUDIO_SPEAKER_LEFT, DM_AUDIO_SPEAKER_RIGHT,
        DM_AUDIO_SPEAKER_CENTER, DM_AUDIO_SPEAKER_LFE,
        DM_AUDIO_SPEAKER_LEFT_SURROUND, DM_AUDIO_SPEAKER_RIGHT_SURROUND
    };
    if (!speakers || count != 6) return 0;
    for (unsigned i = 0; i < 6; ++i)
        if (speakers[i] != expected[i]) return 0;
    return 1;
}

// Compatibility with the Phase 3 helper and its callers.
static inline unsigned dm_audio_route_channels(unsigned guest_channels, int opt_in,
                                                unsigned hardware_channels, int surround_layout_ok) {
    return dm_audio_route_decide(guest_channels, opt_in,
                                 hardware_channels, surround_layout_ok).channels;
}
#endif
