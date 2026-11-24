// Frame-generation admission and ownership rules, independent of Metal or game state.
// These helpers do not predict a display deadline: the presenter must supply one
// from its actual cadence. NSScreen.maximumFramesPerSecond is not such a deadline.
#ifndef NVN_FG_POLICY_H
#define NVN_FG_POLICY_H

#include <stdint.h>
#include <stdatomic.h>

// The captured Ocarina camera metadata is verified only for the original 16:9
// scene. Expanded-aspect guest patches can change projection matrices without
// updating the scalar aspect/FOV consumed by MetalFX. Permit ordinary DRS pixel
// rounding, but do not infer a new camera contract from texture dimensions.
static inline int dm_fg_camera_shape_verified(uint64_t width, uint64_t height) {
    if (!width || !height) return 0;
    double relative = ((double)width / (double)height) / (16.0 / 9.0);
    return relative >= 0.99 && relative <= 1.01;
}

// A display epoch changes on route/refresh reconfiguration. A frame pair owns
// immutable absolute targets; acquiring a drawable must never retarget just one
// half of it. The genuine frame keeps its ordinary path if generation expires.
typedef struct { uint64_t epoch, next_tick_ns, interval_ns; } dm_fg_cadence;
typedef struct {
    uint64_t epoch, phase_ns, interval_ns, generated_ns, real_ns;
} dm_fg_pair;

static inline uint64_t dm_fg_distance(uint64_t a, uint64_t b) { return a > b ? a-b : b-a; }

static inline int dm_fg_pair_plan(const dm_fg_cadence *c, uint64_t now,
                                  uint64_t latest_generated, uint64_t previous_real,
                                  uint64_t frame_period, uint64_t tolerance,
                                  dm_fg_pair *out) {
    if (!c || !out || !c->epoch || !c->interval_ns || c->next_tick_ns <= now ||
        latest_generated < c->next_tick_ns || !frame_period) return 0;
    uint64_t half=frame_period/2, steps=half/c->interval_ns;
    if (half%c->interval_ns > c->interval_ns/2) steps++;
    if (!steps || steps > UINT64_MAX/c->interval_ns) return 0;
    uint64_t separation=steps*c->interval_ns;
    if (dm_fg_distance(separation,half)>tolerance) return 0;
    uint64_t g=c->next_tick_ns+((latest_generated-c->next_tick_ns)/c->interval_ns)*c->interval_ns;
    if (g<=now || g<=previous_real || g>UINT64_MAX-separation) return 0;
    *out=(dm_fg_pair){c->epoch,c->next_tick_ns,c->interval_ns,g,g+separation};
    return 1;
}

static inline int dm_fg_pair_valid(const dm_fg_pair *p, const dm_fg_cadence *c,
                                   uint64_t now, uint64_t tolerance) {
    if (!p || !c || !p->epoch || p->epoch!=c->epoch || !c->interval_ns ||
        c->next_tick_ns<=now || p->generated_ns<=now || p->real_ns<=p->generated_ns ||
        dm_fg_distance(p->interval_ns,c->interval_ns)>tolerance) return 0;
    uint64_t phase=dm_fg_distance(p->generated_ns,c->next_tick_ns)%c->interval_ns;
    return phase<=tolerance || c->interval_ns-phase<=tolerance;
}

// Zero means ordinary genuine presentation, with no added buffering. In
// particular a rejected/expired generated frame never reserves a delayed real.
static inline uint64_t dm_fg_pair_real_target(const dm_fg_pair *p, int generated_submitted, uint64_t now) {
    return p && generated_submitted && p->real_ns>now ? p->real_ns : 0;
}

// SDK contract: prevColorTexture must be the colour passed on the previous
// interpolation encode when reset is false. Capturing a colour alone is not an
// interpolation encode and cannot establish that history.
static inline int dm_fg_history_reset(uint64_t frame, uint64_t captured_previous,
                                      uint64_t encoded_previous, int previous_valid) {
    return !frame || !previous_valid || captured_previous!=frame-1 || encoded_previous!=frame-1;
}

// Measurements identify an actual completed scope, never an Off/On difference
// or the configured safety reserve. Missing/expired samples remain unknown.
typedef struct { uint64_t attempt, begin_ns, end_ns, duration_ns; } dm_fg_cost;
static inline int dm_fg_cost_record(dm_fg_cost *c, uint64_t attempt, uint64_t begin, uint64_t end) {
    if (!c || !attempt || !begin || end<=begin) return 0;
    if (c->end_ns>end) return 0; // callbacks can arrive out of order
    *c=(dm_fg_cost){attempt,begin,end,end-begin}; return 1;
}
static inline int dm_fg_cost_fresh(const dm_fg_cost *c, uint64_t now, uint64_t max_age) {
    return c && c->attempt && c->duration_ns && c->end_ns<=now && max_age && now-c->end_ns<=max_age;
}

// The caller must supply measured remaining work and an actual queue frontier.
// Adding a whole historical frame to a mid-frame CPU timestamp is not this
// estimate. Overflow or unavailable inputs produce no prediction.
static inline int dm_fg_remaining_ready(uint64_t now, uint64_t queue_ready,
                                       uint64_t remaining_ns, int measured, uint64_t *ready) {
    if (!ready || !measured || !queue_ready) return 0;
    uint64_t start=queue_ready>now?queue_ready:now;
    if (remaining_ns>UINT64_MAX-start) return 0;
    *ready=start+remaining_ns; return 1;
}

