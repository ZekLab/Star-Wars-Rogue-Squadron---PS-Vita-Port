// Per-section CPU timers for the Vita renderer, reported by the render context every 5 seconds and by the parser bench
// (vita_bench.cpp). Off unless g_profEnabled (ROGUESQ_VG_PROF=1 in the game): every clock read costs time on the Vita.

#pragma once

#include <cstdint>

namespace vg {
    // Sections nest (a draw submit runs inside the triangle that flushed the batch), so times are inclusive.
    enum ProfSection {
        PROF_TMEM_LOAD,
        PROF_SET_VERTEX,
        PROF_DRAW_TRI,
        PROF_BATCH_STATE,
        PROF_SUBMIT_DRAW,
        PROF_TEX_HASH,
        PROF_TEX_DECODE,
        PROF_RSP_RESET,
        PROF_COUNT
    };

    extern const char *const g_profNames[PROF_COUNT];
    extern bool g_profEnabled;
    extern uint64_t g_profTicks[PROF_COUNT];
    extern uint32_t g_profCalls[PROF_COUNT];

    // Inclusive time per display-list opcode, measured in the interpreter loop when g_opProfEnabled.
    extern bool g_opProfEnabled;
    extern uint64_t g_opTicks[256];
    extern uint32_t g_opCalls[256];

    // Picks the clock: the CPU cycle counter when user mode may read it (Vita), else the OS microsecond clock.
    void profInit();
    uint64_t profNow();
    double profTicksToUs(double ticks);
    const char *profClockName();
    void profReset();

    struct ProfScope {
        ProfSection section;
        uint64_t start;

        explicit ProfScope(ProfSection section) : section(section), start(g_profEnabled ? profNow() : 0) { }

        ~ProfScope() {
            if (g_profEnabled) {
                g_profTicks[section] += profNow() - start;
                g_profCalls[section]++;
            }
        }
    };
}
