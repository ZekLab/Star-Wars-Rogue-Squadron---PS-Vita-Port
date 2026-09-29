#include <thread>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <variant>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <deque>
#include <cstring>
#include <cstdlib>
#include "blockingconcurrentqueue.h"
extern "C" volatile long long g_rs64_pdl_us;
extern "C" volatile unsigned g_f5_task_hops, g_f5_task_faces;   // rt64 F5 GBI diagnostics
// RogueSquadron64Recomp (2026-09-08): on hardware the RSP finishes a graphics task well before the next
// retrace, so the game never sees a VI while its list is still being read. RT64's parse takes 10-70 ms,
// during which the game (woken by the VI) releases and reuses the list's chunks under the parser. Hold the
// VI event while a graphics parse is in flight (bounded). ROGUESQ_VI_WAIT_PARSE=0 disables.
static std::atomic<int> g_gfx_parse_inflight{0};
static std::mutex g_gfx_parse_mutex;
static std::condition_variable g_gfx_parse_cv;
static bool parse_snapshot_enabled();
static bool vi_wait_parse_enabled() {
    // Default: on only when the parse does NOT run from a snapshot (ROGUESQ_VI_WAIT_PARSE=1/0 overrides).
    static const bool on = [](){ const char* e = std::getenv("ROGUESQ_VI_WAIT_PARSE"); if (e && e[0]) return e[0] != '0'; return !parse_snapshot_enabled(); }();
    return on;
}
static bool gfx_sync_submit_enabled();
extern moodycamel::LightweightSemaphore g_gfx_task_parsed;
// Called from the game's chunk-release functions (hand edits in funcs_3.c): a release while RT64 is still
// reading the list rewrites chunk headers under the parser. Hardware never gets here (the RSP is done in
// a few ms); wait for the in-flight parse instead. Bounded. ROGUESQ_VI_WAIT_PARSE=0 disables.
// Count a graphics task as in flight from the moment the game submits it (osSpTaskStartGo), not from
// when the gfx thread dequeues it: the game keeps running on its own host thread in between.
extern "C" volatile unsigned g_rs64_gfx_requests = 0;
extern "C" void rs64_gfx_task_submitted(void) {
    ++g_rs64_gfx_requests;
    g_gfx_parse_inflight.fetch_add(1, std::memory_order_acq_rel);
}
extern "C" void rs64_wait_gfx_parse(void) {
    if (!vi_wait_parse_enabled() || !g_gfx_parse_inflight.load(std::memory_order_acquire)) return;
    std::unique_lock<std::mutex> plock{ g_gfx_parse_mutex };
    g_gfx_parse_cv.wait_for(plock, std::chrono::milliseconds(500), []{ return g_gfx_parse_inflight.load(std::memory_order_acquire) == 0; });
}
// Slice of the same wait (at most `ms`); returns nonzero while a parse is still in flight. The N64-side
// waiter yields between slices so the retrace thread (audio synth) keeps running during a long parse.
extern "C" int rs64_gfx_parse_inflight_wait(int ms) {
    if (!vi_wait_parse_enabled() || !g_gfx_parse_inflight.load(std::memory_order_acquire)) return 0;
    std::unique_lock<std::mutex> plock{ g_gfx_parse_mutex };
    g_gfx_parse_cv.wait_for(plock, std::chrono::milliseconds(ms), []{ return g_gfx_parse_inflight.load(std::memory_order_acquire) == 0; });
    return g_gfx_parse_inflight.load(std::memory_order_acquire) != 0;
}

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/extensions.h"

#include "ultramodern/rsp.hpp"
#include "ultramodern/renderer_context.hpp"

static ultramodern::events::callbacks_t events_callbacks{};

void ultramodern::events::set_callbacks(const ultramodern::events::callbacks_t& callbacks) {
    events_callbacks = callbacks;
}

struct SpTaskAction {
    OSTask task;
    uint8_t* snapshot = nullptr;   // RogueSquadron64Recomp: RDRAM copy taken at task start (parse input)
};
// RogueSquadron64Recomp (2026-09-08): the RSP consumes a display list within a few ms of task start, before
// the game recycles its chunks; RT64's parse takes 7-100 ms (GPU sync per task) and was reading chunks the
// game had already rebuilt. Parse from a copy of RDRAM taken at task start instead (ROGUESQ_PARSE_SNAPSHOT=0
// disables). Three 8 MB buffers: at most one task parsing + one queued.
static bool parse_snapshot_enabled() {
    static const bool on = [](){ const char* e = std::getenv("ROGUESQ_PARSE_SNAPSHOT"); return !(e && e[0] == '0'); }();
    return on;
}
// ROGUESQ_LOG_FRAME_PROFILE: last snapshot memcpy duration (us), sampled by the hitch report.
extern "C" volatile long long g_rs64_snap_us; volatile long long g_rs64_snap_us = 0;
static uint8_t* take_rdram_snapshot(const uint8_t* rdram) {
    const auto snap0 = std::chrono::high_resolution_clock::now();
    // RT64's F5 GBI writes scratch (vertices @0xA00000, viewport @0xA01000) ABOVE the 8 MB game RAM, and
    // reads/writes it through state->RDRAM (= this buffer during the parse). An 8 MB buffer sends those
    // writes ~2 MB out of bounds -> heap corruption / AVs in f5_set_viewport (the in-mission instability).
    // Size the buffer past the scratch region. RogueSquadron64Recomp (2026-09-09).
    static constexpr size_t kSnapSize = 0xC00000;   // 12 MB: 8 MB game RAM + F5 scratch at 0xA0xxxx
    static uint8_t* bufs[3] = { nullptr, nullptr, nullptr };
    static unsigned next = 0;
    uint8_t*& b = bufs[next++ % 3];
    if (!b) { b = static_cast<uint8_t*>(std::malloc(kSnapSize)); if (b) std::memset(b, 0, kSnapSize); }
    if (b) std::memcpy(b, rdram, 0x800000);
    g_rs64_snap_us = (long long)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - snap0).count();
    return b;
}
extern "C" uint8_t* g_rs64_parse_rdram = nullptr;   // read by send_dl: parse input when non-null
extern "C" unsigned long long rs64_cine_iter_get(void);   // host cinematic-loop iteration (matches ROGUESQ_DUMP_RDRAM_ON_CINE_ITER / PJ64 cine_frame<n> goldens)
extern "C" volatile int g_active_overlay;                 // 0=mission/gameplay, 1=menu, 2=cinematic, -1=none (rs64_load_overlay)

