// Hands batches from the display-list parser to the GL backend in two stages. The resolve stage keeps TMEM: it applies the
// parser's texture loads in order with its draws and resolves each draw's textures to a content hash, plus a copy of TMEM to
// decode from the first time a hash is seen. The render stage decodes those textures and runs the GL backend.
// On the Vita each stage has its own thread, so loads, hashing, decoding and vitaGL's per-draw cost overlap with parsing; the
// Mac replay tool runs both inline.

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Kernel semaphores on the Vita: std::condition_variable (pthread-embedded) lost wakeups there and hung the render thread.
#ifndef MOODYCAMEL_DELETE_FUNCTION
#define MOODYCAMEL_DELETE_FUNCTION = delete
#endif
#include "concurrentqueue/lightweightsemaphore.h"

#include "vg_backend_gl.h"
#include "vg_renderer.h"
#include "vg_shader.h"

namespace vg {
    // Fields written by different threads start on their own cache line (128 bytes on Apple silicon, 32 on the Vita).
    constexpr size_t CacheLine = 128;

    class Submitter {
    public:
        // threaded: GL backend on its own thread. resolveThreaded: loads and texture hashing on another one.
        Submitter(Backend *backend, bool threaded, bool resolveThreaded = false);
        ~Submitter();

        // Initializes the backend (on the render thread when threaded); false when GL init failed.
        bool start();

        // Queues a texture load. RDRAM is read when the resolve stage applies it, at the latest before endTask returns.
        void load(const TmemLoad &load, const uint8_t *RDRAM);
        // Takes the batch geometry, which comes back empty (recycled from an earlier batch when one is free). hard: an explicit flush ended
        // the batch, not a state change (Renderer::flushBatch).
        void draw(const DrawState &s, BatchGeometry &geometry, bool hard);
        // Returns once the resolve stage has finished every batch queued so far, so the parser may move the vertex store.
        void sync();
        // Queues a present; blocks while two presents are already pending so parsing stays at most two frames ahead.
        void present(const ViView &view, std::vector<OverlayItem> overlay = {});
        // Returns once every queued load has been applied, so the game may change RDRAM again.
        void endTask();
        // Waits until every queued command has run.
        void finish();

        // Statistics, reset by the reporter.
        alignas(CacheLine) std::atomic<uint32_t> statHashes{ 0 };
        std::atomic<uint32_t> statDecodes{ 0 }, statRedecodes{ 0 };
        // Triangles drawn after culling and clipping, and the batches they went out in.
        std::atomic<uint32_t> statTris{ 0 }, statBatches{ 0 };
        // Microseconds the parsing thread waited in endTask, and the resolve thread's busy time.
        std::atomic<uint64_t> statTaskWaitUs{ 0 }, statResolveUs{ 0 };
        // Kernel ids of the render and resolve threads (Vita), for the CPU census; -1 when not threaded.
        int renderThreadId = -1;
        std::atomic<int> resolveThreadId{ -1 };
        // User core 0-2 for the resolve thread, -1 unpinned (ROGUESQ_VG_RESOLVE_CORE); set before the first op.
        int resolveCore = -1;
        // Parser bench (vita_bench.cpp): textures are still resolved, but nothing reaches the backend (render stage inline only).
        bool dryRun = false;
        // vg_replay regression check: every draw's state, vertices and decoded texels folded into one hash, valid after endTask.
        bool checksumDraws = false;
        uint64_t drawChecksum = 0;

    private:
        struct Command {
            enum class Type { Draw, Present, Fence } type = Type::Draw;
            DrawState state;
            std::vector<Vertex> vertices;
            TextureRef textures[2];
            // Textures seen for the first time: the tile and the TMEM the draw sampled, decoded by the render stage.
            struct Decode {
                TileState tile;
                uint32_t tlut = 0;
                std::vector<uint8_t> tmem;
            } decodes[2];
            ViView view;
            std::vector<OverlayItem> overlay;
            std::vector<uint64_t> deletedTextures;
            // Fence: signaled by the render thread once every earlier command has run.
            moodycamel::LightweightSemaphore *fence = nullptr;
        };

        // Parsing thread -> resolve stage.
        struct Op {
            enum class Type : uint8_t { Load, Draw, Present, TaskEnd, Sync, Fence, Stop } type = Type::Draw;
            TmemLoad load;
            const uint8_t *rdram = nullptr;
            DrawState state;
            BatchGeometry geometry;
            bool hard = false;
            ViView view;
            std::vector<OverlayItem> overlay;
            moodycamel::LightweightSemaphore *signal = nullptr;
        };

        struct HashCacheEntry {
            TileState tile;
            uint32_t tlut;
            uint16_t width, height;
            uint64_t hash;
        };

