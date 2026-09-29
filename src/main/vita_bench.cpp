// Parser bench on the console: replays a frame captured with ROGUESQ_VG_CAPTURE through the Vita renderer's display-list
// parser without drawing, and logs clock costs, time per pass, per section and per opcode.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/dmac.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "debug_logs.h"
#include "vg_capture.h"
#include "vita_host.h"

#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"
#include "vg_backend_gl.h"
#include "vg_prof.h"
#include "vg_renderer.h"
#include "vg_submit.h"
#include "vg_texture.h"

extern "C" volatile int g_current_scene;

namespace {
    template <typename F>
    void benchClock(const char *name, F read) {
        constexpr int Iterations = 20000;
        volatile uint64_t sink = 0;
        const SceUInt64 t0 = sceKernelGetProcessTimeWide();
        for (int i = 0; i < Iterations; i++) {
            sink = sink + uint64_t(read());
        }
        const SceUInt64 t1 = sceKernelGetProcessTimeWide();
        fprintf(stderr, "[vg-bench] clock %s: %.3f us per read\n", name, double(t1 - t0) / Iterations);
    }

    struct PassStats {
        uint32_t tris = 0, rects = 0, batches = 0, hashes = 0, decodes = 0, vertices = 0;
    };
}

namespace rs64::vita {
    void run_parser_bench() {
        const char *dirName = recomp::dbg::env_str("ROGUESQ_VG_BENCH");
        if (dirName == nullptr) {
            return;
        }
        const std::string dir = (data_dir() / dirName).string();
        const int passes = std::max(1, recomp::dbg::env_int("ROGUESQ_VG_BENCH_PASSES", 30));
        fprintf(stderr, "[vg-bench] capture %s, %d passes per mode\n", dir.c_str(), passes);

        uint32_t viOrigin = 0;
        int taskCount = 0;
        {
            FILE *f = fopen((dir + "/vi.txt").c_str(), "r");
            if ((f == nullptr) || (fscanf(f, "%x %d", &viOrigin, &taskCount) != 2) || (taskCount < 1)) {
                fprintf(stderr, "[vg-bench] missing or bad %s/vi.txt\n", dir.c_str());
                sync_log();
                return;
            }
            fclose(f);
        }

        std::vector<uint8_t> rdram(32 * 1024 * 1024, 0);
        std::vector<VgCaptureHeader> headers(taskCount);
        std::vector<std::vector<uint8_t>> snapshots(taskCount);
        for (int i = 0; i < taskCount; i++) {
            char name[64];
            snprintf(name, sizeof(name), "/task_%02d.bin", i);
            FILE *f = fopen((dir + name).c_str(), "rb");
            VgCaptureHeader &header = headers[i];
            if ((f == nullptr) || (fread(&header, sizeof(header), 1, f) != 1) || (header.magic != 0x32434756) || (header.rdramBytes > rdram.size())) {
                fprintf(stderr, "[vg-bench] bad task file %s\n", name);
                if (f != nullptr) {
                    fclose(f);
                }
                sync_log();
                return;
            }
            snapshots[i].resize(header.rdramBytes);
            const size_t got = fread(snapshots[i].data(), 1, header.rdramBytes, f);
            fclose(f);
            if (got != header.rdramBytes) {
                fprintf(stderr, "[vg-bench] short task file %s\n", name);
                sync_log();
                return;
            }
        }

        // One core, so the per-core cycle counter stays consistent.
        const int oldAffinity = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
        sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
        sceKernelDelayThread(1000);
        vg::profInit();
        fprintf(stderr, "[vg-bench] profiler clock: %s, %.1f ticks/us\n", vg::profClockName(), 1.0 / vg::profTicksToUs(1.0));
        benchClock("std::chrono::steady_clock", []() { return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); });
        benchClock("sceKernelGetProcessTimeWide", []() { return uint64_t(sceKernelGetProcessTimeWide()); });
        benchClock("sceKernelGetProcessTimeLow", []() { return uint64_t(sceKernelGetProcessTimeLow()); });
        benchClock("sceKernelGetSystemTimeWide", []() { return uint64_t(sceKernelGetSystemTimeWide()); });
        benchClock("vg::profNow", []() { return vg::profNow(); });
        sync_log();

        vg::Backend backend;
        vg::Submitter submitter(&backend, false);
        submitter.dryRun = true;
        vg::Renderer renderer(&submitter);
        RT64::State state(rdram.data(), &renderer);
        RT64::Interpreter interpreter;
        interpreter.setup(&state);

        PassStats stats;
        auto runPass = [&](bool restore) {
            renderer.statTris = renderer.statRects = renderer.statBatches = 0;
            submitter.statHashes = submitter.statDecodes = 0;
            for (int i = 0; i < taskCount; i++) {
                const VgCaptureHeader &header = headers[i];
                if (restore || (taskCount > 1)) {
                    memcpy(rdram.data(), snapshots[i].data(), header.rdramBytes);
                }
                g_current_scene = header.currentScene;
                state.rsp->reset();
                interpreter.loadUCodeGBI(header.ucode, header.ucodeData, true);
                renderer.beginTask(&state);
                interpreter.processDisplayLists(header.dataPtr, reinterpret_cast<RT64::DisplayList *>(rdram.data() + header.dataPtr));
                renderer.endTask();
            }
            stats.tris = renderer.statTris;
            stats.rects = renderer.statRects;
            stats.batches = renderer.statBatches;
            stats.hashes = submitter.statHashes;
            stats.decodes = submitter.statDecodes;
            stats.vertices = renderer.vertexCount();
        };