struct ScreenUpdateAction {
    ultramodern::renderer::ViRegs regs;
};

struct UpdateConfigAction {
};

struct RdpRangeAction {
    uint32_t lo_phys;
    uint32_t hi_phys;
};

// Batched RDP-range submissions. Factor5's LLE recompile emits ~1M tiny
// ranges/sec; per-range enqueues flood the action_queue. The bridge
// accumulates ~512 ranges then enqueues them all as one batch action.
struct RdpRangeBatchAction {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
};

using Action = std::variant<SpTaskAction, ScreenUpdateAction, UpdateConfigAction, RdpRangeAction, RdpRangeBatchAction>;

struct ViState {
    const OSViMode* mode;
    PTR(void) framebuffer;
    PTR(OSMesg) mq;
    OSMesg msg;
    uint32_t state;
    uint32_t control;
    int retrace_count = 1;
    // Local deep-copy of the game's mode struct. The game (Factor5/Rogue Squadron)
    // sometimes places its OSViMode on a stack frame or scratch heap that gets
    // reused, zeroing the RDRAM contents under our pointer. Copy into local
    // storage and re-anchor `mode` to it inside osViSetMode.
    OSViMode mode_storage;
};

#define VI_STATE_BLACK 0x20
#define VI_STATE_REPEATLINE 0x40

static struct {
    struct {
        std::thread thread;
        int cur_state;
        int field;
        ViState states[2];
        ultramodern::renderer::ViRegs regs;
        ultramodern::renderer::ViRegs update_screen_regs;

        ViState* get_next_state() {
            return &states[cur_state ^ 1];
        }
        ViState* get_cur_state() {
            return &states[cur_state];
        }
        void update_vi() {
            ViState* next_state = get_next_state();
            const OSViMode* next_mode = next_state->mode;
            const OSViCommonRegs* common_regs = &next_mode->comRegs;
            const OSViFieldRegs* field_regs = &next_mode->fldRegs[field];
            PTR(void) framebuffer = osVirtualToPhysical(next_state->framebuffer);
            PTR(void) origin = framebuffer + field_regs->origin;

            // Process the VI state flags.
            uint32_t hStart = common_regs->hStart;
            if (next_state->state & VI_STATE_BLACK) {
                hStart = 0;
            }

            uint32_t yScale = field_regs->yScale;
            if (next_state->state & VI_STATE_REPEATLINE) {
                yScale = 0;
                origin = framebuffer;
            }

            // TODO implement osViFade

            // Update VI registers.
            regs.VI_ORIGIN_REG = origin;
            regs.VI_WIDTH_REG = common_regs->width;
            regs.VI_TIMING_REG = common_regs->burst;
            regs.VI_V_SYNC_REG = common_regs->vSync;
            regs.VI_H_SYNC_REG = common_regs->hSync;
            regs.VI_LEAP_REG = common_regs->leap;
            regs.VI_H_START_REG = hStart;
            regs.VI_V_START_REG = field_regs->vStart; // TODO implement osViExtendVStart
            regs.VI_V_BURST_REG = field_regs->vBurst;
            regs.VI_INTR_REG = field_regs->vIntr;
            regs.VI_X_SCALE_REG = common_regs->xScale; // TODO implement osViSetXScale
            regs.VI_Y_SCALE_REG = yScale; // TODO implement osViSetYScale
            regs.VI_STATUS_REG = next_state->control;

            { static int n=0; if (++n<=10 || (n%60)==0) {
                if(false) fprintf(stderr, "[trace] update_vi #%d STATUS=0x%X H_START=0x%X WIDTH=0x%X ORIGIN=0x%X state=0x%X ctrl=0x%X\n",
                    n, regs.VI_STATUS_REG, regs.VI_H_START_REG, regs.VI_WIDTH_REG, regs.VI_ORIGIN_REG,
                    next_state->state, next_state->control);
                fflush(stderr);
            } }

            // Swap VI states.
            cur_state ^= 1;
            *get_next_state() = *get_cur_state();
        }
    } vi;
    struct {
        std::thread gfx_thread;
        std::thread task_thread;
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } sp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } dp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } ai;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } si;
    // The same message queue may be used for multiple events, so share a mutex for all of them
    std::mutex message_mutex;
    uint8_t* rdram;
    moodycamel::BlockingConcurrentQueue<Action> action_queue{};
    moodycamel::BlockingConcurrentQueue<OSTask*> sp_task_queue{};
    moodycamel::ConcurrentQueue<OSThread*> deleted_threads{};
} events_context{};

ultramodern::renderer::ViRegs* ultramodern::renderer::get_vi_regs() {
    return &events_context.vi.update_screen_regs;
}

extern "C" void osSetEventMesg(RDRAM_ARG OSEvent event_id, PTR(OSMesgQueue) mq_, OSMesg msg) {
    std::lock_guard lock{ events_context.message_mutex };

    switch (event_id) {
        case OS_EVENT_SP:
            events_context.sp.msg = msg;
            events_context.sp.mq = mq_;
            break;
        case OS_EVENT_DP:
            events_context.dp.msg = msg;
            events_context.dp.mq = mq_;
            break;
        case OS_EVENT_AI:
            events_context.ai.msg = msg;
            events_context.ai.mq = mq_;
            break;
        case OS_EVENT_SI:
            events_context.si.msg = msg;
            events_context.si.mq = mq_;
    }
}

extern "C" void osViSetEvent(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, u32 retrace_count) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mq = mq_;
    next_state->msg = msg;
    next_state->retrace_count = retrace_count;
    if(false) fprintf(stderr, "[trace] osViSetEvent mq=0x%08X msg=0x%016llX rc=%u\n",
        (uint32_t)mq_, (unsigned long long)(uintptr_t)msg, (unsigned)retrace_count);
    fflush(stderr);
}

