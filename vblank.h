// Refresh-rate pacing for WebKit on drivers without DRM vblank waits.
//
// WebKit paces page rendering by calling libdrm's drmWaitVBlank() on the
// display's DRM device. Some drivers (e.g. NVIDIA's) answer that call with
// EOPNOTSUPP, and WebKit then falls back to a fixed ~60 fps timer whatever
// the monitor's refresh rate is. vblank.cpp provides its own drmWaitVBlank()
// that WebKit picks up instead: it uses the real call when the driver
// supports it, and otherwise waits for the next refresh itself, at the
// display mode's exact rate and lined up with the compositor's real
// presentation times.
#pragma once

#include <cstdint>

// Tell the pacer when a frame was (or will be) shown, in g_get_monotonic_time()
// microseconds, so its ticks stay in phase with the display's real refreshes.
void vblank_set_phase_hint(int64_t presentation_time_us);
