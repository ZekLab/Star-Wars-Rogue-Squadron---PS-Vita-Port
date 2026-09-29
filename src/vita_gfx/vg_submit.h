// Hands batches from the display-list parser to the GL backend. On the Vita the backend runs on its own thread (and CPU
// core), so vitaGL's per-draw cost overlaps with parsing the next commands; the Mac replay tool runs it inline.
// Textures are resolved here, on the parsing thread, because TMEM changes as parsing continues: each draw carries the
// content hash of its textures, plus decoded texels the first time a hash is seen.

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

namespace vg {
    class Submitter {
    public:
        Submitter(Backend *backend, bool threaded);
        ~Submitter();

        // Initializes the backend (on the render thread when threaded); false when GL init failed.
        bool start();

        // Copies the batch vertices and empties the vector (its capacity is kept).
        void draw(const DrawState &s, std::vector<Vertex> &vertices, const uint8_t *TMEM, uint32_t tmemTag);
        // Queues a present; blocks while two presents are already pending so parsing stays at most two frames ahead.
        void present(const ViView &view, std::vector<OverlayItem> overlay = {});
        // Waits until every queued command has run.
        void finish();

        // Parsing-thread statistics, reset by the reporter.
        uint32_t statHashes = 0, statDecodes = 0, statRedecodes = 0;
        // Kernel id of the render thread (Vita), for the CPU census; -1 when not threaded.
        int renderThreadId = -1;
        // Parser bench (vita_bench.cpp): textures are still resolved, but nothing reaches the backend (unthreaded only).
        bool dryRun = false;

    private:
        struct Command {
            enum class Type { Draw, Present, Fence } type = Type::Draw;
            DrawState state;
            std::vector<Vertex> vertices;
            TextureRef textures[2];
            ViView view;
            std::vector<OverlayItem> overlay;
            std::vector<uint64_t> deletedTextures;
            // Fence: signaled by the render thread once every earlier command has run.
            moodycamel::LightweightSemaphore *fence = nullptr;
        };

        struct HashCacheEntry {
            TileState tile;
            uint32_t tlut;
            uint16_t width, height;
            uint64_t hash;
        };

        void resolveTexture(const DrawState &s, uint32_t index, const uint8_t *TMEM, uint32_t tmemTag, TextureRef &out);
        void enqueue(Command &&command);
        void execute(Command &command);
        void threadMain();
        // Logs the render thread's state when the parsing thread has waited too long for it (mutex not held).
        void logStall(const char *where, double seconds);

        Backend *backend;
        bool threaded;
        bool useHashCache = true;

        // Parsing-thread texture bookkeeping: hash -> present count when last used.
        std::unordered_map<uint64_t, uint64_t> knownTextures;
        std::unordered_set<uint64_t> evictedHashes;
        std::deque<uint64_t> evictedOrder;
        std::vector<HashCacheEntry> hashCache;
        uint32_t hashCacheTag = 0xFFFFFFFF;
        uint64_t presentCount = 0;

        std::thread thread;
        // Guards the queue only; waiting is done on the semaphores.
        std::mutex mutex;
        std::deque<Command> queue;
        // One signal per queued command; one extra with an empty queue stops the thread.
        moodycamel::LightweightSemaphore commandsReady{ 0 };
        // Presents the parser may queue ahead of the render thread.
        moodycamel::LightweightSemaphore presentSlots{ 2 };
        moodycamel::LightweightSemaphore initSignal{ 0 };
        bool initResult = false;

        // Watchdog: what the render thread is doing (Stage) and how many commands it has finished.
        enum Stage : uint32_t { StageIdle, StageDraw, StagePresent, StageSwap, StageDeleteTextures };
        std::atomic<uint32_t> stage{ StageIdle };
        std::atomic<uint64_t> executed{ 0 };
        int renderCore = 2;
    };
}