uint64_t total_vis = 0;


extern std::atomic_bool exited;
extern moodycamel::LightweightSemaphore graphics_shutdown_ready;

void set_dummy_vi(bool odd);

// RogueSquadron64Recomp (2026-09-08): hardware raises the AI interrupt each time a DMA buffer finishes
// playing (~bytes / (4 * rate) seconds apart), and MusyX synthesizes the next buffer on that interrupt.
// Firing it once per VI (60/s) starved production to about half of real time (constant underruns). This
// thread fires the AI event at the consumption cadence of the buffer the game last queued.
// ROGUESQ_AI_CONSUMPTION_PACED=0 restores the per-VI event (see the VI thread).
extern std::atomic<uint32_t> g_rs64_ai_last_bytes;
uint32_t rs64_audio_sample_rate();
static bool ai_consumption_paced() {
    static const bool on = [](){ const char* e = std::getenv("ROGUESQ_AI_CONSUMPTION_PACED"); return !(e && e[0] == '0'); }();
    return on;
}
extern std::mutex g_rs64_ai_mutex;
extern std::deque<std::chrono::steady_clock::time_point> g_rs64_ai_deadlines;
void ai_thread_func() {
    ultramodern::set_native_thread_name("AI Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Critical);
    using namespace std::chrono_literals;
    while (!exited) {
        // Next DMA-complete deadline (one per buffer the game queued); poll briefly when none is pending.
        std::chrono::steady_clock::time_point due{};
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(g_rs64_ai_mutex);
            if (!g_rs64_ai_deadlines.empty()) { due = g_rs64_ai_deadlines.front(); have = true; }
        }
        if (!have) { std::this_thread::sleep_for(1ms); continue; }
        std::this_thread::sleep_until(due);
        {
            std::lock_guard<std::mutex> lk(g_rs64_ai_mutex);
            if (!g_rs64_ai_deadlines.empty()) g_rs64_ai_deadlines.pop_front();
        }
        if (!ultramodern::is_game_started()) continue;
        std::lock_guard lock{ events_context.message_mutex };
        if (events_context.ai.mq != NULLPTR) {
            ultramodern::enqueue_external_message_src(events_context.ai.mq, events_context.ai.msg, false, ultramodern::EventMessageSource::Ai);
        }
    }
}

