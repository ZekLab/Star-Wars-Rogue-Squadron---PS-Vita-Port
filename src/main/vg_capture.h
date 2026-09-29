#pragma once

#include <cstdint>

// Frame capture for the Vita renderer's offline replay (tools/vita/vg_replay). ROGUESQ_VG_CAPTURE=<swap count> writes the
// RDRAM snapshot and task of every graphics task drawn after that swap, plus the VI origin that shows them, into
// ROGUESQ_VG_CAPTURE_DIR (default vg_capture). The captured files contain game memory and must stay out of the repo.

struct VgCaptureHeader {
    uint32_t magic;
    uint32_t ucode;
    uint32_t ucodeData;
    uint32_t dataPtr;
    uint32_t rdramBytes;
    // The only host global the F5 parser reads (hook_helpers.cpp).
    int32_t currentScene;
};

extern "C" {
    void rs64_vg_capture_task(const uint8_t* rdram, uint32_t ucode, uint32_t ucode_data, uint32_t data_ptr);
    void rs64_vg_capture_vi(uint32_t vi_origin);
}
