// Standalone routing policy regression tests. No Apple frameworks or audio device.
#include <assert.h>
#include <stdio.h>
#include "../audio_route.h"
#include "../audio_control.h"

static void test_full_matrix(void) {
    for (unsigned guest = 1; guest <= 8; ++guest)
        for (unsigned hw = 0; hw <= 12; ++hw)
            for (int enabled = 0; enabled <= 1; ++enabled)
                for (int layout = 0; layout <= 1; ++layout) {
                    dm_audio_route_decision r = dm_audio_route_decide(guest, enabled, hw, layout);
                    unsigned expected = guest != 6 ? guest :
                        ((enabled && hw >= 6 && layout) ? 6u : 2u);
                    assert(r.channels == expected);
                    assert(dm_audio_route_channels(guest, enabled, hw, layout) == expected);
                    if (guest != 6) assert(r.reason == DM_AUDIO_ROUTE_PASSTHROUGH);
                    else if (!enabled) assert(r.reason == DM_AUDIO_ROUTE_STEREO_PREF);
                    else if (hw < 6) assert(r.reason == DM_AUDIO_ROUTE_HW_CHANNELS);
                    else if (!layout) assert(r.reason == DM_AUDIO_ROUTE_HW_LAYOUT);
                    else assert(r.reason == DM_AUDIO_ROUTE_SURROUND);
                }
}

static void test_user_modes(void) {
    // Both AUTO and Prefer 5.1 can request surround; the hardware still vetoes
    // it when the layout is unknown. No preference bypasses that safety check.
    const dm_audio_mode modes[] = {
        DM_AUDIO_MODE_AUTO, DM_AUDIO_MODE_STEREO, DM_AUDIO_MODE_SURROUND
    };
    for (unsigned i = 0; i < sizeof modes / sizeof *modes; i++) {
        int enabled = modes[i] != DM_AUDIO_MODE_STEREO;
        dm_audio_route_decision compatible = dm_audio_route_decide(6, enabled, 6, 1);
        dm_audio_route_decision fake51 = dm_audio_route_decide(6, enabled, 6, 0);
        dm_audio_route_decision stereo = dm_audio_route_decide(6, enabled, 2, 0);
        assert(compatible.channels == (enabled ? 6u : 2u));
        assert(fake51.channels == 2);
        assert(stereo.channels == 2);
    }
}

int main(void) {
    test_full_matrix();
    test_user_modes();
    puts("Spatial Audio 2.0 route policy: exhaustive 1–8 guest / 0–12 hardware channel matrix PASS");
    puts("Mode policy: automatic, stereo and preferred 5.1 fall back safely PASS");
    return 0;
}
