#include <memory>
#include <cstdlib>
#include <cstdio>
#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>
#include "recomp.h"
#ifdef _WIN32
#include <windows.h>
#endif

// ROGUESQ_LOG_THREADS=1: thread create/start/destroy trace.
static bool log_thread_lifecycle() {
    static const int on = []() { const char* e = std::getenv("ROGUESQ_LOG_THREADS"); return (e && e[0] && e[0] != (char)48) ? 1 : 0; }();
    return on != 0;
}

extern "C" ULTRAMODERN_OVERRIDABLE void osInitialize_recomp(uint8_t * rdram, recomp_context * ctx) {
    osInitialize();
}

extern "C" ULTRAMODERN_OVERRIDABLE void __osInitialize_common_recomp(uint8_t * rdram, recomp_context * ctx) {
    osInitialize();
}

extern "C" void osCreateThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    if (log_thread_lifecycle()) {
        printf("[DEBUG] osCreateThread: t=0x%08X id=%d entry=0x%08X arg=0x%08X sp=0x%08X pri=%d\n",
            (uint32_t)ctx->r4, (int)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7,
            (uint32_t)MEM_W(0x10, ctx->r29), (int)MEM_W(0x14, ctx->r29)); fflush(stdout);
    }
    osCreateThread(rdram, (int32_t)ctx->r4, (OSId)ctx->r5, (int32_t)ctx->r6, (int32_t)ctx->r7,
        (int32_t)MEM_W(0x10, ctx->r29), (OSPri)MEM_W(0x14, ctx->r29));
    if (log_thread_lifecycle()) {
        printf("[DEBUG] osCreateThread done\n"); fflush(stdout);
    }
}

extern "C" ULTRAMODERN_OVERRIDABLE void osStartThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    OSThread* t = TO_PTR(OSThread, (int32_t)ctx->r4);
    if (log_thread_lifecycle()) {
        printf("[DEBUG] osStartThread: t=0x%08X id=%d pri=%d state=0x%04X\n",
            (uint32_t)ctx->r4, (int)t->id, (int)t->priority, (unsigned)t->state); fflush(stdout);
    }
    osStartThread(rdram, (int32_t)ctx->r4);
    if (log_thread_lifecycle()) {
        printf("[DEBUG] osStartThread done (id=%d)\n", (int)t->id); fflush(stdout);
    }
}

extern "C" ULTRAMODERN_OVERRIDABLE void osStopThread_recomp(uint8_t * rdram, recomp_context * ctx) {
    osStopThread(rdram, (int32_t)ctx->r4);
}

extern "C" ULTRAMODERN_OVERRIDABLE void osDestroyThread_recomp(uint8_t * rdram, recomp_context * ctx) {
    osDestroyThread(rdram, (int32_t)ctx->r4);
}

extern "C" ULTRAMODERN_OVERRIDABLE void osYieldThread_recomp(uint8_t * rdram, recomp_context * ctx) {
    // TODO: proper cooperative yield. ultramodern::check_running_queue(rdram)
    // crashed (null deref) at frame ~25. std::this_thread::yield() is a host-OS
    // yield, not a recomp thread swap, but at least it's stable.
    std::this_thread::yield();
}

extern "C" void osSetThreadPri_recomp(uint8_t* rdram, recomp_context* ctx) {
    osSetThreadPri(rdram, (int32_t)ctx->r4, (OSPri)ctx->r5);
}

extern "C" void osGetThreadPri_recomp(uint8_t * rdram, recomp_context * ctx) {
    ctx->r2 = osGetThreadPri(rdram, (int32_t)ctx->r4);
}

extern "C" void osGetThreadId_recomp(uint8_t * rdram, recomp_context * ctx) {
    ctx->r2 = osGetThreadId(rdram, (int32_t)ctx->r4);
}

extern "C" void osCreateMesgQueue_recomp(uint8_t* rdram, recomp_context* ctx) {
    osCreateMesgQueue(rdram, (int32_t)ctx->r4, (int32_t)ctx->r5, (s32)ctx->r6);
}

extern "C" ULTRAMODERN_OVERRIDABLE void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = osRecvMesg(rdram, (int32_t)ctx->r4, (int32_t)ctx->r5, (s32)ctx->r6);
}

extern "C" ULTRAMODERN_OVERRIDABLE void osSendMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t mq = (uint32_t)ctx->r4;
    // Log every send to the frame-thread's recv2 queue at 0x80128D10. Rate-limit
    // others so we can see who's sending what.
    if (mq == 0x80128D10) {
        if(false) fprintf(stderr, "[trace] osSendMesg->0x80128D10 msg=0x%016llX flags=%d\n",
            (unsigned long long)(uintptr_t)ctx->r5, (int)(int32_t)ctx->r6);
        fflush(stderr);
    } else {
        static int n=0; if (++n<=20 || (n%500)==0) {
            if(false) fprintf(stderr, "[trace] osSendMesg #%d mq=0x%08X msg=0x%016llX\n",
                n, mq, (unsigned long long)(uintptr_t)ctx->r5);
            fflush(stderr);
        }
    }
    ctx->r2 = osSendMesg(rdram, (int32_t)ctx->r4, (OSMesg)ctx->r5, (s32)ctx->r6);
}

