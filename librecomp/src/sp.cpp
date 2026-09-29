#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ultramodern/ultramodern.hpp>
#include "recomp.h"

extern "C" void osSpTaskLoad_recomp(uint8_t* rdram, recomp_context* ctx) {
    // Nothing to do here
}

bool dump_frame = false;

extern "C" ULTRAMODERN_OVERRIDABLE void osSpTaskStartGo_recomp(uint8_t* rdram, recomp_context* ctx) {
    OSTask* task = TO_PTR(OSTask, ctx->r4);
    {
        // SP task dispatch trace. Useful for tracking task scheduling
        // rate (cinematic ~30/s, normal play 1-2/frame). Default off.
        // ROGUESQ_LOG_SP_TASKS=1 (or ROGUESQ_LOG_ALL=1).
        static const bool log_sp = []{
            const char *a = std::getenv("ROGUESQ_LOG_ALL");
            if (a && *a && *a != '0') return true;
            const char *e = std::getenv("ROGUESQ_LOG_SP_TASKS");
            return e && *e && *e != '0';
        }();
        static int n = 0;
        ++n;
        if (log_sp && (n <= 8 || (n % 60) == 0)) {
            const char* kind = (task->t.type == M_GFXTASK) ? "GFX"
                             : (task->t.type == M_AUDTASK) ? "AUD" : "OTHER";
            // ctx->r31 is $ra - the return address of the caller. Tells us which game-side
            // function called osSpTaskStartGo.
            fprintf(stderr, "[sp] osSpTaskStartGo #%d kind=%s task=0x%08X data=0x%08X len=%u ra=0x%08X\n",
                n, kind, (uint32_t)ctx->r4, (uint32_t)task->t.data_ptr,
                (unsigned)task->t.data_size, (uint32_t)ctx->r31);
            fflush(stderr);
        }
    }
    // For debugging
    if (dump_frame) {
        char addr_str[32];
        constexpr size_t ram_size = 0x800000;
        std::unique_ptr<char[]> ram_unswapped = std::make_unique<char[]>(ram_size);
        snprintf(addr_str, sizeof(addr_str) - 1, "%08X", task->t.data_ptr);
        addr_str[sizeof(addr_str) - 1] = '\0';
        std::ofstream dump_file{ "ramdump" + std::string{ addr_str } + ".bin", std::ios::binary};

        for (size_t i = 0; i < ram_size; i++) {
            ram_unswapped[i] = rdram[i ^ 3];
        }

        dump_file.write(ram_unswapped.get(), ram_size);
        dump_frame = false;
    }
    ultramodern::submit_rsp_task(rdram, ctx->r4);
}

extern "C" void osSpTaskYield_recomp(uint8_t* rdram, recomp_context* ctx) {
    // Ignore yield requests (acts as if the task completed before it received the yield request)
    static int n = 0;
    ++n;
    if (n <= 8 || (n & 31) == 0) {
        fprintf(stderr, "[sp] osSpTaskYield #%d ra=0x%08X\n",
            n, (uint32_t)ctx->r31);
        fflush(stderr);
    }
}

extern "C" void osSpTaskYielded_recomp(uint8_t* rdram, recomp_context* ctx) {
    // Task yield requests are ignored, so always return 0 as tasks will never be yielded
    static int n = 0;
    ++n;
    if (n <= 8 || (n & 31) == 0) {
        fprintf(stderr, "[sp] osSpTaskYielded #%d ra=0x%08X\n",
            n, (uint32_t)ctx->r31);
        fflush(stderr);
    }
    ctx->r2 = 0;
}

extern "C" void __osSpSetPc_recomp(uint8_t* rdram, recomp_context* ctx) {
    assert(false);
}
