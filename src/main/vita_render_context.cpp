// PS Vita render context: display lists go through RT64's Factor 5 parser over the vita_gfx shim (CPU transform) and are
// drawn with vitaGL. Replaces rt64_render_context.cpp. ROGUESQ_VITA_GFX=0 drops display lists (Phase 1 behavior).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <memory>
#include <string>
#include <unordered_map>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

#include "ultramodern/ultramodern.hpp"

#include "rt64_render_context.h"
#include "video_config.h"
#include "game_state.h"
#include "nav_sequencer.h"
#include "hook_helpers.h"
#include "vg_capture.h"
#include "debug_logs.h"

#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"
#include "vg_backend_gl.h"
#include "vg_controls_panel.h"
#include "vg_prof.h"
#include "vg_renderer.h"
#include "vg_submit.h"
#include "main.h"
#include "vita_controls_ui.h"

// Draw distance multiplier (desktop keeps it in video_config.cpp next to the RT64 config mapping).
namespace rs64::video {
    namespace {
        std::atomic<float> g_draw_distance{1.0f};
    }

    float draw_distance() {
        return g_draw_distance.load(std::memory_order_relaxed);
    }

    void set_draw_distance(float mult) {
        g_draw_distance.store(std::clamp(mult, kDrawDistanceMin, kDrawDistanceMax), std::memory_order_relaxed);
    }
}

extern "C" std::atomic<unsigned> g_rs64_swap_count;  // upstream_compat.cpp
extern "C" uint8_t* g_rs64_parse_rdram;
extern "C" volatile long long g_rs64_snap_us;
// Game-side counters (upstream_compat.cpp, ultramodern timer.cpp) for the per-window report.
extern "C" volatile unsigned g_rs64_pw_calls, g_rs64_pw_waited, g_rs64_pw_ms;
extern "C" volatile uint32_t g_rs64_osgetcount_calls, g_rs64_osgettime_calls;

#ifdef __vita__
#include "vita_host.h"
static void sync_log() {
    rs64::vita::sync_log();
}
#else
static void sync_log() {}
#endif

namespace {
    using clock_type = std::chrono::steady_clock;

    class VitaRenderContext final : public ultramodern::renderer::RendererContext {
    public:
        explicit VitaRenderContext(uint8_t* rdram) : rdram(rdram) {
            window_start = clock_type::now();
            rs64_nav_set_target(recomp::dbg::env_str("ROGUESQ_BOOT_TARGET"));
            render_enabled = recomp::dbg::env_on("ROGUESQ_VITA_GFX", true);
            backend.resScale = std::clamp(recomp::dbg::env_int("ROGUESQ_VITA_RES_SCALE", 2), 1, 3);
            // ROGUESQ_VITA_ASPECT=4:3 keeps the pillarbox; the default stretches the image over the whole screen.
            const char* aspect = recomp::dbg::env_str("ROGUESQ_VITA_ASPECT");
            backend.stretchToScreen = !(aspect && (std::strcmp(aspect, "4:3") == 0));
            // ROGUESQ_VG_PROF=1: per-section parser timers in the 5-second report.
            vg::g_profEnabled = recomp::dbg::env_on("ROGUESQ_VG_PROF", false);
            if (vg::g_profEnabled) {
                vg::profInit();
            }
            // ROGUESQ_VG_RENDER_THREAD=0 runs the GL backend inline on the gfx thread; ROGUESQ_VG_RESOLVE_THREAD=0 does the same for texture loads and hashing.
            submitter = std::make_unique<vg::Submitter>(&backend, recomp::dbg::env_on("ROGUESQ_VG_RENDER_THREAD", true),
                recomp::dbg::env_on("ROGUESQ_VG_RESOLVE_THREAD", true));
            if (render_enabled && !submitter->start()) {
                render_enabled = false;
            }
            sync_log();
            renderer = std::make_unique<vg::Renderer>(submitter.get());
            state = std::make_unique<RT64::State>(rdram, renderer.get());
            interpreter.setup(state.get());
            setup_result = ultramodern::renderer::SetupResult::Success;
        }

