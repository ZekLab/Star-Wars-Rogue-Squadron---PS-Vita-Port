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

    class Submitter;

    struct Renderer {
        explicit Renderer(Submitter *submitter);

        void beginTask(RT64::State *state);
        void endTask();

        // Vertex store for the current task; the index is the RSP "global index".
        uint32_t addVertex(const Vertex &v);
        // Appends count vertices for the caller to fill; returns the first one, and its index in first.
        Vertex *addVertices(uint32_t count, uint32_t &first) {
            first = uint32_t(vertices.size());
            vertices.resize(vertices.size() + count);
            return &vertices[first];
        }
        Vertex &vertex(uint32_t index) { return vertices[index]; }
        uint32_t vertexCount() const { return uint32_t(vertices.size()); }

        // From the RSP shim, after culling and clipping.
        void addTriangle(uint32_t a, uint32_t b, uint32_t c);
        // From the RDP shim: raw RDP triangles (screen space) and texture/fill rects.
        void addRawTriangle(const Vertex v[3], uint8_t tile, uint8_t levels);
        void addRect(const RectDraw &rect);

        // Any draw state change; the next primitive compares its state against the open batch.
        void markDirty() { dirty = true; }
        // Draws the open batch. Called before TMEM or the color image changes.
        void flush();

        RT64::State *state = nullptr;
        Submitter *submitter;

        // Per-frame statistics for the log.
        uint32_t statTris = 0, statRects = 0, statBatches = 0;

    private:
        void captureState(DrawState &s, DrawKind kind, bool textureOn, uint8_t tile, uint8_t levels);
        // Height of a color image: the tallest scissor the game has drawn it with (Rogue Squadron uses 512x448), at least width * 3/4.
        uint16_t imageHeight(uint32_t address, uint16_t width, int32_t scissorBottom);
        void ensureBatch(DrawKind kind, bool textureOn, uint8_t tile, uint8_t levels);
        void pushScreenVertex(const Vertex &v, bool hasW);

        std::vector<Vertex> vertices;
        std::vector<Vertex> batchVertices;
        DrawState batchState;
        bool batchOpen = false;
        bool dirty = true;
        // 2 / width and 2 / height of the open batch's color image: pixels to clip space without a division per vertex.
        float clipScaleX = 2.0f / 320.0f;
        float clipScaleY = 2.0f / 240.0f;

        struct ImageHeight {
            uint32_t address;
            uint16_t width, height;
        };
        std::vector<ImageHeight> imageHeights;
    };
}
