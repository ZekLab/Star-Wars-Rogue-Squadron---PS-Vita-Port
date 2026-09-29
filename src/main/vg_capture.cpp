#include "vg_capture.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/stat.h>

#include "debug_logs.h"
#include "game_state.h"

extern "C" std::atomic<unsigned> g_rs64_swap_count;  // upstream_compat.cpp
extern "C" volatile int g_current_scene;              // hook_helpers.cpp

namespace {
    // Covers the 8 MB game RDRAM plus the host-side tables the F5 renderer reads above it (up to 0x80B20000).
    constexpr uint32_t kCaptureBytes = 0xB20000;
    constexpr uint32_t kMagic = 0x32434756;  // "VGC2"

    int s_target = -2;
    int s_tasks = 0;
    bool s_done = false;
    std::string s_state;
    int s_state_delay = 0;

    // "<swap>" captures after that swap; "state:<id>+<n>" captures n swaps after the classifier first reports <id>.
    int target() {
        if (s_target == -2) {
            const char* v = recomp::dbg::env_str("ROGUESQ_VG_CAPTURE");
            s_target = -1;
            if ((v != nullptr) && (strncmp(v, "state:", 6) == 0)) {
                const std::string spec = v + 6;
                const size_t plus = spec.find('+');
                s_state = spec.substr(0, plus);
                s_state_delay = (plus == std::string::npos) ? 0 : std::atoi(spec.c_str() + plus + 1);
                s_target = -3;
            }
            else if (v != nullptr) {
                s_target = std::atoi(v);
            }
        }
        if ((s_target == -3) && (s_state == rs64_state_current_id())) {
            s_target = int(g_rs64_swap_count.load(std::memory_order_relaxed)) + s_state_delay;
            fprintf(stderr, "[vg-capture] state %s reached, capturing after swap %d\n", s_state.c_str(), s_target);
        }
        return s_target;
    }

    std::string dir() {
        const char* v = recomp::dbg::env_str("ROGUESQ_VG_CAPTURE_DIR");
        return v ? v : "vg_capture";
    }
}

extern "C" void rs64_vg_capture_task(const uint8_t* rdram, uint32_t ucode, uint32_t ucode_data, uint32_t data_ptr) {
    if (s_done || (target() < 0) || (int(g_rs64_swap_count.load(std::memory_order_relaxed)) != s_target)) {
        return;
    }

    const std::string path = dir();
    mkdir(path.c_str(), 0777);
    char name[64];
    snprintf(name, sizeof(name), "/task_%02d.bin", s_tasks);
    FILE* f = fopen((path + name).c_str(), "wb");
    if (f == nullptr) {
        fprintf(stderr, "[vg-capture] cannot write %s%s\n", path.c_str(), name);
        s_done = true;
        return;
    }
    const VgCaptureHeader header = { kMagic, ucode, ucode_data, data_ptr, kCaptureBytes, g_current_scene };
    fwrite(&header, sizeof(header), 1, f);
    fwrite(rdram, 1, kCaptureBytes, f);
    fclose(f);
    fprintf(stderr, "[vg-capture] task %d (dl 0x%06X) written\n", s_tasks, data_ptr);
    s_tasks++;
}

extern "C" void rs64_vg_capture_vi(uint32_t vi_origin) {
    if (s_done || (target() < 0) || (s_tasks == 0) || (int(g_rs64_swap_count.load(std::memory_order_relaxed)) <= s_target)) {
        return;
    }

    FILE* f = fopen((dir() + "/vi.txt").c_str(), "w");
    if (f != nullptr) {
        fprintf(f, "0x%08X %d\n", vi_origin, s_tasks);
        fclose(f);
    }
    fprintf(stderr, "[vg-capture] done: %d tasks, VI origin 0x%08X\n", s_tasks, vi_origin);
    s_done = true;
}
