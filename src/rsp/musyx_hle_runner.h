#pragma once

#include <cstdint>
#include <functional>

// Native (HLE) MusyX v1 synth from mupen64plus-rsp-hle, an alternative to the RSPRecomp'd ucode.

// True when ROGUESQ_AUDIO_HLE selects the native synth (default on for Vita, off elsewhere).
bool rs64_musyx_hle_enabled();

// Runs MusyX v1 on the task already staged in DMEM[0xFC0..0x1000], reading/writing RDRAM directly.
void rs64_musyx_hle_run(uint8_t* rdram, uint8_t* dmem);

// ROGUESQ_AUDIO_HLE_VERIFY=1: runs both synths on private copies of the same RDRAM/DMEM snapshot and logs where their
// outputs differ; the game itself still gets the recompiled synth on live RDRAM.
bool rs64_musyx_hle_verify_enabled();
void rs64_musyx_hle_verify(uint8_t* rdram, uint8_t* dmem, const std::function<void(uint8_t*)>& run_recompiled);
