// nxnative Spatial Audio 2.0 — portable, fixed-head, generic 5.1 headphone virtualizer.
//
// This is a REAL PCM renderer. It is NOT Apple's personalized Spatial Audio,
// Dolby Atmos, an HRTF measurement set, or head tracking. Headphone tuning and
// subjective validation are required. It never reaches into CoreAudio.
//
// Guest order: L R C LFE Ls Rs. Output: interleaved stereo s16, 48 kHz.
// One render callback owns the state. No allocation, locks, syscalls, or logging
// in dm_binaural51_process(). Initialization/reset is ONLY between callbacks.
#ifndef DM_BINAURAL_51_H
#define DM_BINAURAL_51_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define DM_BINAURAL_RING 64u
#define DM_BINAURAL_FR_DELAY 6u
#define DM_BINAURAL_REAR_DELAY 17u

typedef struct {
    float delay[6][DM_BINAURAL_RING];
    float front_cross_l, front_cross_r;
    float rear_cross_l, rear_cross_r;
    float lfe_lowpass;
    float bass_alpha;
    unsigned cursor;
    uint64_t frames_rendered;
} dm_binaural51;

static inline void dm_binaural51_init(dm_binaural51 *s, unsigned sample_rate) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    // 1st-order low-pass for LFE, approximately 110 Hz at the supplied rate.
    // Avoid expf/powf here so the module stays testable on freestanding hosts.
    if (sample_rate < 8000 || sample_rate > 192000) sample_rate = 48000;
    s->bass_alpha = 690.0f / (float)sample_rate;
    if (s->bass_alpha > 1.f) s->bass_alpha = 1.f;
}

static inline int16_t dm_binaural51_s16(float sample) {
    // Quantize only once at output and saturate without signed overflow.
    if (sample >= 32767.f) return INT16_MAX;
    if (sample <= -32768.f) return INT16_MIN;
    return (int16_t)(sample >= 0.f ? sample + .5f : sample - .5f);
}

static inline void dm_binaural51_process(dm_binaural51 *s,
                                         const int16_t *in, int16_t *out,
                                         size_t frames) {
    if (!s || !in || !out) return;
    for (size_t f = 0; f < frames; ++f) {
        const int16_t *v = in + f * 6;
        unsigned p = s->cursor;
        for (unsigned c = 0; c < 6; ++c)
            s->delay[c][p] = (float)v[c];
        // Delayed, spectrally softened contralateral energy produces modest
        // interaural time/level differences without guessing head orientation.
        unsigned front = (p + DM_BINAURAL_RING - DM_BINAURAL_FR_DELAY) % DM_BINAURAL_RING;
        unsigned rear = (p + DM_BINAURAL_RING - DM_BINAURAL_REAR_DELAY) % DM_BINAURAL_RING;
        s->front_cross_l += .28f * (s->delay[1][front] - s->front_cross_l);
        s->front_cross_r += .28f * (s->delay[0][front] - s->front_cross_r);
        s->rear_cross_l += .18f * (s->delay[5][rear] - s->rear_cross_l);
        s->rear_cross_r += .18f * (s->delay[4][rear] - s->rear_cross_r);
        s->lfe_lowpass += s->bass_alpha * ((float)v[3] - s->lfe_lowpass);
        // Conservative headroom; surrounds have distinct lateral energy.
        // The LFE feed is intentionally modest; no invented LFE bass boost.
        float left = (.50f * v[0] + .39f * v[2] + .34f * v[4] +
                      .19f * s->front_cross_l + .12f * s->rear_cross_l +
                      .08f * s->lfe_lowpass) * .74f;
        float right = (.50f * v[1] + .39f * v[2] + .34f * v[5] +
                       .19f * s->front_cross_r + .12f * s->rear_cross_r +
                       .08f * s->lfe_lowpass) * .74f;
        out[2 * f] = dm_binaural51_s16(left);
        out[2 * f + 1] = dm_binaural51_s16(right);
        s->cursor = (p + 1u) % DM_BINAURAL_RING;
    }
    s->frames_rendered += frames;
}
#endif
