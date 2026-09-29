#include <ultramodern/ultramodern.hpp>
#include "recomp.h"
#include "helpers.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

extern "C" ULTRAMODERN_OVERRIDABLE void osViSetYScale_recomp(uint8_t* rdram, recomp_context * ctx) {
    osViSetYScale(ctx->f12.fl);
}

extern "C" ULTRAMODERN_OVERRIDABLE void osViSetXScale_recomp(uint8_t* rdram, recomp_context * ctx) {
    osViSetXScale(ctx->f12.fl);
}

extern "C" void osCreateViManager_recomp(uint8_t* rdram, recomp_context* ctx) {
    ;
}

extern "C" ULTRAMODERN_OVERRIDABLE void osViBlack_recomp(uint8_t* rdram, recomp_context* ctx) {
    osViBlack((uint32_t)ctx->r4);
}

extern "C" void osViRepeatLine_recomp(uint8_t* rdram, recomp_context* ctx) {
    osViRepeatLine(_arg<0, u8>(rdram, ctx));
}

extern "C" void osViSetSpecialFeatures_recomp(uint8_t* rdram, recomp_context* ctx) {
    osViSetSpecialFeatures((uint32_t)ctx->r4);
}

extern "C" void osViGetCurrentFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)(int32_t)osViGetCurrentFramebuffer();
}

extern "C" void osViGetNextFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)(int32_t)osViGetNextFramebuffer();
}

extern "C" ULTRAMODERN_OVERRIDABLE void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    osViSwapBuffer(rdram, (int32_t)ctx->r4);
}

extern "C" ULTRAMODERN_OVERRIDABLE void osViSetMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    osViSetMode(rdram, (int32_t)ctx->r4);
}

extern uint64_t total_vis;

extern "C" ULTRAMODERN_OVERRIDABLE void osViGetCurrentField_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0; // always field 0 (progressive / non-interlaced)
}

extern "C" void wait_one_frame(uint8_t* rdram, recomp_context* ctx) {
    uint64_t cur_vis = total_vis;
    while (cur_vis == total_vis) {
        std::this_thread::yield();
    }
}