void vi_thread_func() {
    ultramodern::set_native_thread_name("VI Thread");
    // This thread should be prioritized over every other thread in the application, as it's what allows
    // the game to generate new audio and gfx lists.
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Critical);
    using namespace std::chrono_literals;

    int remaining_retraces = 1;

    while (!exited) {
        // Determine the next VI time (more accurate than adding 16ms each VI interrupt)
        auto next = ultramodern::get_start() + (total_vis * 1000000us) / (60 * ultramodern::get_speed_multiplier());
        //if (next > std::chrono::high_resolution_clock::now()) {
        //    printf("Sleeping for %" PRIu64 " us to get from %" PRIu64 " us to %" PRIu64 " us \n",
        //        (next - std::chrono::high_resolution_clock::now()) / 1us,
        //        (std::chrono::high_resolution_clock::now() - events_context.start) / 1us,
        //        (next - events_context.start) / 1us);
        //} else {
        //    printf("No need to sleep\n");
        //}
        // Detect if there's more than a second to wait and wait a fixed amount instead for the next VI if so, as that usually means the system clock went back in time.
        if (std::chrono::floor<std::chrono::seconds>(next - std::chrono::high_resolution_clock::now()) > 1s) {
            // printf("Skipping the next VI wait\n");
            next = std::chrono::high_resolution_clock::now();
        }
        ultramodern::sleep_until(next);
        auto time_now = ultramodern::time_since_start();
        // Calculate how many VIs have passed
        uint64_t new_total_vis = (time_now * (60 * ultramodern::get_speed_multiplier()) / 1000ms) + 1;
        if (new_total_vis > total_vis + 1) {
            //printf("Skipped % " PRId64 " frames in VI interupt thread!\n", new_total_vis - total_vis - 1);
        }
        total_vis = new_total_vis;

        // If the game hasn't started yet, set a dummy VI mode and origin.
        if (!ultramodern::is_game_started()) {
            static bool odd = false;
            set_dummy_vi(odd);
            odd = !odd;
        }

        // Queue a screen update for the graphics thread with the current VI register state.
        // Doing this before the VI update is equivalent to updating the screen after the previous frame's scanout finished.
        events_context.action_queue.enqueue(ScreenUpdateAction{ events_context.vi.regs });

        // Update VI registers and swap VI modes.
        events_context.vi.update_vi();

        // DIAG-3: read first 4 bytes at VI ORIGIN to distinguish black/white/content
        // framebuffers. If fb_first == 0x00010001, the VI is pointing at a
        // properly-cleared-black RGBA-5551 framebuffer. If 0xFFFFFFFF, white.
        // If something else, we're presenting actual content.
        { static int n=0; if (++n<=10 || (n%60)==0) {
            uint32_t orig_phys = events_context.vi.regs.VI_ORIGIN_REG & 0x00FFFFFF;
            uint32_t fb_first = 0;
            if (events_context.rdram && orig_phys < 0x800000 - 4) {
                fb_first = *(uint32_t*)(events_context.rdram + orig_phys);
            }
            if(false) fprintf(stderr, "[diag-vi] #%d ORIGIN=0x%X fb_first=0x%08X\n",
                n, events_context.vi.regs.VI_ORIGIN_REG, fb_first);
            fflush(stderr);
        } }
        // Watch counter_E byte at MIPS 0x80128EAE (host index 0x128EAD due to byte-swap within word).
        { static uint8_t prev_cE = 0xFF; static uint8_t prev_cF = 0xFF;
            if (events_context.rdram) {
                uint8_t cE = *(uint8_t*)(events_context.rdram + 0x128EADu);
                uint8_t cF = *(uint8_t*)(events_context.rdram + 0x128EACu);
                if (cE != prev_cE || cF != prev_cF) {
                    if(false) fprintf(stderr, "[trace] counter-watch: E=%u F=%u (was E=%u F=%u)\n",
                        (unsigned)cE, (unsigned)cF, (unsigned)prev_cE, (unsigned)prev_cF);
                    fflush(stderr);
                    prev_cE = cE;
                    prev_cF = cF;
                }
            }
        }

        // If the game has started, handle sending VI and AI events.
        if (ultramodern::is_game_started()) {
            remaining_retraces--;
            // Hold this retrace while a graphics parse is in flight (before taking message_mutex: the
            // completion path needs it). Bounded so a runaway parse cannot stop the VI clock.
            // Off by default (2026-09-08): the game thread already waits at frame start, and holding the
            // retrace starves the synth tick (audio dropouts). ROGUESQ_VI_HOLD_PARSE=1 re-enables.
            static const bool s_vi_hold = [](){ const char* e = std::getenv("ROGUESQ_VI_HOLD_PARSE"); return e && e[0] && e[0] != '0'; }();
            if (s_vi_hold && remaining_retraces == 0 && vi_wait_parse_enabled() && g_gfx_parse_inflight.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> plock{ g_gfx_parse_mutex };
                g_gfx_parse_cv.wait_for(plock, std::chrono::milliseconds(50), []{ return g_gfx_parse_inflight.load(std::memory_order_acquire) == 0; });
            }

            std::lock_guard lock{ events_context.message_mutex };
            ViState* cur_state = events_context.vi.get_cur_state();
            if (remaining_retraces == 0) {
                if (cur_state->mq != NULLPTR) {
                    // Send a message to the VI queue, and do not set it to be requeued if the queue was full.
                    // The worst case scenario is that the game misses a VI message and has to wait a little longer for the next.
                    ultramodern::enqueue_external_message_src(cur_state->mq, cur_state->msg, false, ultramodern::EventMessageSource::Vi);
                    { static int n=0; if (++n<=10 || (n%500)==0) {
                        if(false) fprintf(stderr, "[trace] VI->retrace_enqueue #%d mq=0x%08X msg=0x%016llX\n",
                            n, (uint32_t)cur_state->mq, (unsigned long long)(uintptr_t)cur_state->msg);
                        fflush(stderr);
                    } }
                } else {
                    static int nullq_count = 0;
                    if (++nullq_count <= 5 || (nullq_count % 500) == 0) {
                        if(false) fprintf(stderr, "[trace] VI->retrace_enqueue NULL mq #%d\n", nullq_count);
                        fflush(stderr);
                    }
                }
                remaining_retraces = cur_state->retrace_count;
            }
            if (!ai_consumption_paced() && events_context.ai.mq != NULLPTR) {
                // The AI event wakes the game's audio thread to synth + submit one
                // ~768-byte buffer. Firing it once per VI (60/s) only sustains ~half of
                // 22050Hz (the audio underproduces -> crackle/cut). On hardware the AI
                // interrupt fires per buffer CONSUMED (~120/s here). Fire N per retrace
                // (default 2 ~= 120/s for 192-frame buffers @22050). ROGUESQ_AI_PER_VI
                // overrides (1 = old behavior). Extra messages past what the audio thread
                // can dequeue are simply dropped (requeue=false), so this self-limits.
                // Default 1 (one per VI): firing more (=2) DID reach ~realtime production
                // but the game then submits buffers the synth hasn't filled yet -> garbled
                // / screechy. So AI-rate forcing is the wrong lever here; left as an opt-in
                // knob for experiments. Real fix = let the synth fill before submit (async).
                static int n_ai = -1;
                if (n_ai < 0) { const char* e = std::getenv("ROGUESQ_AI_PER_VI"); n_ai = (e && e[0]) ? atoi(e) : 1; if (n_ai < 1) n_ai = 1; if (n_ai > 8) n_ai = 8; }
                for (int i = 0; i < n_ai; ++i) {
                    ultramodern::enqueue_external_message_src(events_context.ai.mq, events_context.ai.msg, false, ultramodern::EventMessageSource::Ai);
                }
            }
        }

        if (events_callbacks.vi_callback != nullptr) {
            events_callbacks.vi_callback();
        }
    }
}

void sp_complete() {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    ultramodern::enqueue_external_message_src(events_context.sp.mq, events_context.sp.msg, false, ultramodern::EventMessageSource::Sp);
}

void dp_complete() {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    static const bool s_lfq = [](){ const char* e = std::getenv("ROGUESQ_LOG_FRAMEQ"); return e && *e && *e != '0'; }();
    if (s_lfq) { static unsigned n = 0; fprintf(stderr, "[frameq] dp_complete #%u -> mq=0x%06X\n", ++n, (uint32_t)events_context.dp.mq & 0xFFFFFFu); fflush(stderr); }
    ultramodern::enqueue_external_message_src(events_context.dp.mq, events_context.dp.msg, false, ultramodern::EventMessageSource::Dp);
}

// Counts async audio synth tasks that have been queued but not yet finished
// writing their output. wait_for_pending_audio_synth() (called from
// osAiSetNextBuffer) blocks until this drains, so the game never plays a
// half-synthesized buffer. Only used when ROGUESQ_AUDIO_ASYNC is on.
static std::atomic<int> g_pending_audio_synth{ 0 };
static std::mutex g_audio_synth_mutex;
static std::condition_variable g_audio_synth_cv;