        // Parsing thread.
        Op &newOp(Op::Type type);
        // Runs the newest op inline, or publishes the pending ops to the resolve thread once enough have gathered (or when forced).
        void submitOp(bool force);
        void publish();
        void takeSpareGeometry(BatchGeometry &geometry);

        // Resolve stage.
        void resolveMain();
        void processOp(Op &op);
        void resolveDraw(Op &op);
        // Sends the held-back batch on: textures resolved against the TMEM it was drawn with, then queued for the render stage.
        void finalizePendingDraw();
        void resolveTexture(const DrawState &s, uint32_t index, Command &command);
        void takeSpareVertices(std::vector<Vertex> &vertices);
        void recycleVertices(std::vector<Vertex> &&vertices);
        void recycleGeometry(BatchGeometry &&geometry);

        // Render stage.
        void enqueue(Command &&command);
        void execute(Command &command);
        void threadMain();
        // Logs the render thread's state when the parsing thread has waited too long for it (mutex not held).
        void logStall(const char *where, double seconds);

        Backend *backend;
        bool threaded;
        bool resolveThreaded;
        bool useHashCache = true;

        // Parsing thread: ops not yet published, and emptied batch geometries for the next batches.
        alignas(CacheLine) std::vector<Op> pending;
        std::vector<BatchGeometry> spareGeometries;

        // Resolve stage: TMEM as of the op being processed, bumped tag per load, and texture bookkeeping (hash -> present count when last used).
        alignas(CacheLine) uint8_t tmem[4096] = {};
        uint32_t tmemTag = 0;
        std::unordered_map<uint64_t, uint64_t> knownTextures;
        std::unordered_set<uint64_t> evictedHashes;
        std::deque<uint64_t> evictedOrder;
        std::vector<HashCacheEntry> hashCache;
        uint32_t hashCacheTag = 0xFFFFFFFF;
        uint64_t presentCount = 0;
        // Texture use of the last combiner seen; consecutive batches mostly share it.
        ShaderKey lastKey = { 0, 0, 0, 0, 0xFFFFFFFF };
        bool lastKeyUsesTexture[2] = { false, false };
        // The last batch, held back so a following batch with the same state can join it (mergeOpen: nothing but culled batches and
        // state changes came between them, as when the parser did not split it at all).
        Command pendingDraw;
        bool havePendingDraw = false;
        bool mergeOpen = false;
        // Empty vertex vectors for batch output, and geometries done with, handed back to the parsing thread at the next publish.
        std::vector<std::vector<Vertex>> spareVertices;
        std::vector<BatchGeometry> usedGeometries;

        alignas(CacheLine) std::thread resolveThread;
        // Guards the published op batches and the emptied ones that go back to the parsing thread with their capacity.
        std::mutex resolveMutex;
        std::vector<std::vector<Op>> resolveQueue;
        std::vector<std::vector<Op>> freeOpBatches;
        std::vector<BatchGeometry> freeGeometries;
        // One signal per publish; the resolve thread drains whatever has been published.
        moodycamel::LightweightSemaphore resolveReady{ 0, 200 };
        moodycamel::LightweightSemaphore taskDone{ 0 };
        moodycamel::LightweightSemaphore finishDone{ 0 };
        int resolvePriority = -1;
        // Started with the first op, so tools that never call start() (the bench) get it too.
        bool resolveStarted = false;

        alignas(CacheLine) std::thread thread;
        // Guards the queue and the free vertex vectors; waiting is done on the semaphores.
        std::mutex mutex;
        // Commands for the render thread: a ring (power-of-two size, grown when full), so queueing allocates nothing.
        std::vector<Command> queue = std::vector<Command>(1024);
        size_t queueHead = 0, queueCount = 0;
        // Vertex vectors the render stage has drawn, kept with their capacity for later batches; the count is readable without the lock.
        std::vector<std::vector<Vertex>> freeVertexBuffers;
        std::atomic<uint32_t> freeVertexCount{ 0 };
        // One signal per queued command; one extra with an empty queue stops the thread.
        moodycamel::LightweightSemaphore commandsReady{ 0 };
        // Presents the parser may queue ahead of the render thread.
        moodycamel::LightweightSemaphore presentSlots{ 2 };
        moodycamel::LightweightSemaphore initSignal{ 0 };
        bool initResult = false;

        // Watchdog: what the render thread is doing (Stage) and how many commands it has finished.
        enum Stage : uint32_t { StageIdle, StageDraw, StagePresent, StageSwap, StageDeleteTextures, StageDecode };
        std::atomic<uint32_t> stage{ StageIdle };
        std::atomic<uint64_t> executed{ 0 };
        int renderCore = 2;
    };
}