        bool valid() override { return true; }

        bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override {
            return true;
        }

        void enable_instant_present() override {}

        void send_dl(const OSTask* task) override {
            ++tasks_total;
            ++tasks_window;
            if (render_enabled) {
                const auto t0 = clock_type::now();
                const uint32_t dl = uint32_t(task->t.data_ptr) & 0x3FFFFFF;
                // Parse from the task-start RDRAM snapshot (events.cpp) when one exists, so the game builds the next frame meanwhile.
                uint8_t* const parseMem = g_rs64_parse_rdram ? g_rs64_parse_rdram : rdram;
                rs64_neutralize_matpool_cimg(parseMem, dl);
                rs64_vg_capture_task(parseMem, uint32_t(task->t.ucode) & 0x3FFFFFF, uint32_t(task->t.ucode_data) & 0x3FFFFFF, dl);
                state->RDRAM = parseMem;
                state->rsp->reset();
                interpreter.loadUCodeGBI(uint32_t(task->t.ucode) & 0x3FFFFFF, uint32_t(task->t.ucode_data) & 0x3FFFFFF, true);
                renderer->beginTask(state.get());
                interpreter.processDisplayLists(dl, reinterpret_cast<RT64::DisplayList*>(parseMem + dl));
                renderer->endTask();
                state->RDRAM = rdram;
                dl_us_window += std::chrono::duration_cast<std::chrono::microseconds>(clock_type::now() - t0).count();
            }
            report();
        }

        // Per-VI: game hooks the desktop runs from rt64_render_context.cpp (state classifier, nav engine), then present.
        void update_screen() override {
            ++vi_window;
            // During the first 20 seconds the log is committed twice a second, so a console reset still leaves a trail.
            if ((++vi_total <= 1200) && ((vi_total % 30) == 0)) {
                sync_log();
            }
            rs64_state_poll(rdram);
            rs64_nav_tick(rdram);
            log_vi_mode();
            if (render_enabled) {
                const auto* regs = ultramodern::renderer::get_vi_regs();
                vg::ViView view;
                view.origin = regs->VI_ORIGIN_REG & 0xFFFFFF;
                view.width = regs->VI_WIDTH_REG & 0xFFF;
                view.hStart = regs->VI_H_START_REG;
                view.vStart = regs->VI_V_START_REG;
                view.xScale = regs->VI_X_SCALE_REG;
                view.yScale = regs->VI_Y_SCALE_REG;
                rs64_vg_capture_vi(view.origin);
                // The game's Controller Settings screen shows an N64 pad; list each action's Vita controls over it.
                std::vector<vg::OverlayItem> overlay;
                if (std::strcmp(rs64_state_current_id(), "menu.controller_settings") == 0) {
                    const rs64::input::Bindings bindings = rs64_input_bindings_snapshot();
                    overlay = vg::controlsPanel(rdram, [&bindings](uint16_t bit) { return rs64::vita::vita_button_names(bindings, bit); },
                        rs64::vita::controls_ui_state());
                }
                submitter->present(view, std::move(overlay));
            }
        }

        void shutdown() override {}

        uint32_t get_display_framerate() const override { return 60; }

        float get_resolution_scale() const override { return float(backend.resScale); }

    private:
        // The VI registers that decide which framebuffer lines reach the screen, logged whenever they change.
        void log_vi_mode() {
            const auto* vi = ultramodern::renderer::get_vi_regs();
            const uint32_t key[6] = { vi->VI_STATUS_REG, vi->VI_WIDTH_REG, vi->VI_H_START_REG, vi->VI_V_START_REG, vi->VI_X_SCALE_REG, vi->VI_Y_SCALE_REG };
            if (std::memcmp(key, last_vi_mode, sizeof(key)) != 0) {
                std::memcpy(last_vi_mode, key, sizeof(key));
                fprintf(stderr, "[vita-vi] status 0x%X width %u h_start 0x%08X v_start 0x%08X x_scale 0x%X y_scale 0x%X origin 0x%06X\n",
                    key[0], key[1], key[2], key[3], key[4], key[5], vi->VI_ORIGIN_REG & 0xFFFFFF);
            }
        }