// ROGUESQ_AUDIO_ASYNC=1: run the slow MusyX synth without blocking the game frame.
static bool audio_async_enabled() {
    static int s_async = -1;
    if (s_async < 0) { const char* e = std::getenv("ROGUESQ_AUDIO_ASYNC"); s_async = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return s_async != 0;
}

void ultramodern::wait_for_pending_audio_synth() {
    if (g_pending_audio_synth.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::unique_lock lock{ g_audio_synth_mutex };
    g_audio_synth_cv.wait(lock, [] { return g_pending_audio_synth.load(std::memory_order_acquire) == 0; });
}

void task_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready) {
    ultramodern::set_native_thread_name("SP Task Thread");
#ifdef __vita__
    // The audio synth (a few ms per task) must not wait behind the gfx thread's long display-list parses on the Vita.
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::High);
#else
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);
#endif

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (true) {
        // Wait until an RSP task has been sent
        OSTask* task;
        events_context.sp_task_queue.wait_dequeue(task);

        if (task == nullptr) {
            return;
        }

        // Cache the task type before running — run_task can clobber the
        // pointer's contents (DMA, etc).
        const uint32_t task_type = task->t.type;

        // ROGUESQ_AUDIO_ASYNC=1: the MusyX synth is slow (~42ms/task). Holding
        // sp_complete until it finishes stalls the game (which blocks on the SP
        // queue) by ~4 audio tasks/frame → ~4fps. Mirror the gfx path: signal
        // completion BEFORE running the synth so the game keeps rendering, and
        // synthesize in the background. The synth has ~1.5x realtime headroom so
        // the queue stays bounded; audio gains ~1 task of latency (double-buffered).
        // Relies on the game preserving the audio input until the next task (same
        // assumption the gfx path makes). Default off.
        const bool early_complete = (audio_async_enabled() && task_type == M_AUDTASK);
        if (early_complete) {
            sp_complete();
        }

        if (!ultramodern::rsp::run_task(PASS_RDRAM task)) {
            fprintf(stderr, "Failed to execute task type: %" PRIu32 "\n", task->t.type);
            ULTRAMODERN_QUICK_EXIT();
        }

        // Tell the game that the RSP has completed (unless already signaled early).
        if (!early_complete) {
            sp_complete();
        }
        else {
            // The synth has now finished filling its output buffer; release any
            // osAiSetNextBuffer caller waiting on this buffer (see wait_for_pending_audio_synth).
            if (g_pending_audio_synth.fetch_sub(1, std::memory_order_release) <= 1) {
                std::lock_guard lock{ g_audio_synth_mutex };
                g_audio_synth_cv.notify_all();
            }
        }

        (void)task_type;
    }
}

std::atomic_uint32_t display_refresh_rate = 60;
std::atomic<float> resolution_scale = 1.0f;

uint32_t ultramodern::get_target_framerate(uint32_t original) {
    auto& config = ultramodern::renderer::get_graphics_config();

    switch (config.rr_option) {
        case ultramodern::renderer::RefreshRate::Original:
        default:
            return original;
        case ultramodern::renderer::RefreshRate::Manual:
            return config.rr_manual_value;
        case ultramodern::renderer::RefreshRate::Display:
            return display_refresh_rate.load();
    }
}

uint32_t ultramodern::get_display_refresh_rate() {
    return display_refresh_rate.load();
}

float ultramodern::get_resolution_scale() {
    return resolution_scale.load();
}

void ultramodern::trigger_config_action() {
    events_context.action_queue.enqueue(UpdateConfigAction{});
}

std::atomic<ultramodern::renderer::SetupResult> renderer_setup_result = ultramodern::renderer::SetupResult::Success;
std::atomic<ultramodern::renderer::GraphicsApi> renderer_chosen_api = ultramodern::renderer::GraphicsApi::Auto;