typedef enum {
    DM_FG_ADMIT,
    DM_FG_NO_CAPABILITY,
    DM_FG_NO_CADENCE,
    DM_FG_NO_BASE_TIMING,
    DM_FG_STALE_TIMING,
    DM_FG_GPU_BUDGET,
    DM_FG_QUEUE_BUSY,
    DM_FG_NO_DEADLINE,
    DM_FG_MISSED_DEADLINE
} dm_fg_admission_reason;

typedef struct {
    uint64_t now_ns, target_period_ns;
    // Measured display ticks, not the interval between genuine game frames.
    uint64_t display_interval_ns, cadence_tolerance_ns;
    uint64_t sample_end_ns, max_sample_age_ns, sample_total_ns;
    // Either compare the measured total of the same generated path (additional=0),
    // or subtract separately measured generation work in this SAME sample. A
    // baseline-only sample may add a conservative estimate in additional_gpu_ns;
    // the caller must identify that estimate honestly in telemetry.
    uint64_t sample_generation_ns, additional_gpu_ns;
    uint64_t predicted_ready_ns, deadline_ns;
    unsigned queued_real_frames, max_queued_real_frames;
    int capable, cadence_valid, deadline_valid;
    int sample_has_generation, sample_generation_measured, sample_is_same_generated_path;
} dm_fg_admission;

static inline dm_fg_admission_reason dm_fg_admit(const dm_fg_admission *a) {
    if (!a || !a->capable) return DM_FG_NO_CAPABILITY;
    if (!a->cadence_valid || !a->display_interval_ns || !a->target_period_ns ||
        // Subtraction avoids overflow for invalid or synthetic timestamps.
        (a->display_interval_ns > a->target_period_ns / 2 &&
         a->display_interval_ns - a->target_period_ns / 2 > a->cadence_tolerance_ns))
        return DM_FG_NO_CADENCE;
    if (!a->sample_total_ns || !a->sample_end_ns ||
        (a->sample_is_same_generated_path && (!a->sample_has_generation || a->additional_gpu_ns)) ||
        (a->sample_has_generation && !a->sample_is_same_generated_path && (!a->sample_generation_measured ||
         !a->sample_generation_ns || a->sample_generation_ns >= a->sample_total_ns)))
        return DM_FG_NO_BASE_TIMING;
    if (a->sample_end_ns > a->now_ns || !a->max_sample_age_ns ||
        a->now_ns - a->sample_end_ns > a->max_sample_age_ns)
        return DM_FG_STALE_TIMING;
    uint64_t base = a->sample_total_ns - (a->sample_has_generation && !a->sample_is_same_generated_path ? a->sample_generation_ns : 0);
    if (a->additional_gpu_ns >= a->target_period_ns ||
        base > a->target_period_ns - a->additional_gpu_ns)
        return DM_FG_GPU_BUDGET;
    if (a->queued_real_frames > a->max_queued_real_frames) return DM_FG_QUEUE_BUSY;
    if (!a->deadline_valid || !a->deadline_ns || !a->predicted_ready_ns)
        return DM_FG_NO_DEADLINE;
    if (a->deadline_ns <= a->now_ns || a->predicted_ready_ns < a->now_ns ||
        a->predicted_ready_ns > a->deadline_ns)
        return DM_FG_MISSED_DEADLINE;
    return DM_FG_ADMIT;
}

// Snapshot under the owning queue lock. A genuine-frame ticket becoming done is
// insufficient: failed/dropped genuine presentation does not complete these uses.
typedef struct {
    uint64_t composition_submitted, composition_done;
    uint64_t reader_submitted, reader_done;
    int queued;
} dm_fg_slot;

static inline int dm_fg_slot_reusable(const dm_fg_slot *s) {
    return s && !s->queued && s->composition_done >= s->composition_submitted &&
           s->reader_done >= s->reader_submitted;
}

typedef enum {
    DM_FG_PENDING,
    DM_FG_PRESENTED,
    DM_FG_DROPPED,
    DM_FG_REJECTED_LATE,
    DM_FG_REJECTED_QUEUE,
    DM_FG_REJECTED_UNAVAILABLE,
    DM_FG_FAILED,
    DM_FG_OUTCOME_COUNT
} dm_fg_outcome;

// One terminal outcome per encoded candidate. Ordinary frames without generation
// never own this token. IDs also prevent a delayed callback finishing a reused slot.
#define DM_FG_ATTEMPT_MAX (UINT64_MAX >> 8)
static inline int dm_fg_attempt_begin(_Atomic uint64_t *state, uint64_t id) {
    if (!state || !id || id > DM_FG_ATTEMPT_MAX) return 0;
    uint64_t old = atomic_load(state);
    for (;;) {
        // Never lose an unresolved outcome, or reuse an ID a delayed callback knows.
        if (old && (!(old & 255) || id <= (old >> 8))) return 0;
        if (atomic_compare_exchange_weak(state, &old, id << 8)) return 1;
    }
}

static inline int dm_fg_finish_once(_Atomic uint64_t *state, uint64_t id, dm_fg_outcome outcome) {
    if (!state || !id || id > DM_FG_ATTEMPT_MAX || outcome <= DM_FG_PENDING || outcome >= DM_FG_OUTCOME_COUNT) return 0;
    uint64_t expected = id << 8;
    return atomic_compare_exchange_strong(state, &expected, expected | (uint64_t)outcome);
}

// Missing timestamps mean unknown latency. Failure deliberately leaves the output
// untouched, so callers must test the return value before reporting a measurement.
static inline int dm_fg_latency_ns(uint64_t enqueue_ns, uint64_t presented_ns, uint64_t *latency_ns) {
    if (!latency_ns || !enqueue_ns || !presented_ns || presented_ns < enqueue_ns) return 0;
    *latency_ns = presented_ns - enqueue_ns;
    return 1;
}

#endif
