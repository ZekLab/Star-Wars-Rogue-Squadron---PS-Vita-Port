// Vita renderer core: collects the triangles and rects RT64's Factor 5 parser emits (through the RSP/RDP shim) into
// batches that share one RDP state, and hands each batch to the submitter (vg_submit.h), which feeds the GL backend.

#pragma once

#include <cstdint>
#include <vector>

namespace RT64 {
    struct State;
}

namespace vg {
    // Processed vertex: x, y in color-image pixels, z in 0..1, w clip-space; shade RGBA (lighting/fog applied) and
    // texcoords in texel units before the tile shift. After batching x, y, z hold clip-space coordinates.
    struct Vertex {
        float x, y, z, w;
        float r, g, b, a;
        float s, t;
    };

    // Screen-space rectangle in color-image pixels (edges already rounded as the RDP does).
    struct RectDraw {
        float x0, y0, x1, y1;
        float u0, v0, u1, v1;
        bool flip;
        bool textured;
        uint8_t tile;
    };

    struct TileState {
        uint8_t fmt, siz, palette, cms, cmt, masks, maskt, shifts, shiftt;
        uint16_t line, tmem, uls, ult, lrs, lrt;
    };

    enum class DrawKind : uint8_t {
        Triangles,
        Rect
    };

    // An RDP texture load (LOADTILE, LOADBLOCK or LOADTLUT) with the tile and texture-image state it reads.
    struct TmemLoad {
        enum class Kind : uint8_t { Tile, Block, TLUT } kind;
        uint8_t tileFmt, tileSiz, textureSiz;
        uint16_t line, tmem, uls, ult, lrs, lrt, textureWidth;
        uint32_t textureAddress;
    };

    // Applies a load to a 4 KB TMEM image, reading the texels from RDRAM (vg_rdp.cpp).
    void applyTmemLoad(uint8_t *TMEM, const uint8_t *RDRAM, const TmemLoad &load);

    // Everything a batch is drawn with. Compared with memcmp, so always build it zeroed (captureState does).
    struct DrawState {
        uint32_t otherModeH, otherModeL;
        uint32_t combineL, combineH;
        uint32_t geometryMode;
        float prim[4], env[4], fog[4], blend[4];
        float primLodFrac;
        float primDepth;
        float k4, k5;
        uint32_t fillColor;
        uint32_t colorImage, depthImage;
        uint16_t colorImageWidth, colorImageHeight;
        uint8_t colorImageSiz;
        uint8_t textureOn, textureTile, textureLevels;
        DrawKind kind;
        TileState tiles[2];
        // Scissor in color-image pixels, top-left origin.
        int32_t scissorX0, scissorY0, scissorX1, scissorY1;
    };

    // A triangle of a batch: three vertex indices, culled, clipped and turned into GL vertices by the resolve stage (buildBatchVertices).
    struct TriRef {
        // Direct: indices into the batch's own vertices (rects, RDP triangles), drawn without culling or clipping.
        static constexpr uint32_t Direct = 1;
        uint32_t a, b, c;
        // Direct, plus the index of the viewport clipped vertices use (BatchGeometry::viewports) from bit 8 up.
        uint32_t info;
    };

    // The RSP's cull and clip rules: its GBI constants and the ROGUESQ_* switches of drawIndexedTri.
    struct TriRules {
        uint32_t cullBothMask = 0, cullFrontMask = 0;
        bool f5Cull = false, cullBothDraw = true, noCull = false, clip = true, pinAll = false, domeNearDrop = true;
    };

    struct BatchViewport {
        float scale[3], translate[3];
    };

    // A batch as the parser records it: its triangles, and what the resolve stage needs to cull, clip and expand them.
    struct BatchGeometry {
        std::vector<TriRef> tris;
        std::vector<Vertex> ownVertices;
        std::vector<BatchViewport> viewports;
        // The task's processed vertices by global index (4 floats per position, as RSP::setVertex leaves them). The parser grows these
        // arrays only after the resolve stage has caught up (Renderer::reserveVertices), so they stay put while a batch is resolved.
        const float *posTransformed = nullptr;
        const float *posScreen = nullptr;
        const Vertex *vertices = nullptr;
        float clipScaleX = 0.0f, clipScaleY = 0.0f;
        TriRules rules;

