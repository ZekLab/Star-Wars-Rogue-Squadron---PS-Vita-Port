#include "vg_prof.h"

#include <chrono>
#include <cstring>

#ifdef __vita__
#   include <psp2/kernel/processmgr.h>
#   include <psp2/kernel/threadmgr.h>
#endif

namespace vg {
    const char *const g_profNames[PROF_COUNT] = {
        "tmem load", "setVertex", "drawIndexedTri", "batch state", "submit draw", "texture hash", "texture decode", "rsp reset"
    };
    bool g_profEnabled = false;
    uint64_t g_profTicks[PROF_COUNT] = {};
    uint32_t g_profCalls[PROF_COUNT] = {};
    bool g_opProfEnabled = false;
    uint64_t g_opTicks[256] = {};
    uint32_t g_opCalls[256] = {};

#ifdef __vita__
    static double s_ticksPerUs = 1.0;
#else
    static double s_ticksPerUs = 1000.0;
#endif

#ifdef __vita__
    // ARMv7 PMU: PMUSERENR is always readable at PL0; its EN bit says whether the other PMU registers are too.
    static inline uint32_t readPmuserenr() {
        uint32_t v;
        asm volatile("mrc p15, 0, %0, c9, c14, 0" : "=r"(v));
        return v;
    }

    static inline uint32_t readPmccntr() {
        uint32_t v;
        asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
        return v;
    }

    static bool s_useCycles = false;
    // The cycle counter is 32 bits (under 10 s at 444 MHz); extended here. Only the profiled threads read it.
    static uint32_t s_cyclesLast = 0;
    static uint64_t s_cyclesHigh = 0;

    void profInit() {
        s_useCycles = false;
        s_ticksPerUs = 1.0;
        if ((readPmuserenr() & 1) == 0) {
            return;
        }
        // Enabled and counting? Calibrate against the microsecond clock over ~20 ms.
        const uint32_t c0 = readPmccntr();
        const SceUInt64 t0 = sceKernelGetProcessTimeWide();
        sceKernelDelayThread(20000);
        const uint32_t c1 = readPmccntr();
        const SceUInt64 t1 = sceKernelGetProcessTimeWide();
        if ((c1 == c0) || (t1 <= t0)) {
            return;
        }
        s_ticksPerUs = double(uint32_t(c1 - c0)) / double(t1 - t0);
        s_useCycles = true;
        s_cyclesLast = readPmccntr();
        s_cyclesHigh = 0;
    }

    uint64_t profNow() {
        if (s_useCycles) {
            const uint32_t c = readPmccntr();
            if (c < s_cyclesLast) {
                s_cyclesHigh += uint64_t(1) << 32;
            }
            s_cyclesLast = c;
            return s_cyclesHigh | c;
        }
        return sceKernelGetProcessTimeWide();
    }

    const char *profClockName() {
        return s_useCycles ? "PMU cycle counter" : "sceKernelGetProcessTimeWide";
    }
#else
    void profInit() { }

    uint64_t profNow() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    const char *profClockName() {
        return "steady_clock";
    }
#endif

    double profTicksToUs(double ticks) {
        return ticks / s_ticksPerUs;
    }

    void profReset() {
        memset(g_profTicks, 0, sizeof(g_profTicks));
        memset(g_profCalls, 0, sizeof(g_profCalls));
        memset(g_opTicks, 0, sizeof(g_opTicks));
        memset(g_opCalls, 0, sizeof(g_opCalls));
    }
}
