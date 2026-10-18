#include "vg_submit.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __vita__
#   include <psp2/io/fcntl.h>
#   include <psp2/kernel/cpu.h>
#   include <psp2/kernel/threadmgr.h>
#endif

#include "shared/rt64_f3d_defines.h"

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include "vg_prof.h"
#include "vg_shader.h"
#include "vg_texture.h"

namespace vg {
    // Textures unused for this many presents (~5 s) are released on both sides; a decode costs up to a millisecond on the Vita,
    // and explosions bring back the same flipbook frames every few seconds. Idle textures are looked for every SweepPresents.
    constexpr uint64_t TextureIdlePresents = 150;
    constexpr uint64_t SweepPresents = 30;
    // Released hashes remembered to count re-decodes (statRedecodes).
    constexpr size_t EvictedHashesKept = 4096;
    // The parsing thread logs the render thread's state after waiting this long for it.
    constexpr std::int64_t StallLogUs = 2000000;
    // About two frames of batches; vectors returned beyond this are freed.
    constexpr size_t FreeVertexBuffersKept = 1024;
    static const char *StageNames[] = { "idle", "draw", "present", "swap", "delete textures" };

    Submitter::Submitter(Backend *backend, bool threaded) : backend(backend), threaded(threaded) {
        const char *v = std::getenv("ROGUESQ_VG_HASH_CACHE");
        useHashCache = !(v && (v[0] == '0'));
        // ROGUESQ_VG_RENDER_CORE: user core 0-2 for the render thread, -1 leaves it unpinned.
        const char *core = std::getenv("ROGUESQ_VG_RENDER_CORE");
        if (core != nullptr) {
            renderCore = std::atoi(core);
        }
    }

    Submitter::~Submitter() {
        if (thread.joinable()) {
            commandsReady.signal();
            thread.join();
        }
    }

    bool Submitter::start() {
        if (!threaded) {
            return backend->init();
        }

        if (backend->swapCallback) {
            auto swap = backend->swapCallback;
            backend->swapCallback = [this, swap]() {
                stage = StageSwap;
                swap();
                stage = StagePresent;
            };
        }
        thread = std::thread(&Submitter::threadMain, this);
        initSignal.wait();
        return initResult;
    }

    void Submitter::threadMain() {
#ifdef __vita__
        // Core 2: the game and its gfx thread (the parser) run on the other two.
        if ((renderCore >= 0) && (renderCore <= 2)) {
            sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << renderCore);
        }
        renderThreadId = sceKernelGetThreadId();
        fprintf(stderr, "[vg-submit] render thread 0x%08X: core %d, priority 0x%X\n", renderThreadId, renderCore,
            sceKernelGetThreadCurrentPriority());
#endif
        initResult = backend->init();
        initSignal.signal();

        // The last draw's vertices go back to the free list under the same lock that takes the next command.
        std::vector<Vertex> drawnVertices;
        while (true) {
            commandsReady.wait();
            Command command;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if ((drawnVertices.capacity() > 0) && (freeVertexBuffers.size() < FreeVertexBuffersKept)) {
                    drawnVertices.clear();
                    freeVertexBuffers.push_back(std::move(drawnVertices));
                }
                drawnVertices = std::vector<Vertex>();
                if (queue.empty()) {
                    return;
                }
                command = std::move(queue.front());
                queue.pop_front();
            }

            execute(command);
            stage = StageIdle;
            executed++;