void gfx_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready, ultramodern::renderer::WindowHandle window_handle) {
    bool enabled_instant_present = false;
    using namespace std::chrono_literals;

    ultramodern::set_native_thread_name("Gfx Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    auto old_config = ultramodern::renderer::get_graphics_config();

    auto renderer_context = ultramodern::renderer::create_render_context(rdram, window_handle, ultramodern::renderer::get_graphics_config().developer_mode);

    renderer_chosen_api.store(renderer_context->get_chosen_api());
    if (!renderer_context->valid()) {
        renderer_setup_result.store(renderer_context->get_setup_result());
        // Notify the caller thread that this thread is ready.
        thread_ready->signal();
        return;
    }

    if (events_callbacks.gfx_init_callback != nullptr) {
        events_callbacks.gfx_init_callback();
    }

    ultramodern::rsp::init();

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (!exited) {
        // Try to pull an action from the queue
        Action action;
        if (events_context.action_queue.wait_dequeue_timed(action, 1ms)) {
            // Determine the action type and act on it
            if (const auto* task_action = std::get_if<SpTaskAction>(&action)) {
                // Turn on instant present if the game has been started and it hasn't been turned on yet.
                if (ultramodern::is_game_started() && !enabled_instant_present) {
                    renderer_context->enable_instant_present();
                    enabled_instant_present = true;
                }
                // Tell the game that the RSP completed instantly. This will allow it to queue other task types, but it won't
                // start another graphics task until the RDP is also complete. Games usually preserve the RSP inputs until the RDP
                // is finished as well, so sending this early shouldn't be an issue in most cases.
                // If this causes issues then the logic can be replaced with responding to yield requests.
                // RogueSquadron64Recomp (2026-09-07): ROGUESQ_SP_COMPLETE_AFTER_PARSE=1 delivers the SP-done
                // event only after the list has been parsed, as the RSP would. With the early completion the
                // game recycles DL chunks the HLE is still walking (phantom commands / garbage SETCIMG).
                static const bool s_sp_after = [](){ const char* e = std::getenv("ROGUESQ_SP_COMPLETE_AFTER_PARSE"); const char* v = std::getenv("ROGUESQ_VI_DRIVEN_LOOP"); if (e && *e) return *e != '0'; return !(v && *v && *v == '0'); }();   // default on with the VI-driven loop
                if (!s_sp_after) sp_complete();
                ultramodern::measure_input_latency();

                PTR(u64) displaylist = task_action->task.t.data_ptr;
                ultramodern::extensions::on_displaylist_submitted(displaylist);

                // HLE rendering (send_dl) is suppressed for Rogue Squadron — the game
                // uses Factor5 ucode, which we drive via the LLE recompile + dpc_bridge.
                // Running HLE in parallel races with LLE (flicker) and floods the
                // gfx_thread, starving the Win32 message pump (Not Responding).
                // We keep sp_complete/dp_complete so the game's frame state machine
                // still advances on the expected schedule.
                [[maybe_unused]] auto renderer_start = std::chrono::high_resolution_clock::now();
                // RogueSquadron64Recomp: send_dl RE-ENABLED. RT64 has a
                // GBI_F3DFACTOR5 implementation (rt64_gbi_f3dfactor5.cpp)
                // and the Factor 5 ucode hash IS in RT64's database
                // (rt64_gbi.cpp lines 166, 264). HLE GBI handles Factor 5
                // commands properly. The previous LLE-only approach
                // bypasses the registered GBI and submits raw RDP that
                // confuses RT64 state. See project_factor5_lle_breakthrough.md
                // for context but supersede with HLE path.
                // ROGUESQ_LOG_GFX_TASK=1: one line per gfx task around the parse (stall diagnosis).
                static const bool s_log_task = [](){ const char* e = std::getenv("ROGUESQ_LOG_GFX_TASK"); return e && *e && *e != '0'; }();
                static unsigned s_task_n = 0; ++s_task_n;
                if (s_log_task) { fprintf(stderr, "[gfx-task #%u] parse dl=0x%08X\n", s_task_n, (uint32_t)displaylist); fflush(stderr); }
                g_rs64_parse_rdram = task_action->snapshot;
                renderer_context->send_dl(&task_action->task);
                g_rs64_parse_rdram = nullptr;
                // RogueSquadron64Recomp (2026-09-09): dump the EXACT snapshot the parser just walked, so
                // tools/validate/f5_dl_walk.py sees the parser-visible DL, not live RDRAM (which races with
                // post-parse chunk recycling). Anchored to the cinematic frame counter (RDRAM 0x13889C);
                // written big-endian (i^3) once. ROGUESQ_DUMP_PARSE_SNAPSHOT_ON_ITER=<n> [+ _PATH].
                static const long s_dump_iter = [](){ const char* e = std::getenv("ROGUESQ_DUMP_PARSE_SNAPSHOT_ON_ITER"); return (e && *e) ? std::atol(e) : -1L; }();
                static const int s_dump_cnt = [](){ const char* e = std::getenv("ROGUESQ_DUMP_PARSE_SNAPSHOT_COUNT"); return (e && *e) ? std::atoi(e) : 1; }();
                if (s_dump_iter >= 0 && task_action->snapshot) {
                    // Once the cinematic-loop iteration reaches the target (the SAME landmark as
                    // ROGUESQ_DUMP_RDRAM_ON_CINE_ITER and the PJ64 cine_frame<n> goldens - NOT the raw
                    // 0x13889C frame tick), dump the next COUNT gfx-task snapshots numbered _taskNN.bin
                    // (several gfx tasks per frame). Pick the one matching the golden offline.
                    static bool s_armed = false; static int s_dumped = 0;
                    const uint8_t* snap = task_action->snapshot;
                    const uint32_t fc = *reinterpret_cast<const uint32_t*>(snap + 0x13889C);
                    if (!s_armed) {
                        if (s_dump_iter >= 0 && rs64_cine_iter_get() >= (unsigned long long)s_dump_iter) s_armed = true;
                    }
                    if (s_armed && s_dumped < s_dump_cnt) {
                        const char* path = std::getenv("ROGUESQ_DUMP_PARSE_SNAPSHOT_PATH");
                        const char* base = (path && *path) ? path : "parse_snapshot";
                        char fn[1024]; snprintf(fn, sizeof(fn), "%s_task%02d.bin", base, s_dumped);
                        if (FILE* f = fopen(fn, "wb")) {
                            static uint8_t obuf[0x800000];
                            for (uint32_t i = 0; i < 0x800000u; ++i) obuf[i] = snap[i ^ 3];
                            fwrite(obuf, 1, sizeof(obuf), f); fclose(f);
                            fprintf(stderr, "[parse-snapshot] task%02d fc=%u dl=0x%08X -> %s\n", s_dumped, fc, (uint32_t)displaylist, fn); fflush(stderr);
                        }
                        ++s_dumped;
                    }
                }
                [[maybe_unused]] auto renderer_end = std::chrono::high_resolution_clock::now();

                if (s_sp_after) sp_complete();
                dp_complete();
                { std::lock_guard<std::mutex> plock{ g_gfx_parse_mutex }; if (g_gfx_parse_inflight.load(std::memory_order_acquire) > 0) g_gfx_parse_inflight.fetch_sub(1, std::memory_order_acq_rel); }
                g_gfx_parse_cv.notify_all();
                if (s_log_task) { fprintf(stderr, "[gfx-task #%u] done sp+dp signaled parse=%lld us pdl=%lld us hops=%u faces=%u req=%u inflight=%d\n", s_task_n, (long long)std::chrono::duration_cast<std::chrono::microseconds>(renderer_end - renderer_start).count(), g_rs64_pdl_us, g_f5_task_hops, g_f5_task_faces, g_rs64_gfx_requests, g_gfx_parse_inflight.load(std::memory_order_acquire)); fflush(stderr); }
                if (gfx_sync_submit_enabled()) g_gfx_task_parsed.signal();   // ROGUESQ_GFX_SYNC_SUBMIT
                // TODO hook the parsed event up to the actual parsing point when a callback is added to RT64.
                ultramodern::extensions::on_displaylist_parsed(displaylist);
                ultramodern::extensions::on_displaylist_completed(displaylist);
                // printf("Renderer ProcessDList time: %d us\n", static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(renderer_end - renderer_start).count()));
            }
            else if (const auto* screen_update_action = std::get_if<ScreenUpdateAction>(&action)) {
                events_context.vi.update_screen_regs = screen_update_action->regs;
                renderer_context->update_screen();
                display_refresh_rate = renderer_context->get_display_framerate();
                resolution_scale = renderer_context->get_resolution_scale();
            }
            else if (const auto* config_action = std::get_if<UpdateConfigAction>(&action)) {
                (void)config_action;
                auto new_config = ultramodern::renderer::get_graphics_config();
                if (renderer_context->update_config(old_config, new_config)) {
                    old_config = new_config;
                }
            }
            else if (const auto* rdp_action = std::get_if<RdpRangeAction>(&action)) {
                renderer_context->send_rdp_range(rdp_action->lo_phys, rdp_action->hi_phys);
            }
            else if (const auto* rdp_batch = std::get_if<RdpRangeBatchAction>(&action)) {
                for (const auto& r : rdp_batch->ranges) {
                    renderer_context->send_rdp_range(r.first, r.second);
                }
            }
        }
    }

    graphics_shutdown_ready.wait();
    renderer_context->shutdown();
}

#define VI_CTRL_TYPE_16             0x00002
#define VI_CTRL_TYPE_32             0x00003
#define VI_CTRL_GAMMA_DITHER_ON     0x00004
#define VI_CTRL_GAMMA_ON            0x00008
#define VI_CTRL_DIVOT_ON            0x00010
#define VI_CTRL_SERRATE_ON          0x00040
#define VI_CTRL_ANTIALIAS_MASK      0x00300
#define VI_CTRL_ANTIALIAS_MODE_1    0x00100
#define VI_CTRL_ANTIALIAS_MODE_2    0x00200
#define VI_CTRL_ANTIALIAS_MODE_3    0x00300
#define VI_CTRL_PIXEL_ADV_MASK      0x01000
#define VI_CTRL_PIXEL_ADV_1         0x01000
#define VI_CTRL_PIXEL_ADV_2         0x02000
#define VI_CTRL_PIXEL_ADV_3         0x03000
#define VI_CTRL_DITHER_FILTER_ON    0x10000

static const OSViMode dummy_mode = []() {
    OSViMode ret{};

    ret.type = 2;
    ret.comRegs.ctrl = VI_CTRL_TYPE_16 | VI_CTRL_GAMMA_DITHER_ON | VI_CTRL_GAMMA_ON | VI_CTRL_DIVOT_ON | VI_CTRL_ANTIALIAS_MODE_1 | VI_CTRL_PIXEL_ADV_3;
    ret.comRegs.width = 0x140;
    ret.comRegs.burst = 0x03E52239;
    ret.comRegs.vSync = 0x20D;
    ret.comRegs.hSync = 0xC15;
    ret.comRegs.leap = 0x0C150C15;
    ret.comRegs.hStart = 0x006C02EC;
    ret.comRegs.xScale = 0x200;
    ret.comRegs.vCurrent = 0x0;

    for (int field = 0; field < 2; field++) {
        ret.fldRegs[field].origin = 0x280;
        ret.fldRegs[field].yScale = 0x400;
        ret.fldRegs[field].vStart = 0x2501FF;
        ret.fldRegs[field].vBurst = 0xE0204;
        ret.fldRegs[field].vIntr = 0x2;
    }

    return ret;
}();

void set_dummy_vi(bool odd) {
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode = &dummy_mode;
    // Set up a dummy framebuffer.
    next_state->framebuffer = 0x80700000;
    if (odd) {
        next_state->framebuffer += 0x25800;
    }
}

extern "C" void osViSwapBuffer(RDRAM_ARG PTR(void) frameBufPtr) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    next_state->framebuffer = frameBufPtr;
    // Rogue Squadron / Factor5 calls osViBlack(1) at boot but never osViBlack(0).
    // Submitting a real framebuffer for swap implies the game wants the display
    // unblanked, so auto-clear the BLACK flag here.
    next_state->state &= ~VI_STATE_BLACK;
    { static int n=0;
      static uint32_t seen[16] = {0};
      static int n_seen = 0;
      ++n;
      uint32_t fb = (uint32_t)frameBufPtr;
      bool is_new = true;
      for (int k = 0; k < n_seen; ++k) if (seen[k] == fb) { is_new = false; break; }
      if (is_new && n_seen < 16) seen[n_seen++] = fb;
      if (n<=10 || (n%50)==0 || is_new) {
          if(false) fprintf(stderr, "[trace] osViSwapBuffer #%d fb=0x%08X state-after=0x%X%s\n",
              n, fb, next_state->state, is_new ? "  *** NEW ***" : "");
          fflush(stderr);
      }
    }
}

