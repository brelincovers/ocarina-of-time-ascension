#ifndef DM_FPS_ACTIVITY_H
#define DM_FPS_ACTIVITY_H
#include <math.h>
#include <stdint.h>

// Keep the UI's foreground policy separate from counters that can still advance
// while DM_FOCUS_KEEP / DM_WINDOW_TOP permit an unattended background workload.
typedef enum {
    DM_FPS_MEASURED, DM_FPS_APP_INACTIVE, DM_FPS_MINIMIZED, DM_FPS_OCCLUDED,
    DM_FPS_FIRST_SAMPLE, DM_FPS_RESUMING, DM_FPS_SHORT_INTERVAL,
    DM_FPS_STALE_INTERVAL, DM_FPS_COUNTER_RESET
} dm_fps_gate;
static inline dm_fps_gate dm_fps_gate_reason(int app_active, int minimized, int visible,
    int previous_foreground, double last_time, double elapsed,
    uint64_t rendered, uint64_t previous_rendered, uint64_t presented, uint64_t previous_presented) {
    if (!app_active) return DM_FPS_APP_INACTIVE;
    if (minimized) return DM_FPS_MINIMIZED;
    if (!visible) return DM_FPS_OCCLUDED;
    if (!(last_time > 0)) return DM_FPS_FIRST_SAMPLE;
    if (!previous_foreground) return DM_FPS_RESUMING;
    if (!isfinite(elapsed) || elapsed < 0 || elapsed >= 2) return DM_FPS_STALE_INTERVAL;
    if (elapsed < .25) return DM_FPS_SHORT_INTERVAL;
    if (rendered < previous_rendered || presented < previous_presented) return DM_FPS_COUNTER_RESET;
    return DM_FPS_MEASURED;
}
static inline const char *dm_fps_gate_name(dm_fps_gate gate) {
    switch (gate) {
        case DM_FPS_MEASURED: return "measured";
        case DM_FPS_APP_INACTIVE: return "application inactive";
        case DM_FPS_MINIMIZED: return "window minimized";
        case DM_FPS_OCCLUDED: return "window occluded";
        case DM_FPS_FIRST_SAMPLE: return "first sample";
        case DM_FPS_RESUMING: return "resuming foreground sampling";
        case DM_FPS_SHORT_INTERVAL: return "sample interval below 0.25 seconds";
        case DM_FPS_STALE_INTERVAL: return "stale or invalid sample interval";
        case DM_FPS_COUNTER_RESET: return "counter reset; establishing a new baseline";
    }
    return "unknown sampling state";
}
#endif