        void report() {
            const auto now = clock_type::now();
            const double secs = std::chrono::duration<double>(now - window_start).count();
            if (secs < 5.0) {
                return;
            }
            const unsigned swaps = g_rs64_swap_count.load(std::memory_order_relaxed);
            fprintf(stderr, "[vita-gfx] frames (swaps): %.1f/s, presents: %.1f/s (stale %u, low %u), gfx tasks: %.1f/s, vi: %.1f/s, vram free %u KB, state=%s\n",
                (swaps - swaps_prev) / secs, backend.statPresents / secs, backend.statStalePresents, backend.statLowPresents,
                tasks_window / secs, vi_window / secs, vg::Backend::freeVramKB(), rs64_state_current_id());
            if (render_enabled) {
                const double tasks = std::max(1u, tasks_window);
                fprintf(stderr, "[vita-gfx] per task: parse+submit %.2f ms (resolve wait %.2f, resolve busy %.2f), tris %.0f, rects %.0f, batches %.0f | texture hashes +%u, decodes +%u (again +%u), uploads +%u, shaders compiled +%u cached +%u, skipped %u | snapshot copy %.2f ms, heap in use %u KB\n",
                    dl_us_window / tasks / 1000.0, submitter->statTaskWaitUs.load() / tasks / 1000.0, submitter->statResolveUs.load() / tasks / 1000.0,
                    submitter->statTris.load() / tasks, renderer->statRects / tasks, submitter->statBatches.load() / tasks,
                    submitter->statHashes.load(), submitter->statDecodes.load(), submitter->statRedecodes.load(), backend.statTexUploads, backend.statShaderCompiles, backend.statShaderCacheHits, backend.statSkipped,
                    g_rs64_snap_us / 1000.0, unsigned(mallinfo().uordblks / 1024));
                const unsigned pwCalls = g_rs64_pw_calls, pwWaited = g_rs64_pw_waited, pwMs = g_rs64_pw_ms;
                const uint32_t getCount = g_rs64_osgetcount_calls, getTime = g_rs64_osgettime_calls;
                fprintf(stderr, "[vita-game] per task: vertices %.0f (transform reused %.0f) | parse-wait hooks %.1f, waited %.1f for %.2f ms | osGetCount %.1f, osGetTime %.1f\n",
                    state->rsp->statVertices / tasks, state->rsp->statReusedVertices / tasks, (pwCalls - pw_calls_prev) / tasks, (pwWaited - pw_waited_prev) / tasks,
                    (pwMs - pw_ms_prev) / tasks, (getCount - getcount_prev) / tasks, (getTime - gettime_prev) / tasks);
                pw_calls_prev = pwCalls;
                pw_waited_prev = pwWaited;
                pw_ms_prev = pwMs;
                getcount_prev = getCount;
                gettime_prev = getTime;
                state->rsp->statVertices = state->rsp->statReusedVertices = 0;
                if (vg::g_profEnabled) {
                    std::string line = "[vita-gfx] per task ms:";
                    for (int i = 0; i < vg::PROF_COUNT; i++) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), " %s %.2f", vg::g_profNames[i], vg::profTicksToUs(double(vg::g_profTicks[i])) / tasks / 1000.0);
                        line += buf;
                    }
                    fprintf(stderr, "%s\n", line.c_str());
                    vg::profReset();
                }
            }
            swaps_prev = swaps;
            log_thread_cpu(secs);
            tasks_window = 0;
            vi_window = 0;
            dl_us_window = 0;
            renderer->statRects = 0;
            submitter->statTris = submitter->statBatches = 0;
            submitter->statHashes = submitter->statDecodes = submitter->statRedecodes = 0;
            submitter->statTaskWaitUs = submitter->statResolveUs = 0;
            backend.statShaderCompiles = backend.statShaderCacheHits = backend.statTexUploads = backend.statSkipped = backend.statDraws = backend.statPresents = 0;
            backend.statStalePresents = backend.statLowPresents = 0;
            window_start = now;
            sync_log();
        }

        // Per-thread CPU share over the last report window, from each named thread's run clock.
        void log_thread_cpu(double secs) {
#ifdef __vita__
            std::string line = "[vita-cpu]";
            double total = 0;
            auto threads = ultramodern::vita_named_threads();
            if (submitter->renderThreadId >= 0) {
                threads.emplace_back(submitter->renderThreadId, "Render Thread");
            }
            if (const int resolveId = submitter->resolveThreadId.load(); resolveId >= 0) {
                threads.emplace_back(resolveId, "Resolve Thread");
            }
            for (const auto& [uid, name] : threads) {
                SceKernelThreadInfo info{};
                info.size = sizeof(info);
                if (sceKernelGetThreadInfo(uid, &info) < 0) {
                    continue;
                }
                const unsigned long long run = info.runClocks;
                auto it = last_run.find(uid);
                const unsigned long long prev = (it == last_run.end()) ? run : it->second;
                last_run[uid] = run;
                const double pct = (double)(run - prev) / (secs * 10000.0);
                total += pct;
                if (pct >= 1.0) {
                    char buf[96];
                    snprintf(buf, sizeof(buf), " %s=%.0f%%", name.c_str(), pct);
                    line += buf;
                }
            }
            fprintf(stderr, "%s | named total=%.0f%% of one core\n", line.c_str(), total);
#else
            (void)secs;
#endif
        }

        std::unordered_map<int, unsigned long long> last_run;
        unsigned pw_calls_prev = 0, pw_waited_prev = 0, pw_ms_prev = 0;
        uint32_t getcount_prev = 0, gettime_prev = 0;
        uint32_t last_vi_mode[6] = {};
        unsigned swaps_prev = 0;
        uint8_t* rdram;
        bool render_enabled = true;
        vg::Backend backend;
        // Declared after the backend so its render thread stops before the backend is destroyed.
        std::unique_ptr<vg::Submitter> submitter;
        std::unique_ptr<vg::Renderer> renderer;
        std::unique_ptr<RT64::State> state;
        RT64::Interpreter interpreter;
        clock_type::time_point window_start;
        unsigned long long tasks_total = 0;
        unsigned tasks_window = 0;
        unsigned vi_window = 0;
        unsigned vi_total = 0;
        long long dl_us_window = 0;
    };
}

namespace recomp {
    std::unique_ptr<ultramodern::renderer::RendererContext>
    create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
        fprintf(stderr, "[vita-gfx] render context: RT64 Factor 5 parser + vitaGL\n");
        return std::make_unique<VitaRenderContext>(rdram);
    }
}

extern "C" int rs64_rt64_inspector_open(void) { return 0; }
extern "C" void rs64_render_suspend_surface(void) {}
extern "C" bool rs64_render_resume_surface(void) { return true; }
extern "C" bool rs64_render_surface_suspended(void) { return false; }

// Game-side hooks from rogue_squadron.toml that the desktop implements in rt64_render_context.cpp.
extern "C" float rs64_cull_widen(void) { return 1.0f; }
extern "C" void rs64_sanitize_fb_registry(void) {}

// Renderer profiling counters read by upstream_compat.cpp (F5 profiler HUD) and events.cpp (frame logs).
extern "C" std::atomic<uint32_t> g_rs64_frame_tris{0};
extern "C" std::atomic<uint32_t> g_rs64_frame_draws{0};
extern "C" std::atomic<uint32_t> g_rs64_frame_texloads{0};
extern "C" volatile long long g_rs64_pdl_us = 0;