extern "C" void osViSetMode(RDRAM_ARG PTR(OSViMode) mode_) {
    std::lock_guard lock{ events_context.message_mutex };
    OSViMode* mode = TO_PTR(OSViMode, mode_);
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode_storage = *mode;
    next_state->mode = &next_state->mode_storage;
    next_state->control = next_state->mode->comRegs.ctrl;
    { static int n=0; if (++n<=10) {
        if(false) fprintf(stderr, "[trace] osViSetMode #%d modePtr=0x%08X ctrl=0x%08X type=0x%X width=0x%X hStart=0x%X (deep-copied)\n",
            n, (uint32_t)mode_, next_state->control,
            next_state->control & 0x3, next_state->mode->comRegs.width,
            next_state->mode->comRegs.hStart);
        fflush(stderr);
    } }
}

#define OS_VI_GAMMA_ON          0x0001
#define OS_VI_GAMMA_OFF         0x0002
#define OS_VI_GAMMA_DITHER_ON   0x0004
#define OS_VI_GAMMA_DITHER_OFF  0x0008
#define OS_VI_DIVOT_ON          0x0010
#define OS_VI_DIVOT_OFF         0x0020
#define OS_VI_DITHER_FILTER_ON  0x0040
#define OS_VI_DITHER_FILTER_OFF 0x0080

extern "C" void osViSetSpecialFeatures(uint32_t func) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* control_out = &next_state->control;
    if ((func & OS_VI_GAMMA_ON) != 0) {
        *control_out |= VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_OFF) != 0) {
        *control_out &= ~VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_ON) != 0) {
        *control_out |= VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_OFF) != 0) {
        *control_out &= ~VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_DIVOT_ON) != 0) {
        *control_out |= VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DIVOT_OFF) != 0) {
        *control_out &= ~VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DITHER_FILTER_ON) != 0) {
        *control_out |= VI_CTRL_DITHER_FILTER_ON;
        *control_out &= ~VI_CTRL_ANTIALIAS_MASK;
    }

    if ((func & OS_VI_DITHER_FILTER_OFF) != 0) {
        *control_out &= ~VI_CTRL_DITHER_FILTER_ON;
        *control_out |= next_state->mode->comRegs.ctrl & VI_CTRL_ANTIALIAS_MASK;
    }
}