        // Warm-up: the first pass decodes every texture and fills the parser's caches (the capture is copied in before the clock starts).
        for (int i = 0; i < taskCount; i++) {
            memcpy(rdram.data(), snapshots[i].data(), headers[i].rdramBytes);
        }
        const SceUInt64 w0 = sceKernelGetProcessTimeWide();
        runPass(false);
        const SceUInt64 w1 = sceKernelGetProcessTimeWide();
        fprintf(stderr, "[vg-bench] first pass %.2f ms: tris %u rects %u batches %u vertices %u hashes %u decodes %u\n",
            double(w1 - w0) / 1000.0, stats.tris, stats.rects, stats.batches, stats.vertices, stats.hashes, stats.decodes);
        runPass(false);
        runPass(false);

        // Cost of the per-task parse snapshot (events.cpp take_rdram_snapshot: 8 MB into a 12 MB buffer), by copy routine.
        {
            std::vector<uint8_t> snap(0xC00000, 0);
            constexpr int Copies = 4;
            auto timeCopy = [&](const char *name, auto copy) {
                const SceUInt64 t0 = sceKernelGetProcessTimeWide();
                for (int i = 0; i < Copies; i++) {
                    copy(snap.data(), rdram.data(), size_t(0x800000));
                }
                fprintf(stderr, "[vg-bench] 8 MB copy with %s: %.2f ms\n", name, double(sceKernelGetProcessTimeWide() - t0) / 1000.0 / Copies);
                sync_log();
            };
            timeCopy("memcpy", [](void *d, const void *s, size_t n) { memcpy(d, s, n); });
            timeCopy("sceClibMemcpy", [](void *d, const void *s, size_t n) { sceClibMemcpy(d, s, n); });
            timeCopy("sceDmacMemcpy", [](void *d, const void *s, size_t n) { sceDmacMemcpy(d, s, n); });
        }

        auto timeMode = [&](const char *mode) {
            std::vector<double> ms;
            for (int p = 0; p < passes; p++) {
                const SceUInt64 t0 = sceKernelGetProcessTimeWide();
                runPass(false);
                ms.push_back(double(sceKernelGetProcessTimeWide() - t0) / 1000.0);
            }
            std::sort(ms.begin(), ms.end());
            double sum = 0;
            for (double v : ms) {
                sum += v;
            }
            fprintf(stderr, "[vg-bench] %s: min %.2f ms, median %.2f ms, mean %.2f ms, max %.2f ms (tris %u rects %u batches %u vertices %u hashes %u decodes %u)\n",
                mode, ms.front(), ms[ms.size() / 2], sum / ms.size(), ms.back(), stats.tris, stats.rects, stats.batches, stats.vertices, stats.hashes, stats.decodes);
            sync_log();
        };

        vg::g_profEnabled = false;
        vg::g_opProfEnabled = false;
        vg::g_hashBytes = 0;
        vg::g_hashUpdates = 0;
        timeMode("no timers");
        fprintf(stderr, "[vg-bench] texture hash input: %.0f bytes/pass in %.0f updates/pass\n",
            double(vg::g_hashBytes) / passes, double(vg::g_hashUpdates) / passes);

        vg::profReset();
        vg::g_profEnabled = true;
        timeMode("section timers");
        vg::g_profEnabled = false;
        for (int i = 0; i < vg::PROF_COUNT; i++) {
            fprintf(stderr, "[vg-bench]   section %-15s %8.3f ms/pass %7.1f calls/pass %7.2f us/call\n", vg::g_profNames[i],
                vg::profTicksToUs(double(vg::g_profTicks[i])) / passes / 1000.0, double(vg::g_profCalls[i]) / passes,
                vg::g_profCalls[i] ? vg::profTicksToUs(double(vg::g_profTicks[i])) / vg::g_profCalls[i] : 0.0);
        }

        vg::profReset();
        vg::g_opProfEnabled = true;
        timeMode("opcode timers");
        vg::g_opProfEnabled = false;
        std::vector<int> ops;
        for (int op = 0; op < 256; op++) {
            if (vg::g_opCalls[op] != 0) {
                ops.push_back(op);
            }
        }
        std::sort(ops.begin(), ops.end(), [](int a, int b) { return vg::g_opTicks[a] > vg::g_opTicks[b]; });
        for (int op : ops) {
            fprintf(stderr, "[vg-bench]   op 0x%02X %8.3f ms/pass %7.1f calls/pass %7.2f us/call\n", op,
                vg::profTicksToUs(double(vg::g_opTicks[op])) / passes / 1000.0, double(vg::g_opCalls[op]) / passes,
                vg::profTicksToUs(double(vg::g_opTicks[op])) / vg::g_opCalls[op]);
        }
        vg::profReset();
        sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), oldAffinity);
        fprintf(stderr, "[vg-bench] done\n");
        sync_log();
    }
}
