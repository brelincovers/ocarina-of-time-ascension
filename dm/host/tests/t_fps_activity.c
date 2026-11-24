// CPU-only activity policy: no AppKit, window, GPU, game or simulated clock.
#include "../fps_activity.h"
#include <stdio.h>
#include <string.h>
static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main(void) {
    CHECK(dm_fps_gate_reason(1,0,1,1,1,.5,115,100,230,200) == DM_FPS_MEASURED);
    CHECK(dm_fps_gate_reason(0,0,1,1,1,.5,115,100,230,200) == DM_FPS_APP_INACTIVE);
    CHECK(dm_fps_gate_reason(1,1,1,1,1,.5,115,100,230,200) == DM_FPS_MINIMIZED);
    CHECK(dm_fps_gate_reason(1,0,0,1,1,.5,115,100,230,200) == DM_FPS_OCCLUDED);
    CHECK(dm_fps_gate_reason(1,0,1,0,0,.5,115,100,230,200) == DM_FPS_FIRST_SAMPLE);
    CHECK(dm_fps_gate_reason(1,0,1,0,1,.5,115,100,230,200) == DM_FPS_RESUMING);
    CHECK(dm_fps_gate_reason(1,0,1,1,1,.249,115,100,230,200) == DM_FPS_SHORT_INTERVAL);
    CHECK(dm_fps_gate_reason(1,0,1,1,1,.25,115,100,230,200) == DM_FPS_MEASURED);
    CHECK(dm_fps_gate_reason(1,0,1,1,1,2,115,100,230,200) == DM_FPS_STALE_INTERVAL);
    CHECK(dm_fps_gate_reason(1,0,1,1,1,NAN,115,100,230,200) == DM_FPS_STALE_INTERVAL);
    CHECK(dm_fps_gate_reason(1,0,1,1,1,-.5,115,100,230,200) == DM_FPS_STALE_INTERVAL);
    CHECK(dm_fps_gate_reason(1,0,1,1,1,.5,1,100,230,200) == DM_FPS_COUNTER_RESET);
    CHECK(dm_fps_gate_reason(1,0,1,1,1,.5,115,100,1,200) == DM_FPS_COUNTER_RESET);
    // Paused/stopped counters measure zero, never an old positive cached rate.
    CHECK(dm_fps_gate_reason(1,0,1,1,1,.5,100,100,200,200) == DM_FPS_MEASURED);
    // Display/fullscreen/refresh changes do not fabricate counters or alter eligibility.
    CHECK(dm_fps_gate_reason(1,0,1,1,1,.75,122,100,222,200) == DM_FPS_MEASURED);
    CHECK(!strcmp(dm_fps_gate_name(DM_FPS_APP_INACTIVE), "application inactive"));
    printf("FPS activity: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
