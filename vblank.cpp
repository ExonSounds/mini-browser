// See vblank.h for why this exists.
//
// This file defines drmWaitVBlank(). Because WebKit's library looks the
// function up dynamically and the program's own definitions come first,
// WebKit's display-refresh monitor calls this version. The real libdrm
// function is still used whenever the driver supports it.

#include "vblank.h"

#include <dlfcn.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <utility>

namespace {

using RealWaitVBlank = int (*)(int, drmVBlankPtr);

RealWaitVBlank real_wait_vblank() {
    static RealWaitVBlank fn =
        reinterpret_cast<RealWaitVBlank>(dlsym(RTLD_NEXT, "drmWaitVBlank"));
    return fn;
}

// Latest known presentation time, from GTK (CLOCK_MONOTONIC, microseconds).
std::atomic<int64_t> g_phase_us{0};

struct Pacer {
    int64_t period_ns = 0;   // one refresh, from the display mode
    int64_t next_ns = 0;     // next tick we hand out
    uint32_t sequence = 0;   // fake vblank counter for the reply
};

std::mutex g_mutex;
std::map<std::pair<int, unsigned>, Pacer> g_pacers;  // (fd, crtc index)
std::map<std::pair<int, unsigned>, bool> g_emulate;  // driver said "unsupported"

int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// Which CRTC the request is for, decoded the same way libdrm encodes it.
unsigned crtc_index(unsigned type) {
    if (type & DRM_VBLANK_SECONDARY) return 1;
    return (type & DRM_VBLANK_HIGH_CRTC_MASK) >> DRM_VBLANK_HIGH_CRTC_SHIFT;
}

// Exact refresh period of that CRTC's current mode, e.g. 5555.5 µs at 180 Hz.
int64_t mode_period_ns(int fd, unsigned index) {
    int64_t period = 1000000000 / 60;  // fallback if the mode can't be read
    if (drmModeRes *res = drmModeGetResources(fd)) {
        if (index < unsigned(res->count_crtcs)) {
            if (drmModeCrtc *crtc = drmModeGetCrtc(fd, res->crtcs[index])) {
                const drmModeModeInfo &m = crtc->mode;
                if (crtc->mode_valid && m.clock && m.htotal && m.vtotal) {
                    // clock is in kHz: period = htotal * vtotal / (clock * 1000) s
                    double ns = double(m.htotal) * m.vtotal * 1e6 / m.clock;
                    if (m.flags & DRM_MODE_FLAG_INTERLACE) ns /= 2;
                    if (m.flags & DRM_MODE_FLAG_DBLSCAN) ns *= 2;
                    if (m.vscan > 1) ns *= m.vscan;
                    period = int64_t(std::llround(ns));
                }
                drmModeFreeCrtc(crtc);
            }
        }
        drmModeFreeResources(res);
    }
    return period;
}

// Sleep until the next refresh boundary and fill in the reply like the
// kernel would.
int emulated_wait(int fd, drmVBlankPtr vbl) {
    unsigned index = crtc_index(vbl->request.type);
    int64_t target;
    uint32_t sequence;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Pacer &p = g_pacers[{fd, index}];
        if (!p.period_ns) p.period_ns = mode_period_ns(fd, index);

        int64_t now = now_ns();
        // Line ticks up with the last presentation time GTK reported, so
        // WebKit starts each frame right after a real refresh.
        int64_t anchor = g_phase_us.load(std::memory_order_relaxed) * 1000;
        if (!anchor) anchor = p.next_ns ? p.next_ns : now;
        int64_t ahead = now - anchor;
        int64_t k = ahead >= 0 ? ahead / p.period_ns + 1 : 0;
        target = anchor + k * p.period_ns;
        // Never hand out the same tick twice or go backwards.
        if (p.next_ns && target < p.next_ns + p.period_ns / 2)
            target += p.period_ns;
        unsigned steps = (vbl->request.type & DRM_VBLANK_RELATIVE) &&
                                 vbl->request.sequence > 1
                             ? vbl->request.sequence
                             : 1;
        target += int64_t(steps - 1) * p.period_ns;
        p.next_ns = target;
        p.sequence += steps;
        sequence = p.sequence;
    }

    timespec ts{time_t(target / 1000000000), long(target % 1000000000)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }

    vbl->reply.type = vbl->request.type;
    vbl->reply.sequence = sequence;
    vbl->reply.tval_sec = long(target / 1000000000);
    vbl->reply.tval_usec = long((target % 1000000000) / 1000);
    return 0;
}

}  // namespace

void vblank_set_phase_hint(int64_t presentation_time_us) {
    if (presentation_time_us > 0)
        g_phase_us.store(presentation_time_us, std::memory_order_relaxed);
}

extern "C" int drmWaitVBlank(int fd, drmVBlankPtr vbl) {
    std::pair<int, unsigned> key{fd, crtc_index(vbl->request.type)};
    bool emulate;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        emulate = g_emulate[key];
    }
    if (!emulate) {
        RealWaitVBlank real = real_wait_vblank();
        drmVBlank copy = *vbl;
        int ret = real ? real(fd, vbl) : (errno = EOPNOTSUPP, -1);
        if (ret == 0 || errno != EOPNOTSUPP) return ret;
        // The driver can't wait for vblank; pace it ourselves from now on.
        *vbl = copy;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_emulate[key] = true;
    }
    return emulated_wait(fd, vbl);
}