        void clear() {
            tris.clear();
            ownVertices.clear();
            viewports.clear();
        }
    };

    // Appends the batch's visible triangles to out as clip-space GL vertices, three per triangle; returns how many triangles that is.
    uint32_t buildBatchVertices(const DrawState &s, const BatchGeometry &g, std::vector<Vertex> &out);

    class Submitter;

    struct Renderer {
        explicit Renderer(Submitter *submitter);

        void beginTask(RT64::State *state);
        void endTask();

        // Vertex store for the current task; the index is the RSP "global index". The resolve stage reads it while the parser appends,
        // so room is reserved first (reserveVertices): the arrays only move once the resolve stage has caught up.
        void reserveVertices(uint32_t count);
        uint32_t addVertex(const Vertex &v);
        // Appends count vertices for the caller to fill; returns the first one, and its index in first.
        Vertex *addVertices(uint32_t count, uint32_t &first) {
            first = uint32_t(vertices.size());
            vertices.resize(vertices.size() + count);
            return &vertices[first];
        }
        Vertex &vertex(uint32_t index) { return vertices[index]; }
        uint32_t vertexCount() const { return uint32_t(vertices.size()); }

        // From the RSP shim: a triangle by global vertex indices, culled and clipped later by the resolve stage.
        void recordTriangle(uint32_t a, uint32_t b, uint32_t c);
        // From the RDP shim: raw RDP triangles (screen space) and texture/fill rects.
        void addRawTriangle(const Vertex v[3], uint8_t tile, uint8_t levels);
        void addRect(const RectDraw &rect);

        // Any draw state change; the next primitive compares its state against the open batch.
        void markDirty() { dirty = true; }
        // Draws the open batch. Called before TMEM or the color image changes.
        void flush();
        // From the RDP shim, after flush(): TMEM is kept by the submitter, which applies loads in order with the draws.
        void queueLoad(const TmemLoad &load);

        RT64::State *state = nullptr;
        Submitter *submitter;

        // Per-frame statistics for the log (drawn triangles and batches are counted by the submitter, after culling).
        uint32_t statRects = 0;

    private:
        void captureState(DrawState &s, DrawKind kind, bool textureOn, uint8_t tile, uint8_t levels);
        // Height of a color image: the tallest scissor the game has drawn it with (Rogue Squadron uses 512x448), at least width * 3/4.
        uint16_t imageHeight(uint32_t address, uint16_t width, int32_t scissorBottom);
        void ensureBatch(DrawKind kind, bool textureOn, uint8_t tile, uint8_t levels);
        // hard: an explicit flush (TMEM, color image, sync, task end). The flush ensureBatch makes on a state change is soft: culling now
        // happens later, so the resolve stage joins batches split only by triangles it culls, as the parser did not split them before.
        void flushBatch(bool hard);
        // A triangle from vertices the batch carries itself (rects, RDP triangles).
        void addOwnTriangle(const Vertex &a, const Vertex &b, const Vertex &c);

        std::vector<Vertex> vertices;
        BatchGeometry batchGeometry;
        DrawState batchState;
        bool batchOpen = false;
        bool dirty = true;
        // 2 / width and 2 / height of the open batch's color image: pixels to clip space without a division per vertex.
        float clipScaleX = 2.0f / 320.0f;
        float clipScaleY = 2.0f / 240.0f;
        // RSP viewport generation of the open batch's last viewport entry.
        uint32_t batchViewportGen = 0;

        struct ImageHeight {
            uint32_t address;
            uint16_t width, height;
        };
        std::vector<ImageHeight> imageHeights;
    };
}