            if (command.type == Command::Type::Draw) {
                drawnVertices = std::move(command.vertices);
            }
            else if (command.type == Command::Type::Present) {
                presentSlots.signal();
            }
            else if (command.type == Command::Type::Fence) {
                command.fence->signal();
            }
        }
    }

    void Submitter::execute(Command &command) {
        if (command.type == Command::Type::Draw) {
            stage = StageDraw;
            backend->draw(command.state, command.vertices.data(), uint32_t(command.vertices.size()), command.textures);
        }
        else if (command.type == Command::Type::Present) {
            stage = StageDeleteTextures;
            backend->deleteTextures(command.deletedTextures);
            stage = StagePresent;
            backend->present(command.view, command.overlay, false);
        }
    }

    void Submitter::enqueue(Command &&command, std::vector<Vertex> *spare) {
        if (!threaded) {
            if (!dryRun) {
                execute(command);
            }
            if (spare != nullptr) {
                command.vertices.clear();
                spare->swap(command.vertices);
            }
            return;
        }

        if (command.type == Command::Type::Present) {
            const auto t0 = std::chrono::steady_clock::now();
            while (!presentSlots.wait(StallLogUs)) {
                logStall("present", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push_back(std::move(command));
            if ((spare != nullptr) && !freeVertexBuffers.empty()) {
                spare->swap(freeVertexBuffers.back());
                freeVertexBuffers.pop_back();
            }
        }
        commandsReady.signal();
    }

    void Submitter::finish() {
        if (!threaded) {
            return;
        }
        moodycamel::LightweightSemaphore done;
        Command command;
        command.type = Command::Type::Fence;
        command.fence = &done;
        enqueue(std::move(command));
        const auto t0 = std::chrono::steady_clock::now();
        while (!done.wait(StallLogUs)) {
            logStall("finish", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
    }

    void Submitter::logStall(const char *where, double seconds) {
        size_t queued = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            queued = queue.size();
        }
        const uint32_t st = stage.load();
        fprintf(stderr, "[vg-submit] STALL in %s for %.1f s: render stage=%s, executed=%llu, queue=%u, free present slots=%u\n",
            where, seconds, (st < 5) ? StageNames[st] : "?", (unsigned long long)executed.load(), unsigned(queued),
            unsigned(presentSlots.availableApprox()));
#ifdef __vita__
        const int uids[2] = { renderThreadId, sceKernelGetThreadId() };
        const char *labels[2] = { "render", "waiting" };
        for (int i = 0; i < 2; i++) {
            SceKernelThreadInfo info{};
            info.size = sizeof(info);
            if ((uids[i] < 0) || (sceKernelGetThreadInfo(uids[i], &info) < 0)) {
                continue;
            }
            fprintf(stderr, "[vg-submit]   %s thread \"%s\": status=0x%X waitType=0x%X prio=0x%X affinity=0x%X cpu=%d last cpu=%d run=%llu us\n",
                labels[i], info.name, info.status, info.waitType, info.currentPriority, info.currentCpuAffinityMask,
                info.currentCpuId, info.lastExecutedCpuId, (unsigned long long)info.runClocks);
        }
        fflush(stderr);
        sceIoSync("ux0:", 0);
#endif
    }

    void Submitter::resolveTexture(const DrawState &s, uint32_t index, const uint8_t *TMEM, uint32_t tmemTag, TextureRef &out) {
        const TileState &tile = s.tiles[index];
        const uint32_t tlut = s.otherModeH & (3U << G_MDSFT_TEXTLUT);
        uint16_t w = 1, h = 1;
        tileSampleSize(tile, s.textureOn != 0, w, h);

        // TMEM only changes on a load (tmemTag advances), so hashes computed since the last load are reused.
        if ((hashCacheTag != tmemTag) || !useHashCache) {
            hashCache.clear();
            hashCacheTag = tmemTag;
        }
        uint64_t hash = 0;
        bool cached = false;
        for (const HashCacheEntry &e : hashCache) {
            if ((e.tlut == tlut) && (e.width == w) && (e.height == h) && (memcmp(&e.tile, &tile, sizeof(TileState)) == 0)) {
                hash = e.hash;
                cached = true;
                break;
            }
        }
        if (!cached) {
            ProfScope prof(PROF_TEX_HASH);
            hash = hashTile(TMEM, tile, tlut, w, h);
            hashCache.push_back({ tile, tlut, w, h, hash });
            statHashes++;
        }

        out.hash = hash;
        out.width = w;
        out.height = h;
        auto it = knownTextures.find(hash);
        if (it == knownTextures.end()) {
            ProfScope prof(PROF_TEX_DECODE);
            out.pixels.resize(size_t(w) * h * 4);
            decodeTile(TMEM, tile, tlut, w, h, out.pixels.data());
            // ROGUESQ_VG_LOG_DECODES=1: size and format of every decoded texture.
            static const bool s_logDecodes = []() { const char *v = std::getenv("ROGUESQ_VG_LOG_DECODES"); return v && (v[0] == '1'); }();
            if (s_logDecodes) {
                fprintf(stderr, "[vg-decode] %ux%u fmt %u siz %u tlut %u masks %u maskt %u cms %u cmt %u uls %u lrs %u ult %u lrt %u\n", w, h,
                    tile.fmt, tile.siz, tlut >> 14, tile.masks, tile.maskt, tile.cms, tile.cmt, tile.uls, tile.lrs, tile.ult, tile.lrt);
            }
            knownTextures.emplace(hash, presentCount);
            statDecodes++;
            if (evictedHashes.count(hash) != 0) {
                statRedecodes++;
            }
        }
        else {
            it->second = presentCount;
        }
    }

    void Submitter::draw(const DrawState &s, std::vector<Vertex> &vertices, const uint8_t *TMEM, uint32_t tmemTag) {
        ProfScope prof(PROF_SUBMIT_DRAW);
        Command command;
        command.type = Command::Type::Draw;
        command.state = s;
        command.vertices.swap(vertices);

        // Draws into the depth image are handled without textures (depth clears) or skipped by the backend.
        if (s.colorImage != s.depthImage) {
            const ShaderKey key = makeShaderKey(s);
            if (!(key == lastKey)) {
                lastKey = key;
                lastKeyUsesTexture[0] = shaderUsesTexture(key, 0);
                lastKeyUsesTexture[1] = shaderUsesTexture(key, 1);
            }
            for (uint32_t i = 0; i < 2; i++) {
                if (lastKeyUsesTexture[i]) {
                    resolveTexture(s, i, TMEM, tmemTag, command.textures[i]);
                }
            }
        }
        if (checksumDraws) {
            drawChecksum = XXH3_64bits_withSeed(&s, sizeof(s), drawChecksum);
            drawChecksum = XXH3_64bits_withSeed(command.vertices.data(), command.vertices.size() * sizeof(Vertex), drawChecksum);
            for (uint32_t i = 0; i < 2; i++) {
                const TextureRef &t = command.textures[i];
                if (t.width == 0) {
                    continue;
                }
                std::vector<uint8_t> texels(size_t(t.width) * t.height * 4);
                decodeTile(TMEM, s.tiles[i], s.otherModeH & (3U << G_MDSFT_TEXTLUT), t.width, t.height, texels.data());
                drawChecksum = XXH3_64bits_withSeed(texels.data(), texels.size(), drawChecksum ^ (uint64_t(t.width) << 16) ^ t.height);
            }
        }
        enqueue(std::move(command), &vertices);
    }

    void Submitter::present(const ViView &view, std::vector<OverlayItem> overlay) {
        Command command;
        command.type = Command::Type::Present;
        command.view = view;
        command.overlay = std::move(overlay);
        presentCount++;
        if ((presentCount % SweepPresents) == 0) {
            for (auto it = knownTextures.begin(); it != knownTextures.end();) {
                if ((presentCount - it->second) > TextureIdlePresents) {
                    command.deletedTextures.push_back(it->first);
                    if (evictedHashes.insert(it->first).second) {
                        evictedOrder.push_back(it->first);
                        if (evictedOrder.size() > EvictedHashesKept) {
                            evictedHashes.erase(evictedOrder.front());
                            evictedOrder.pop_front();
                        }
                    }
                    it = knownTextures.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        enqueue(std::move(command));
    }
}
