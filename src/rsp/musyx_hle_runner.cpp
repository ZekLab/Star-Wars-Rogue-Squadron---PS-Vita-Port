#include "musyx_hle_runner.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../main/debug_logs.h"

extern "C" {
#include "musyx_hle/hle_internal.h"
#include "musyx_hle/hle_external.h"
}

// Host callbacks the vendored HLE code expects (mupen64plus provides these in plugin.c).
extern "C" {
    void rsp_break(struct hle_t*, unsigned int) {}
    void HleVerboseMessage(void*, const char*, ...) {}
    void HleInfoMessage(void*, const char*, ...) {}

    void HleWarnMessage(void*, const char* message, ...) {
        static int s_n = 0;
        if (++s_n > 20) {
            return;
        }
        va_list args;
        va_start(args, message);
        fprintf(stderr, "[musyx-hle] warning: ");
        vfprintf(stderr, message, args);
        fprintf(stderr, "\n");
        va_end(args);
    }

    void HleErrorMessage(void*, const char* message, ...) {
        static int s_n = 0;
        if (++s_n > 20) {
            return;
        }
        va_list args;
        va_start(args, message);
        fprintf(stderr, "[musyx-hle] error: ");
        vfprintf(stderr, message, args);
        fprintf(stderr, "\n");
        va_end(args);
    }
}

bool rs64_musyx_hle_enabled() {
#ifdef __vita__
    static const bool on = recomp::dbg::env_on("ROGUESQ_AUDIO_HLE", true);
#else
    static const bool on = recomp::dbg::env_on("ROGUESQ_AUDIO_HLE", false);
#endif
    return on;
}

void rs64_musyx_hle_run(uint8_t* rdram, uint8_t* dmem) {
    // hle_t carries large scratch buffers for other ucodes; one instance serves the single audio task thread.
    static struct hle_t hle;
    hle.dram = rdram;
    hle.dmem = dmem;
    hle.imem = nullptr;
    musyx_v1_task(&hle);
}

bool rs64_musyx_hle_verify_enabled() {
    static const bool on = recomp::dbg::env_on("ROGUESQ_AUDIO_HLE_VERIFY");
    return on;
}

void rs64_musyx_hle_verify(uint8_t* rdram, uint8_t* dmem, const std::function<void(uint8_t*)>& run_recompiled) {
    constexpr size_t kRam = 0x800000;
    // The recompiled synth also reads the RDRAM mirror above 8 MB (terrain/F5 scratch), so copies cover the whole 16 MB window it may touch.
    constexpr size_t kCopy = 0x1000000;
    static std::vector<uint8_t> snap(kCopy), ram_lle(kCopy), ram_hle(kCopy), dmem_snap(0x1000);
    static unsigned long long s_tasks = 0, s_bad_tasks = 0, s_seen = 0;
    static unsigned long long s_hist[5] = {};
    static int s_max_delta = 0;

    // Checking every task slows the game too much to reach real music; check one in ROGUESQ_AUDIO_HLE_VERIFY_EVERY (default 8).
    static const int s_every = recomp::dbg::env_int("ROGUESQ_AUDIO_HLE_VERIFY_EVERY", 8);
    const bool check = (s_seen++ % (unsigned long long)(s_every > 0 ? s_every : 1)) == 0;
    if (check) {
        // Both synths start from the same private snapshot, so concurrent game-thread writes to live RDRAM can't leak into the diff.
        std::memcpy(snap.data(), rdram, kCopy);
        std::memcpy(dmem_snap.data(), dmem, 0x1000);

        std::memcpy(ram_lle.data(), snap.data(), kCopy);
        run_recompiled(ram_lle.data());

        std::memcpy(ram_hle.data(), snap.data(), kCopy);
        std::memcpy(dmem, dmem_snap.data(), 0x1000);
        rs64_musyx_hle_run(ram_hle.data(), dmem);

        std::memcpy(dmem, dmem_snap.data(), 0x1000);
    }

    // The game always gets the recompiled synth on live RDRAM.
    run_recompiled(rdram);
    if (!check) {
        return;
    }

    size_t diff = 0, first = 0, last = 0, lle_wrote = 0, hle_wrote = 0;
    for (size_t i = 0; i < kRam; ++i) {
        lle_wrote += ram_lle[i] != snap[i];
        hle_wrote += ram_hle[i] != snap[i];
        if (ram_lle[i] != ram_hle[i]) {
            if (diff == 0) {
                first = i;
            }
            last = i;
            ++diff;
        }
    }

    // Differences as signed 16-bit samples (the halfword at N64 address a is stored at a ^ 2): rounding noise vs real errors.
    int task_max = 0;
    for (size_t a = first & ~size_t(1); diff && a <= last && a + 1 < kRam; a += 2) {
        const size_t host = a ^ 2;
        int16_t s_lle, s_hle;
        std::memcpy(&s_lle, &ram_lle[host], 2);
        std::memcpy(&s_hle, &ram_hle[host], 2);
        const int d = s_lle > s_hle ? s_lle - s_hle : s_hle - s_lle;
        if (d == 0) {
            continue;
        }
        task_max = d > task_max ? d : task_max;
        s_hist[d == 1 ? 0 : d <= 4 ? 1 : d <= 16 ? 2 : d <= 256 ? 3 : 4]++;
    }
    s_max_delta = task_max > s_max_delta ? task_max : s_max_delta;

    ++s_tasks;
    s_bad_tasks += diff != 0;
    if (diff || (s_tasks % 64) == 0) {
        fprintf(stderr, "[musyx-verify] task %llu: recompiled wrote %u B, native wrote %u B, differ %u B (0x%08X..0x%08X) max|d16| %d | tasks differing %llu/%llu, all-time max %d, hist 1:%llu 2-4:%llu 5-16:%llu 17-256:%llu >256:%llu\n",
            s_tasks, unsigned(lle_wrote), unsigned(hle_wrote), unsigned(diff), unsigned(diff ? (0x80000000u | (first ^ 3)) : 0), unsigned(diff ? (0x80000000u | (last ^ 3)) : 0), task_max,
            s_bad_tasks, s_tasks, s_max_delta, s_hist[0], s_hist[1], s_hist[2], s_hist[3], s_hist[4]);
    }

    // ROGUESQ_AUDIO_HLE_VERIFY_DUMP=<dir>: once, for the first task with a large difference, write the images for offline analysis.
    static const char* s_dump_dir = recomp::dbg::env_str("ROGUESQ_AUDIO_HLE_VERIFY_DUMP");
    static bool s_dumped = false;
    if (s_dump_dir && !s_dumped && diff > 500) {
        s_dumped = true;
        const std::pair<const char*, const uint8_t*> images[] = { { "before", snap.data() }, { "lle", ram_lle.data() }, { "hle", ram_hle.data() } };
        for (const auto& [name, data] : images) {
            const std::string path = std::string(s_dump_dir) + "/musyx_" + name + ".bin";
            if (FILE* f = fopen(path.c_str(), "wb")) {
                fwrite(data, 1, kRam, f);
                fclose(f);
            }
        }
        const std::string dpath = std::string(s_dump_dir) + "/musyx_dmem.bin";
        if (FILE* f = fopen(dpath.c_str(), "wb")) {
            fwrite(dmem_snap.data(), 1, 0x1000, f);
            fclose(f);
        }
        fprintf(stderr, "[musyx-verify] dumped task %llu images to %s\n", s_tasks, s_dump_dir);
    }
}