extern "C" void osViBlack(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_BLACK;
    } else {
        *state_out &= ~VI_STATE_BLACK;
    }
    { static int n=0; if (++n<=20) { if(false) fprintf(stderr, "[trace] osViBlack #%d active=%u state=0x%X\n", n, (unsigned)active, *state_out); fflush(stderr); } }
}

extern "C" void osViRepeatLine(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_REPEATLINE;
    } else {
        *state_out &= ~VI_STATE_REPEATLINE;
    }
}

extern "C" void osViSetXScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" void osViSetYScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" PTR(void) osViGetNextFramebuffer() {
    return events_context.vi.get_next_state()->framebuffer;
}

extern "C" PTR(void) osViGetCurrentFramebuffer() {
    return events_context.vi.get_cur_state()->framebuffer;
}

static bool gfx_sync_submit_enabled() {
    static int s = -1;
    if (s < 0) { const char* e = std::getenv("ROGUESQ_GFX_SYNC_SUBMIT"); s = (e && *e && *e != '0') ? 1 : 0; }
    return s == 1;
}
moodycamel::LightweightSemaphore g_gfx_task_parsed;

void ultramodern::submit_rsp_task(RDRAM_ARG PTR(OSTask) task_) {
    OSTask* task = TO_PTR(OSTask, task_);

    // 2026-05-10: GFX tasks now route to the gfx_thread's action_queue so
    // RT64's registered F3DFACTOR5 GBI handles them via send_dl →
    // processDisplayLists(isHLE=true). Non-GFX tasks (audio, etc.) still go
    // to sp_task_queue → task_thread_func → rsp::run_task (LLE).
    if (task->t.type == M_GFXTASK) {
        events_context.action_queue.enqueue(SpTaskAction{ *task, parse_snapshot_enabled() ? take_rdram_snapshot(rdram) : nullptr });
        // RogueSquadron64Recomp (2026-09-07): ROGUESQ_GFX_SYNC_SUBMIT=1 waits until the gfx thread has
        // parsed this list (send_dl + dp_complete). On hardware the RSP consumes the list before the game
        // can recycle its DL chunks; otherwise the game thread runs ahead and reuses chunks the HLE is
        // still walking (phantom commands, garbage SETCIMG, fb-registry crash). Experiment.
        if (gfx_sync_submit_enabled()) g_gfx_task_parsed.wait();
    }
    else {
        // Track in-flight async audio synth tasks so osAiSetNextBuffer can wait
        // for the output buffer to be filled before the game plays it.
        if (audio_async_enabled() && task->t.type == M_AUDTASK) {
            g_pending_audio_synth.fetch_add(1, std::memory_order_acq_rel);
        }
        events_context.sp_task_queue.enqueue(task);
    }
}

ULTRAMODERN_OVERRIDABLE void ultramodern::submit_rdp_range(uint32_t lo_phys, uint32_t hi_phys) {
    if (hi_phys > lo_phys) {
        events_context.action_queue.enqueue(RdpRangeAction{ lo_phys, hi_phys });
    }
}

void ultramodern::submit_rdp_range_batch(std::vector<std::pair<uint32_t, uint32_t>>&& ranges) {
    if (!ranges.empty()) {
        events_context.action_queue.enqueue(RdpRangeBatchAction{ std::move(ranges) });
    }
}

void ultramodern::send_si_message() {
    ultramodern::enqueue_external_message_src(events_context.si.mq, events_context.si.msg, false, ultramodern::EventMessageSource::Si);
}

void ultramodern::init_events(RDRAM_ARG ultramodern::renderer::WindowHandle window_handle) {
    moodycamel::LightweightSemaphore gfx_thread_ready;
    moodycamel::LightweightSemaphore task_thread_ready;
    events_context.rdram = rdram;
    events_context.sp.gfx_thread = std::thread{ gfx_thread_func, rdram, &gfx_thread_ready, window_handle };
    events_context.sp.task_thread = std::thread{ task_thread_func, rdram, &task_thread_ready };

    // Wait for the two sp threads to be ready before continuing to prevent the game from
    // running before we're able to handle RSP tasks.
    gfx_thread_ready.wait();
    task_thread_ready.wait();

    ultramodern::renderer::SetupResult setup_result = renderer_setup_result.load();
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        auto show_renderer_error = [](const std::string& msg) {
            std::string error_msg = "An error has been encountered on startup: " + msg;

            ultramodern::error_handling::message_box(error_msg.c_str());
        };

        const std::string driver_os_suffix = "\nPlease make sure your GPU drivers and your OS are up to date.";
        switch (setup_result) {
            case ultramodern::renderer::SetupResult::Success:
                break;
            case ultramodern::renderer::SetupResult::DynamicLibrariesNotFound:
                show_renderer_error("Failed to load dynamic libraries. Make sure the DLLs are next to the recomp executable.");
                break;
            case ultramodern::renderer::SetupResult::InvalidGraphicsAPI:
                show_renderer_error(ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + " is not supported on this platform. Please select a different graphics API.");
                break;
            case ultramodern::renderer::SetupResult::GraphicsAPINotFound:
                show_renderer_error("Unable to initialize " + ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + "." + driver_os_suffix);
                break;
            case ultramodern::renderer::SetupResult::GraphicsDeviceNotFound:
                show_renderer_error("Unable to find compatible graphics device." + driver_os_suffix);
                break;
        }
        throw std::runtime_error("Failed to initialize the renderer");
    }

    // Seed both VI states with the dummy mode so the VI thread's first
    // update_vi() has a valid mode pointer even if the game starts before
    // calling osViSetMode. Otherwise update_vi dereferences a null mode.
    events_context.vi.states[0].mode = &dummy_mode;
    events_context.vi.states[1].mode = &dummy_mode;

    events_context.vi.thread = std::thread{ vi_thread_func };
    if (ai_consumption_paced()) { static std::thread s_ai_thread{ ai_thread_func }; s_ai_thread.detach(); }
}

void ultramodern::join_event_threads() {
    events_context.sp.gfx_thread.join();
    events_context.vi.thread.join();

    // Send a null RSP task to indicate that the RSP task thread should exit.
    events_context.sp_task_queue.enqueue(nullptr);
    events_context.sp.task_thread.join();
}
