// GL backend of the Vita renderer: vitaGL on the Vita, legacy desktop GL (GLSL 1.20) in the Mac harness.
// Each N64 color image address gets its own FBO; the VI origin picks which one is presented.
// Every method must be called from the thread that ran init() (vitaGL is not thread safe); see vg_submit.h.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "vg_renderer.h"
#include "vg_shader.h"

namespace vg {
    // A texture resolved on the parsing thread: its content hash, size, and the decoded RGBA8 texels when the backend
    // has not seen this hash yet (empty otherwise).
    struct TextureRef {
        uint64_t hash = 0;
        uint16_t width = 0, height = 0;
        std::vector<uint8_t> pixels;
    };

    // VI registers that choose the framebuffer window reaching the screen; a zero width shows the whole color image.
    struct ViView {
        uint32_t origin = 0;
        uint32_t width = 0, hStart = 0, vStart = 0, xScale = 0, yScale = 0;
    };

    // A panel or a line of text drawn over the presented image (the controller-settings panel). Positions and sizes are fractions of
    // the shown image; text height is the glyph cell height, and text starts, ends or is centered at x per align. A panel's radius
    // rounds its corners, as a fraction of the image height.
    struct OverlayItem {
        enum class Kind : uint8_t { Panel, Text };
        enum class Align : uint8_t { Left, Right, Center };
        Kind kind = Kind::Text;
        Align align = Align::Left;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        float radius = 0.0f;
        uint32_t color = 0xFFFFFFFF;
        std::string text;
    };

    class Backend {
    public:
        Backend();

        // The GL context must be current on the calling thread (Vita: creates it through vitaGL).
        bool init();

        // textures[i] is used when the combiner reads TEXEL i (shaderUsesTexture).
        void draw(const DrawState &s, const Vertex *vertices, uint32_t vertexCount, const TextureRef textures[2]);

        // Releases GL textures the parsing thread no longer tracks.
        void deleteTextures(const std::vector<uint64_t> &hashes);

        // Shows the VI window of the color image containing the VI origin (black while the VI blanks); returns false when nothing changed.
        bool present(const ViView &vi, const std::vector<OverlayItem> &overlay, bool force);

        // Reads back the color image containing the VI origin, top row first (replay tool).
        bool readTarget(uint32_t viOrigin, std::vector<uint8_t> &rgba, int &width, int &height);

        uint32_t lastDrawnAddress = 0;

        // Swaps the window; set by the host context (desktop harness) or vitaGL on the Vita.
        std::function<void()> swapCallback;
        // Output size of the window/screen.
        int screenWidth = 960;
        int screenHeight = 544;
        // Render resolution multiplier over the N64 color image size.
        int resScale = 2;
        // True: the 4:3 image is stretched over the whole screen; false: 4:3 pillarbox.
        bool stretchToScreen = false;
        // Toggle for the GL state cache (ROGUESQ_VG_GL_STATE_CACHE; default on).
        bool useStateCache = true;

        // Statistics, reset by the reporter.
        uint32_t statDraws = 0, statTexUploads = 0, statShaderCompiles = 0, statShaderCacheHits = 0, statSkipped = 0, statPresents = 0;
        // Presents whose image got no draws since it was last shown (stale), or under a quarter of the usual count (low).
        uint32_t statStalePresents = 0, statLowPresents = 0;
        // Free vitaGL VRAM in KB (0 off the Vita).
        static uint32_t freeVramKB();

    private:
        struct Target {
            uint32_t address = 0;
            uint16_t width = 0, height = 0;
            uint32_t fbo = 0, color = 0, depth = 0;
            bool drawnSincePresent = false;
            uint32_t drawsSincePresent = 0;
            uint64_t lastUse = 0;
            // The N64 has one depth buffer; each FBO has its own, so a depth clear is applied when the target is next bound.
            bool depthClearPending = false;
            // Binds since the last present; each one starts a GXM scene and render targets allow a fixed number per frame.
            uint32_t scenesThisFrame = 0;
        };

        static constexpr uint32_t MaxScenesPerFrame = 4;
        // Running average of draws per presented image, for statLowPresents.
        float avgPresentDraws = 0.0f;

        struct Program {
            uint32_t program = 0;
            int locPrim = -1, locEnv = -1, locFog = -1, locBlend = -1, locFill = -1, locMisc = -1;
            int locTile[2] = { -1, -1 }, locTexSize[2] = { -1, -1 }, locTex[2] = { -1, -1 };
            bool usesTexture[2] = { false, false };
            bool alphaBlend = false;
        };

        struct CachedTexture {
            uint32_t texture = 0;
            uint16_t width = 0, height = 0;
            // Sampler state last set on the GL texture (-1 = never).
            int wrapS = -1, wrapT = -1, filter = -1;
        };

        // Last GL state set by draw(); -1 = unknown. Reset whenever other code paths touch the state.
        struct GLStateCache {
            uint32_t program = 0;
            int scissorTest = -1, depthTest = -1, depthFunc = -1, depthMask = -1, polygonOffset = -1, blend = -1, cullFace = -1;
            int scissor[4] = { -1, -1, -1, -1 };
            bool polygonOffsetSet = false, blendFuncSet = false, attribsEnabled = false;
        };

        // Finds or creates the target for a color image; one shorter than `height` is recreated.
        Target *targetFor(uint32_t address, uint16_t width, uint16_t height, uint8_t siz);
        void releaseTarget(Target *t);
        uint32_t depthBufferFor(uint16_t width, uint16_t height);
        bool bindTarget(Target *target);
        Program *programFor(const ShaderKey &key);
        CachedTexture *textureFor(const TextureRef &ref);
        uint32_t compile(uint32_t type, const std::string &source);
        uint32_t link(uint32_t vs, uint32_t fs, bool withColor);
        void evictTargets();
        // The target holding a VI origin (nullptr when none does).
        Target *targetForOrigin(uint32_t viOrigin);
        static void setEnabled(uint32_t cap, bool enabled, int &cached);

        std::vector<std::unique_ptr<Target>> targets;
        std::unordered_map<uint32_t, uint32_t> depthBuffers;
        std::unordered_map<ShaderKey, Program, ShaderKeyHash> programs;
        std::unordered_map<uint64_t, CachedTexture> textures;
        GLStateCache glc;
        Target *boundTarget = nullptr;
        uint32_t presentProgram = 0;
        uint32_t overlayProgram = 0;
        uint32_t fontTexture = 0;
        uint32_t lastPresentedAddress = 0;
        size_t lastOverlayHash = 0;
        // Draws the overlay inside the image's destination rectangle (clip space) of the current viewport.
        void drawOverlay(const std::vector<OverlayItem> &overlay, float x0, float x1, float yTop, float yBottom, int viewW, int viewH);
        ViView lastPresentedView;
        bool lastPresentedBlank = false;
        uint64_t frameCounter = 0;
        bool cg = false;
    };
}