extern "C" void osJamMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = osJamMesg(rdram, (int32_t)ctx->r4, (OSMesg)ctx->r5, (s32)ctx->r6);
}

extern "C" void osSetEventMesg_recomp(uint8_t* rdram, recomp_context* ctx) {
    osSetEventMesg(rdram, (OSEvent)ctx->r4, (int32_t)ctx->r5, (OSMesg)ctx->r6);
}

extern "C" void osViSetEvent_recomp(uint8_t * rdram, recomp_context * ctx) {
    osViSetEvent(rdram, (int32_t)ctx->r4, (OSMesg)ctx->r5, (u32)ctx->r6);
}

extern "C" void osGetCount_recomp(uint8_t * rdram, recomp_context * ctx) {
    ctx->r2 = osGetCount();
}

extern "C" void osSetCount_recomp(uint8_t * rdram, recomp_context * ctx) {
    osSetCount(ctx->r4);
}

extern "C" void osGetTime_recomp(uint8_t * rdram, recomp_context * ctx) {
    uint64_t total_count = osGetTime();
    ctx->r2 = (int32_t)(total_count >> 32);
    ctx->r3 = (int32_t)(total_count >> 0);
}

extern "C" void osSetTime_recomp(uint8_t * rdram, recomp_context * ctx) {
    uint64_t t = ((uint64_t)(ctx->r4) << 32) | ((ctx->r5) & 0xFFFFFFFFu);
    osSetTime(t);
}

extern "C" void osSetTimer_recomp(uint8_t * rdram, recomp_context * ctx) {
    uint64_t countdown = ((uint64_t)(ctx->r6) << 32) | ((ctx->r7) & 0xFFFFFFFFu);
    uint64_t interval = load_doubleword(rdram, ctx->r29, 0x10);
    ctx->r2 = osSetTimer(rdram, (int32_t)ctx->r4, countdown, interval, (int32_t)MEM_W(0x18, ctx->r29), (OSMesg)MEM_W(0x1C, ctx->r29));
}

extern "C" void osStopTimer_recomp(uint8_t * rdram, recomp_context * ctx) {
    ctx->r2 = osStopTimer(rdram, (int32_t)ctx->r4);
}

extern "C" void osVirtualToPhysical_recomp(uint8_t * rdram, recomp_context * ctx) {
    ctx->r2 = osVirtualToPhysical((int32_t)ctx->r4);
}

extern "C" void osInvalDCache_recomp(uint8_t * rdram, recomp_context * ctx) {
    ;
}

extern "C" void osInvalICache_recomp(uint8_t * rdram, recomp_context * ctx) {
    ;
}

extern "C" void osWritebackDCache_recomp(uint8_t * rdram, recomp_context * ctx) {
    ;
}

extern "C" void osWritebackDCacheAll_recomp(uint8_t * rdram, recomp_context * ctx) {
    ;
}

extern "C" void osSetIntMask_recomp(uint8_t * rdram, recomp_context * ctx) {
    ;
}

extern "C" void __osDisableInt_recomp(uint8_t * rdram, recomp_context * ctx) {
    ;
}

extern "C" void __osRestoreInt_recomp(uint8_t * rdram, recomp_context * ctx) {
    ;
}

extern "C" void __osSetFpcCsr_recomp(uint8_t * rdram, recomp_context * ctx) {
    ctx->r2 = 0;
}

// For the Mario Party games (not working)
//extern "C" void longjmp_recomp(uint8_t * rdram, recomp_context * ctx) {
//    RecompJmpBuf* buf = TO_PTR(RecompJmpBuf, ctx->r4);
//
//    // Check if this is a buffer that was set up with setjmp
//    if (buf->magic == SETJMP_MAGIC) {
//        // If so, longjmp to it
//        // Setjmp/longjmp does not work across threads, so verify that this buffer was made by this thread
//        assert(buf->owner == ultramodern::this_thread());
//        longjmp(buf->storage->buffer, ctx->r5);
//    } else {
//        // Otherwise, check if it was one built manually by the game with $ra pointing to a function
//        gpr sp = MEM_W(0, ctx->r4);
//        gpr ra = MEM_W(4, ctx->r4);
//        ctx->r29 = sp;
//        recomp_func_t* target = LOOKUP_FUNC(ra);
//        if (target == nullptr) {
//            fprintf(stderr, "Failed to find function for manual longjmp\n");
//            std::quick_exit(EXIT_FAILURE);
//        }
//        target(rdram, ctx);
//
//        // TODO kill this thread if the target function returns
//        assert(false);
//    }
//}
//
//#undef setjmp_recomp
//extern "C" void setjmp_recomp(uint8_t * rdram, recomp_context * ctx) {
//    fprintf(stderr, "Program called setjmp_recomp\n");
//    std::quick_exit(EXIT_FAILURE);
//}
//
//extern "C" int32_t osGetThreadEx(void) {
//    return ultramodern::this_thread();
//}
