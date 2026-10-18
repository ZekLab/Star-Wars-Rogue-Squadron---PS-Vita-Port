#include "vg_submit.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

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
    // Vectors returned beyond this are freed. Few, and reused most recent first, so the next batch writes into memory still in cache.
    constexpr size_t FreeVertexBuffersKept = 64;
    // Ops handed to the resolve thread at once: fewer wakeups, while it trails the parser by little at the end of a task.
    constexpr size_t PublishOps = 32;
    // Spare vertex vectors taken from the render stage's free list in one lock.
    constexpr size_t SpareRefill = 8;
    // Emptied batch geometries kept for reuse (about two publishes' worth of batches).
    constexpr size_t SpareGeometriesKept = 64;
    static const char *StageNames[] = { "idle", "draw", "present", "swap", "delete textures", "decode" };

    Submitter::Submitter(Backend *backend, bool threaded, bool resolveThreaded) : backend(backend), threaded(threaded), resolveThreaded(resolveThreaded) {
        const char *v = std::getenv("ROGUESQ_VG_HASH_CACHE");
        useHashCache = !(v && (v[0] == '0'));
        // ROGUESQ_VG_RENDER_CORE: user core 0-2 for the render thread, -1 leaves it unpinned.
        const char *core = std::getenv("ROGUESQ_VG_RENDER_CORE");
        if (core != nullptr) {
            renderCore = std::atoi(core);
        }
        const char *resolve = std::getenv("ROGUESQ_VG_RESOLVE_CORE");
        if (resolve != nullptr) {
            resolveCore = std::atoi(resolve);
        }
#ifdef __vita__
        // The resolve thread runs at the priority of the thread that parses (this one).
        resolvePriority = sceKernelGetThreadCurrentPriority();
#endif
    }

    Submitter::~Submitter() {
        if (resolveThread.joinable()) {
            newOp(Op::Type::Stop);
            publish();
            resolveThread.join();
        }
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

    // Parsing thread.

    Submitter::Op &Submitter::newOp(Op::Type type) {
        Op &op = pending.emplace_back();
        op.type = type;
        return op;
    }

    void Submitter::submitOp(bool force) {
        if (!resolveThreaded) {
            processOp(pending.back());
            pending.pop_back();
            return;
        }
        if (!resolveStarted) {
            resolveStarted = true;
            resolveThread = std::thread(&Submitter::resolveMain, this);
        }
        if (force || (pending.size() >= PublishOps)) {
            publish();
        }
    }

    void Submitter::publish() {
        if (pending.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(resolveMutex);
            resolveQueue.push_back(std::move(pending));
            pending = std::vector<Op>();
            if (!freeOpBatches.empty()) {
                pending.swap(freeOpBatches.back());
                freeOpBatches.pop_back();
            }
            while (!freeGeometries.empty() && (spareGeometries.size() < SpareGeometriesKept)) {
                spareGeometries.push_back(std::move(freeGeometries.back()));
                freeGeometries.pop_back();
            }
        }
        resolveReady.signal();
    }

    void Submitter::takeSpareGeometry(BatchGeometry &geometry) {
        if (!spareGeometries.empty()) {
            std::swap(geometry, spareGeometries.back());
            spareGeometries.pop_back();
        }
    }

    void Submitter::load(const TmemLoad &load, const uint8_t *RDRAM) {
        Op &op = newOp(Op::Type::Load);
        op.load = load;
        op.rdram = RDRAM;
        submitOp(false);
    }

    void Submitter::draw(const DrawState &s, BatchGeometry &geometry, bool hard) {
        ProfScope prof(PROF_SUBMIT_DRAW);
        Op &op = newOp(Op::Type::Draw);
        op.state = s;
        op.hard = hard;
        std::swap(op.geometry, geometry);
        takeSpareGeometry(geometry);
        submitOp(false);
    }

    void Submitter::sync() {
        if (!resolveThreaded) {
            return;
        }
        newOp(Op::Type::Sync).signal = &taskDone;
        submitOp(true);
        for (int n = 1; !taskDone.wait(StallLogUs); n++) {
            logStall("sync", n * (StallLogUs / 1e6));
        }
    }

    void Submitter::present(const ViView &view, std::vector<OverlayItem> overlay) {
        // Stall time counted in wait timeouts: std::chrono's clock costs ~5 us per read on the Vita.
        if (threaded) {
            for (int n = 1; !presentSlots.wait(StallLogUs); n++) {
                logStall("present", n * (StallLogUs / 1e6));
            }
        }
        Op &op = newOp(Op::Type::Present);
        op.view = view;
        op.overlay = std::move(overlay);
        submitOp(true);
    }

    void Submitter::endTask() {
        if (!resolveThreaded) {
            finalizePendingDraw();
            return;
        }
        const uint64_t t0 = profNow();
        newOp(Op::Type::TaskEnd).signal = &taskDone;
        submitOp(true);
        for (int n = 1; !taskDone.wait(StallLogUs); n++) {
            logStall("task end", n * (StallLogUs / 1e6));
        }
        statTaskWaitUs.fetch_add(uint64_t(profTicksToUs(double(profNow() - t0))), std::memory_order_relaxed);
    }

    void Submitter::finish() {
        if (!threaded && !resolveThreaded) {
            return;
        }
        newOp(Op::Type::Fence).signal = &finishDone;
        submitOp(true);
        for (int n = 1; !finishDone.wait(StallLogUs); n++) {
            logStall("finish", n * (StallLogUs / 1e6));
        }
    }

    // Resolve stage.

    void Submitter::resolveMain() {
#ifdef __vita__
        if ((resolveCore >= 0) && (resolveCore <= 2)) {
            sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << resolveCore);
        }
        if (resolvePriority > 0) {
            sceKernelChangeThreadPriority(sceKernelGetThreadId(), resolvePriority);
        }
        const int threadId = sceKernelGetThreadId();
        resolveThreadId = threadId;
        fprintf(stderr, "[vg-submit] resolve thread 0x%08X: core %d, priority 0x%X\n", threadId, resolveCore,
            sceKernelGetThreadCurrentPriority());
#endif
        std::vector<std::vector<Op>> batches;
        while (true) {
            resolveReady.wait();
            {
                std::lock_guard<std::mutex> lock(resolveMutex);
                batches.swap(resolveQueue);
            }
            if (batches.empty()) {
                continue;
            }
            const uint64_t t0 = profNow();
            for (std::vector<Op> &ops : batches) {
                for (Op &op : ops) {
                    if (op.type == Op::Type::Stop) {
                        return;
                    }
                    processOp(op);
                }
                ops.clear();
            }
            {
                std::lock_guard<std::mutex> lock(resolveMutex);
                for (std::vector<Op> &ops : batches) {
                    freeOpBatches.push_back(std::move(ops));
                }
                for (BatchGeometry &geometry : usedGeometries) {
                    if (freeGeometries.size() < SpareGeometriesKept) {
                        freeGeometries.push_back(std::move(geometry));
                    }
                }
            }
            batches.clear();
            usedGeometries.clear();
            statResolveUs.fetch_add(uint64_t(profTicksToUs(double(profNow() - t0))), std::memory_order_relaxed);
        }
    }

    void Submitter::processOp(Op &op) {
        // Anything but a draw (or a store sync) ends the batch held back for joining: a load changes TMEM, the others mark frame boundaries.
        if ((op.type != Op::Type::Draw) && (op.type != Op::Type::Sync)) {
            finalizePendingDraw();
        }
        switch (op.type) {
            case Op::Type::Load:
                applyTmemLoad(tmem, op.rdram, op.load);
                tmemTag++;
                break;
            case Op::Type::Draw:
                resolveDraw(op);
                break;
            case Op::Type::Sync:
                op.signal->signal();
                break;
            case Op::Type::Present: {
                Command command;
                command.type = Command::Type::Present;
                command.view = op.view;
                command.overlay = std::move(op.overlay);
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
                break;
            }
            case Op::Type::TaskEnd:
                op.signal->signal();
                break;
            case Op::Type::Fence:
                if (threaded) {
                    Command command;
                    command.type = Command::Type::Fence;
                    command.fence = op.signal;
                    enqueue(std::move(command));
                }
                else {
                    op.signal->signal();
                }
                break;
            case Op::Type::Stop:
                break;
        }
    }

    void Submitter::resolveDraw(Op &op) {
        std::vector<Vertex> vertices;
        takeSpareVertices(vertices);
        const uint32_t drawn = buildBatchVertices(op.state, op.geometry, vertices);
        recycleGeometry(std::move(op.geometry));
        if (drawn == 0) {
            // Every triangle culled: the parser would not have opened this batch, but an explicit flush that ended it still separates its neighbours.
            if (op.hard) {
                mergeOpen = false;
            }
            recycleVertices(std::move(vertices));
            return;
        }

        statTris.fetch_add(drawn, std::memory_order_relaxed);
        if (havePendingDraw && mergeOpen && (memcmp(&pendingDraw.state, &op.state, sizeof(DrawState)) == 0)) {
            pendingDraw.vertices.insert(pendingDraw.vertices.end(), vertices.begin(), vertices.end());
            recycleVertices(std::move(vertices));
        }
        else {
            finalizePendingDraw();
            pendingDraw.type = Command::Type::Draw;
            pendingDraw.state = op.state;
            pendingDraw.vertices = std::move(vertices);
            havePendingDraw = true;
        }
        mergeOpen = !op.hard;
    }

    void Submitter::finalizePendingDraw() {
        if (!havePendingDraw) {
            return;
        }
        havePendingDraw = false;
        mergeOpen = false;
        Command command = std::move(pendingDraw);
        pendingDraw = Command();
        const DrawState &s = command.state;

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
                    resolveTexture(s, i, command);
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
                decodeTile(tmem, s.tiles[i], s.otherModeH & (3U << G_MDSFT_TEXTLUT), t.width, t.height, texels.data());
                drawChecksum = XXH3_64bits_withSeed(texels.data(), texels.size(), drawChecksum ^ (uint64_t(t.width) << 16) ^ t.height);
            }
        }
        statBatches.fetch_add(1, std::memory_order_relaxed);
        enqueue(std::move(command));
    }

    void Submitter::resolveTexture(const DrawState &s, uint32_t index, Command &command) {
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
            hash = hashTile(tmem, tile, tlut, w, h);
            hashCache.push_back({ tile, tlut, w, h, hash });
            statHashes.fetch_add(1, std::memory_order_relaxed);
        }

        TextureRef &out = command.textures[index];
        out.hash = hash;
        out.width = w;
        out.height = h;
        auto it = knownTextures.find(hash);
        if (it == knownTextures.end()) {
            // The render stage decodes from this copy, off the parsing path; the bench never decodes.
            if (!dryRun) {
                Command::Decode &decode = command.decodes[index];
                decode.tile = tile;
                decode.tlut = tlut;
                decode.tmem.assign(tmem, tmem + sizeof(tmem));
            }
            // ROGUESQ_VG_LOG_DECODES=1: size and format of every decoded texture.
            static const bool s_logDecodes = []() { const char *v = std::getenv("ROGUESQ_VG_LOG_DECODES"); return v && (v[0] == '1'); }();
            if (s_logDecodes) {
                fprintf(stderr, "[vg-decode] %ux%u fmt %u siz %u tlut %u masks %u maskt %u cms %u cmt %u uls %u lrs %u ult %u lrt %u\n", w, h,
                    tile.fmt, tile.siz, tlut >> 14, tile.masks, tile.maskt, tile.cms, tile.cmt, tile.uls, tile.lrs, tile.ult, tile.lrt);
            }
            knownTextures.emplace(hash, presentCount);
            statDecodes.fetch_add(1, std::memory_order_relaxed);
            if (evictedHashes.count(hash) != 0) {
                statRedecodes.fetch_add(1, std::memory_order_relaxed);
            }
        }
        else {
            it->second = presentCount;
        }
    }

    // Resolve stage: output vectors come back from the render thread through freeVertexBuffers, or straight here when it runs inline.
    void Submitter::takeSpareVertices(std::vector<Vertex> &vertices) {
        if (spareVertices.empty() && (freeVertexCount.load(std::memory_order_relaxed) > 0)) {
            std::lock_guard<std::mutex> lock(mutex);
            // The most recently drawn vector ends up last, so it is used first.
            const size_t n = std::min(SpareRefill, freeVertexBuffers.size());
            spareVertices.insert(spareVertices.end(), std::make_move_iterator(freeVertexBuffers.end() - n), std::make_move_iterator(freeVertexBuffers.end()));
            freeVertexBuffers.resize(freeVertexBuffers.size() - n);
            freeVertexCount.store(uint32_t(freeVertexBuffers.size()), std::memory_order_relaxed);
        }
        if (!spareVertices.empty()) {
            vertices.swap(spareVertices.back());
            spareVertices.pop_back();
        }
    }

    void Submitter::recycleVertices(std::vector<Vertex> &&vertices) {
        vertices.clear();
        if (spareVertices.size() < FreeVertexBuffersKept) {
            spareVertices.push_back(std::move(vertices));
        }
    }

    void Submitter::recycleGeometry(BatchGeometry &&geometry) {
        geometry.clear();
        std::vector<BatchGeometry> &pool = resolveThreaded ? usedGeometries : spareGeometries;
        if (pool.size() < SpareGeometriesKept) {
            pool.push_back(std::move(geometry));
        }
    }

    // Render stage.

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
                    freeVertexCount.store(uint32_t(freeVertexBuffers.size()), std::memory_order_relaxed);
                }
                drawnVertices = std::vector<Vertex>();
                if (queueCount == 0) {
                    return;
                }
                command = std::move(queue[queueHead]);
                queueHead = (queueHead + 1) & (queue.size() - 1);
                queueCount--;
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
            for (uint32_t i = 0; i < 2; i++) {
                Command::Decode &decode = command.decodes[i];
                if (decode.tmem.empty()) {
                    continue;
                }
                stage = StageDecode;
                ProfScope prof(PROF_TEX_DECODE);
                TextureRef &t = command.textures[i];
                t.pixels.resize(size_t(t.width) * t.height * 4);
                decodeTile(decode.tmem.data(), decode.tile, decode.tlut, t.width, t.height, t.pixels.data());
            }
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

    void Submitter::enqueue(Command &&command) {
        if (!threaded) {
            if (!dryRun) {
                execute(command);
            }
            if (command.type == Command::Type::Draw) {
                recycleVertices(std::move(command.vertices));
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (queueCount == queue.size()) {
                std::vector<Command> grown(queue.size() * 2);
                for (size_t i = 0; i < queueCount; i++) {
                    grown[i] = std::move(queue[(queueHead + i) & (queue.size() - 1)]);
                }
                queue.swap(grown);
                queueHead = 0;
            }
            queue[(queueHead + queueCount) & (queue.size() - 1)] = std::move(command);
            queueCount++;
        }
        commandsReady.signal();
    }

    void Submitter::logStall(const char *where, double seconds) {
        size_t queued = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            queued = queueCount;
        }
        const uint32_t st = stage.load();
        fprintf(stderr, "[vg-submit] STALL in %s for %.1f s: render stage=%s, executed=%llu, queue=%u, free present slots=%u\n",
            where, seconds, (st < 6) ? StageNames[st] : "?", (unsigned long long)executed.load(), unsigned(queued),
            unsigned(presentSlots.availableApprox()));
#ifdef __vita__
        const int uids[3] = { renderThreadId, resolveThreadId.load(), sceKernelGetThreadId() };
        const char *labels[3] = { "render", "resolve", "waiting" };
        for (int i = 0; i < 3; i++) {
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
}
