// Replays a frame captured with ROGUESQ_VG_CAPTURE through the Vita renderer (desktop GL backend) and writes the
// presented color image as a PNG. Usage: vg_replay <capture dir> <out.png> [res scale]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <SDL.h>
#include <SDL_opengl.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"
#include "vg_backend_gl.h"
#include "vg_capture.h"
#include "vg_controls_panel.h"
#include "vg_prof.h"
#include "vg_renderer.h"
#include "vg_submit.h"

// Game-side globals the F5 parser reads (src/main/hook_helpers.cpp in the game build).
extern "C" volatile int g_current_scene = -1;
extern "C" volatile unsigned g_op_bf_count = 0;

// VG_REPLAY_PAGEMAP=1: the last pass runs with RDRAM protected; each first touch of a page is recorded and the page reopened,
// giving the set of RDRAM pages the parser reads or writes for the frame.
static uint8_t *g_pageMapBase = nullptr;
static size_t g_pageMapSize = 0, g_pageSize = 0;
static std::vector<uint8_t> g_pagesTouched;

static void pageMapHandler(int sig, siginfo_t *info, void *) {
    uint8_t *address = static_cast<uint8_t *>(info->si_addr);
    if ((address >= g_pageMapBase) && (address < g_pageMapBase + g_pageMapSize)) {
        const size_t page = size_t(address - g_pageMapBase) / g_pageSize;
        g_pagesTouched[page] = 1;
        mprotect(g_pageMapBase + page * g_pageSize, g_pageSize, PROT_READ | PROT_WRITE);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <capture dir> <out.png> [res scale]\n", argv[0]);
        return 1;
    }
    const std::string dir = argv[1];
    const int scale = (argc > 3) ? atoi(argv[3]) : 2;
    // Same terrain subdivision default as the Vita (vita_host.cpp).
    setenv("ROGUESQ_F5_TERRAIN_SUB", "1", 0);

    uint32_t viOrigin = 0;
    int taskCount = 0;
    {
        FILE *f = fopen((dir + "/vi.txt").c_str(), "r");
        if ((f == nullptr) || (fscanf(f, "%x %d", &viOrigin, &taskCount) != 2)) {
            fprintf(stderr, "missing or bad %s/vi.txt\n", dir.c_str());
            return 1;
        }
        fclose(f);
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_Window *window = SDL_CreateWindow("vg_replay", 0, 0, 960, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (context == nullptr) {
        fprintf(stderr, "GL context: %s\n", SDL_GetError());
        return 1;
    }

    const size_t rdramSize = 32 * 1024 * 1024;
    uint8_t *rdram = static_cast<uint8_t *>(mmap(nullptr, rdramSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    const bool pageMap = (getenv("VG_REPLAY_PAGEMAP") != nullptr);
    vg::Backend backend;
    backend.resScale = scale;
    backend.screenWidth = 960;
    backend.screenHeight = 720;
    // VG_REPLAY_DRY=1: parse only, nothing reaches GL (profiling the parser, like the console bench).
    const bool dry = (getenv("VG_REPLAY_DRY") != nullptr);
    // VG_REPLAY_RESOLVE_THREAD=1 (with DRY): texture loads and hashing on their own thread, as on the Vita.
    vg::Submitter submitter(&backend, false, dry && (getenv("VG_REPLAY_RESOLVE_THREAD") != nullptr));
    if (!submitter.start()) {
        return 1;
    }
    submitter.dryRun = dry;
    // VG_REPLAY_CHECKSUM=1: prints a hash of the last pass's draws (state, vertices, decoded texels) to compare parser changes.
    submitter.checksumDraws = (getenv("VG_REPLAY_CHECKSUM") != nullptr);
    vg::Renderer renderer(&submitter);
    RT64::State state(rdram, &renderer);
    RT64::Interpreter interpreter;
    interpreter.setup(&state);

    // VG_REPLAY_REPEAT=<n> replays the frame n times (for profiling); only the last pass is reported.
    const char *repeatEnv = getenv("VG_REPLAY_REPEAT");
    const int repeat = repeatEnv ? std::max(1, atoi(repeatEnv)) : 1;
    // VG_REPLAY_PROF=1: section timers, 2: opcode timers (same breakdown as the console bench, vita_bench.cpp).
    const char *profEnv = getenv("VG_REPLAY_PROF");
    const int profMode = profEnv ? atoi(profEnv) : 0;
    vg::profInit();
    vg::g_profEnabled = (profMode == 1);
    vg::g_opProfEnabled = (profMode == 2);
    const auto t0 = std::chrono::steady_clock::now();
    // CPU time of this (the parsing) thread: with the resolve thread on, wall time also counts waiting for it.
    auto threadCpuMs = []() {
        timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
    };
    const double cpu0 = threadCpuMs();
    std::vector<VgCaptureHeader> headers(taskCount);
    std::vector<std::vector<uint8_t>> snapshots(taskCount);
    for (int i = 0; i < taskCount; i++) {
        char name[64];
        snprintf(name, sizeof(name), "/task_%02d.bin", i);
        FILE *f = fopen((dir + name).c_str(), "rb");
        VgCaptureHeader &header = headers[i];
        if ((f == nullptr) || (fread(&header, sizeof(header), 1, f) != 1) || (header.magic != 0x32434756) || (header.rdramBytes > rdramSize)) {
            fprintf(stderr, "bad task file %s\n", name);
            return 1;
        }
        snapshots[i].resize(header.rdramBytes);
        const size_t got = fread(snapshots[i].data(), 1, header.rdramBytes, f);
        fclose(f);
        if (got != header.rdramBytes) {
            fprintf(stderr, "short task file %s\n", name);
            return 1;
        }
    }

    for (int pass = 0; pass < repeat; pass++)
    for (int i = 0; i < taskCount; i++) {
        const VgCaptureHeader &header = headers[i];
        if ((pass == 0) || (taskCount > 1)) {
            memcpy(rdram, snapshots[i].data(), header.rdramBytes);
        }

        const bool mapThisTask = pageMap && (pass == repeat - 1);
        if (mapThisTask) {
            g_pageSize = size_t(getpagesize());
            g_pageMapBase = rdram;
            g_pageMapSize = rdramSize;
            g_pagesTouched.assign(rdramSize / g_pageSize, 0);
            struct sigaction sa = {};
            sa.sa_sigaction = pageMapHandler;
            sa.sa_flags = SA_SIGINFO;
            sigaction(SIGSEGV, &sa, nullptr);
            sigaction(SIGBUS, &sa, nullptr);
            mprotect(rdram, rdramSize, PROT_NONE);
        }

        g_current_scene = header.currentScene;
        if ((i == 0) && (pass == repeat - 1)) {
            submitter.drawChecksum = 0;
        }
        state.rsp->reset();
        interpreter.loadUCodeGBI(header.ucode, header.ucodeData, true);
        renderer.beginTask(&state);
        interpreter.processDisplayLists(header.dataPtr, reinterpret_cast<RT64::DisplayList *>(rdram + header.dataPtr));
        renderer.endTask();

        if (mapThisTask) {
            mprotect(rdram, rdramSize, PROT_READ | PROT_WRITE);
            size_t total = 0;
            fprintf(stderr, "task %d pages touched (%zu KB pages), RDRAM ranges:\n", i, g_pageSize / 1024);
            for (size_t p = 0; p < g_pagesTouched.size();) {
                if (!g_pagesTouched[p]) {
                    p++;
                    continue;
                }
                size_t q = p;
                while ((q < g_pagesTouched.size()) && g_pagesTouched[q]) {
                    q++;
                }
                fprintf(stderr, "  0x%06zX-0x%06zX (%zu KB)\n", p * g_pageSize, q * g_pageSize, (q - p) * g_pageSize / 1024);
                total += q - p;
                p = q;
            }
            fprintf(stderr, "  total %zu KB\n", total * g_pageSize / 1024);
        }
        if (pass == repeat - 1) {
            fprintf(stderr, "task %d: dl 0x%06X tris %u rects %u batches %u draws %u skipped %u shaders %u textures %u hashes %u vertices %u (transform reused %u) (last pass)\n", i, header.dataPtr,
                submitter.statTris.load(), renderer.statRects, submitter.statBatches.load(), backend.statDraws, backend.statSkipped, backend.statShaderCompiles, backend.statTexUploads,
                submitter.statHashes.load(), state.rsp->statVertices, state.rsp->statReusedVertices);
        }
        submitter.statHashes = submitter.statDecodes = 0;
        state.rsp->statVertices = state.rsp->statReusedVertices = 0;
        renderer.statRects = 0;
        submitter.statTris = submitter.statBatches = 0;
        backend.statDraws = backend.statSkipped = backend.statShaderCompiles = backend.statTexUploads = 0;
    }

    fprintf(stderr, "replay: %.3f ms per pass (%d passes), parsing thread cpu %.3f ms per pass\n",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / repeat, repeat, (threadCpuMs() - cpu0) / repeat);
    if (submitter.checksumDraws) {
        fprintf(stderr, "draw checksum %016llX\n", (unsigned long long)submitter.drawChecksum);
    }
    for (int i = 0; (profMode == 1) && (i < vg::PROF_COUNT); i++) {
        fprintf(stderr, "  section %-15s %8.3f ms/pass %7.1f calls/pass\n", vg::g_profNames[i],
            vg::profTicksToUs(double(vg::g_profTicks[i])) / repeat / 1000.0, double(vg::g_profCalls[i]) / repeat);
    }
    for (int op = 0; (profMode == 2) && (op < 256); op++) {
        if (vg::g_opCalls[op] != 0) {
            fprintf(stderr, "  op 0x%02X %8.3f ms/pass %7.1f calls/pass\n", op, vg::profTicksToUs(double(vg::g_opTicks[op])) / repeat / 1000.0,
                double(vg::g_opCalls[op]) / repeat);
        }
    }

    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    // The capture's VI origin can still show the previous buffer; fall back to the one these tasks drew.
    if (!backend.readTarget(viOrigin & 0xFFFFFF, rgba, width, height) && !backend.readTarget(backend.lastDrawnAddress, rgba, width, height)) {
        fprintf(stderr, "no color image to read (VI origin 0x%06X)\n", viOrigin & 0xFFFFFF);
        return 1;
    }
    if (!stbi_write_png(argv[2], width, height, 4, rgba.data(), width * 4)) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    fprintf(stderr, "wrote %s (%dx%d)\n", argv[2], width, height);

    // VG_REPLAY_VI="<width> <h_start> <v_start> <x_scale> <y_scale>" (hex VI registers): also present through that VI window onto a
    // 960x544 screen as the console does, written to <out>.screen.png. VG_REPLAY_ASPECT=4:3 selects the pillarbox.
    if (const char *viEnv = getenv("VG_REPLAY_VI")) {
        vg::ViView view;
        // The capture's origin can name the previous buffer: keep its offset (the game's buffers are 64 KB aligned) on the one drawn.
        view.origin = backend.lastDrawnAddress + (viOrigin & 0xFFFF);
        if (sscanf(viEnv, "%x %x %x %x %x", &view.width, &view.hStart, &view.vStart, &view.xScale, &view.yScale) != 5) {
            fprintf(stderr, "bad VG_REPLAY_VI\n");
            return 1;
        }
        const char *aspect = getenv("VG_REPLAY_ASPECT");
        backend.stretchToScreen = !(aspect && (strcmp(aspect, "4:3") == 0));
        backend.screenWidth = 960;
        backend.screenHeight = 544;
        // VG_REPLAY_CONTROLS=1|2|3: the Controller Settings panel (choosing, after an assignment, listening), with a sample layout and an L + R chord.
        std::vector<vg::OverlayItem> overlay;
        if (const char *controlsEnv = getenv("VG_REPLAY_CONTROLS")) {
            const int controlsMode = atoi(controlsEnv);
            vg::ControlsPanelState panelState;
            panelState.mode = (controlsMode == 3) ? vg::ControlsPanelState::Mode::Listen : vg::ControlsPanelState::Mode::Choose;
            panelState.selected = 7;
            if (controlsMode == 2) {
                panelState.message = "SPECIAL = L + R";
            }
            overlay = vg::controlsPanel(rdram, [](uint16_t bit) -> std::string {
                switch (bit) {
                    case 0x8000: return "CROSS";
                    case 0x4000: return "SQUARE";
                    case 0x1000: return "START";
                    case 0x0800: return "UP";
                    case 0x0400: return "DOWN";
                    case 0x0200: return "LEFT";
                    case 0x0100: return "RIGHT";
                    case 0x0020: return "L";
                    case 0x0010: return "R";
                    case 0x0008: return "TRIANGLE";
                    case 0x0004: return "CIRCLE";
                    case 0x0002: return "SELECT";
                    case 0x0001: return "L + R";
                    default: return "";
                }
            }, panelState);
        }
        backend.present(view, overlay, true);
        std::vector<uint8_t> raw(960 * 544 * 4), screen(raw.size());
        glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
        for (int y = 0; y < 544; y++) {
            memcpy(&screen[size_t(y) * 960 * 4], &raw[size_t(543 - y) * 960 * 4], 960 * 4);
        }
        const std::string screenPath = std::string(argv[2]) + ".screen.png";
        stbi_write_png(screenPath.c_str(), 960, 544, 4, screen.data(), 960 * 4);
        fprintf(stderr, "wrote %s\n", screenPath.c_str());
    }
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
