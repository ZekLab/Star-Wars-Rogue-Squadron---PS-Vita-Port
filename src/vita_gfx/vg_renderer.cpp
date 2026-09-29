#include "vg_renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "hle/rt64_state.h"

#include "vg_backend_gl.h"
#include "vg_submit.h"
#include "vg_prof.h"

namespace vg {
    Renderer::Renderer(Submitter *submitter) : submitter(submitter) {
        memset(&batchState, 0, sizeof(batchState));
    }

    void Renderer::beginTask(RT64::State *state) {
        this->state = state;
        vertices.clear();
        state->workloadQueue.workloads[0].drawData.posTransformed.clear();
        state->workloadQueue.workloads[0].drawData.posScreen.clear();
        dirty = true;
    }

    void Renderer::endTask() {
        flush();
    }

    uint32_t Renderer::addVertex(const Vertex &v) {
        vertices.push_back(v);
        return uint32_t(vertices.size() - 1);
    }

    static void copyColor(float dst[4], const hlslpp::float4 &src) {
        dst[0] = src.x;
        dst[1] = src.y;
        dst[2] = src.z;
        dst[3] = src.w;
    }

    static void copyTile(TileState &dst, const RT64::LoadTile &src) {
        dst.fmt = src.fmt;
        dst.siz = src.siz;
        dst.palette = src.palette;
        dst.cms = src.cms;
        dst.cmt = src.cmt;
        dst.masks = src.masks;
        dst.maskt = src.maskt;
        dst.shifts = src.shifts;
        dst.shiftt = src.shiftt;
        dst.line = src.line;
        dst.tmem = src.tmem;
        dst.uls = src.uls;
        dst.ult = src.ult;
        dst.lrs = src.lrs;
        dst.lrt = src.lrt;
    }

    uint16_t Renderer::imageHeight(uint32_t address, uint16_t width, int32_t scissorBottom) {
        if (width == 0) {
            return 0;
        }

        // A scissor taller than the image is RT64's reset default (8192), not the game's. Heights round up to 8 lines (447 -> 448).
        const uint16_t minHeight = uint16_t((uint32_t(width) * 3) / 4);
        const uint16_t candidate = ((scissorBottom > minHeight) && (scissorBottom <= width)) ? uint16_t((scissorBottom + 7) & ~7) : minHeight;
        for (ImageHeight &e : imageHeights) {
            if ((e.address == address) && (e.width == width)) {
                if (candidate > e.height) {
                    fprintf(stderr, "[vita-gfx] image 0x%06X w %u: height %u -> %u (scissor bottom %d)\n", address, width, e.height, candidate, scissorBottom);
                    e.height = candidate;
                }
                return e.height;
            }
        }
        if (imageHeights.size() >= 64) {
            imageHeights.erase(imageHeights.begin());
        }
        imageHeights.push_back({ address, width, candidate });
        return candidate;
    }

    void Renderer::captureState(DrawState &s, DrawKind kind, bool textureOn, uint8_t tile, uint8_t levels) {
        memset(&s, 0, sizeof(s));
        const RT64::RDP &rdp = *state->rdp;
        const RT64::RSP &rsp = *state->rsp;
        s.otherModeH = rdp.otherMode.H;
        s.otherModeL = rdp.otherMode.L;
        const interop::ColorCombiner &cc = rdp.colorCombinerStack[rdp.colorCombinerStackSize - 1];
        s.combineL = cc.L;
        s.combineH = cc.H;
        s.geometryMode = rsp.geometryModeStack[rsp.geometryModeStackSize - 1];
        copyColor(s.prim, rdp.primColorStack[rdp.primColorStackSize - 1]);
        copyColor(s.env, rdp.envColorStack[rdp.envColorStackSize - 1]);
        copyColor(s.fog, rdp.fogColorStack[rdp.fogColorStackSize - 1]);
        copyColor(s.blend, rdp.blendColorStack[rdp.blendColorStackSize - 1]);
        s.primLodFrac = rdp.primLODStack[rdp.primColorStackSize - 1].x;
        s.primDepth = rdp.primDepthStack[rdp.primDepthStackSize - 1].x;
        s.k4 = rdp.convertK[4] / 255.0f;
        s.k5 = rdp.convertK[5] / 255.0f;
        s.fillColor = rdp.fillColorStack[rdp.fillColorStackSize - 1];
        s.colorImage = rdp.colorImage.address;
        s.depthImage = rdp.depthImage.address;
        s.colorImageWidth = rdp.colorImage.width;
        s.colorImageSiz = rdp.colorImage.siz;
        s.kind = kind;
        s.textureOn = textureOn ? 1 : 0;
        if (textureOn) {
            s.textureTile = tile & 7;
            s.textureLevels = levels;
            copyTile(s.tiles[0], rdp.tiles[tile & 7]);
            copyTile(s.tiles[1], rdp.tiles[(tile + 1) & 7]);
        }

        const RT64::FixedRect &sc = rdp.scissorRectStack[rdp.scissorStackSize - 1];
        s.scissorX0 = sc.ulx >> 2;
        s.scissorY0 = sc.uly >> 2;
        s.scissorX1 = sc.lrx >> 2;
        s.scissorY1 = sc.lry >> 2;
        s.colorImageHeight = imageHeight(s.colorImage, s.colorImageWidth, s.scissorY1);
    }

    void Renderer::ensureBatch(DrawKind kind, bool textureOn, uint8_t tile, uint8_t levels) {
        if (batchOpen && !dirty && (batchState.kind == kind) && (batchState.textureOn == (textureOn ? 1 : 0)) &&
            (!textureOn || ((batchState.textureTile == (tile & 7)) && (batchState.textureLevels == levels))))
        {
            return;
        }

        ProfScope prof(PROF_BATCH_STATE);
        DrawState s;
        captureState(s, kind, textureOn, tile, levels);
        dirty = false;
        if (batchOpen && (memcmp(&s, &batchState, sizeof(DrawState)) == 0)) {
            return;
        }

        flush();
        batchState = s;
        batchOpen = true;
        if ((s.colorImageWidth > 0) && (s.colorImageHeight > 0)) {
            clipScaleX = 2.0f / float(s.colorImageWidth);
            clipScaleY = 2.0f / float(s.colorImageHeight);
        }
    }

    // Color-image pixels to clip space; y points down on the N64 and up in GL (the backend flips on present).
    void Renderer::pushScreenVertex(const Vertex &v, bool hasW) {
        Vertex o = v;
        const float w = hasW ? v.w : 1.0f;
        const bool zPrim = (batchState.otherModeL & (1U << G_MDSFT_ZSRCSEL)) != 0;
        const float z = zPrim ? batchState.primDepth : v.z;
        o.x = (v.x * clipScaleX - 1.0f) * w;
        o.y = (1.0f - v.y * clipScaleY) * w;
        o.z = (z * 2.0f - 1.0f) * w;
        o.w = w;
        batchVertices.push_back(o);
    }

    void Renderer::addTriangle(uint32_t a, uint32_t b, uint32_t c) {
        const RT64::RSP::TextureState &ts = state->rsp->textureState;
        ensureBatch(DrawKind::Triangles, ts.on != 0, ts.tile, ts.levels);
        pushScreenVertex(vertices[a], true);
        pushScreenVertex(vertices[b], true);
        pushScreenVertex(vertices[c], true);
        statTris++;
    }

    void Renderer::addRawTriangle(const Vertex v[3], uint8_t tile, uint8_t levels) {
        ensureBatch(DrawKind::Triangles, true, tile, levels);
        for (int i = 0; i < 3; i++) {
            pushScreenVertex(v[i], true);
        }
        statTris++;
    }

    void Renderer::addRect(const RectDraw &r) {
        ensureBatch(DrawKind::Rect, r.textured, r.tile, 1);
        Vertex tl{}, tr{}, bl{}, br{};
        tl.x = r.x0; tl.y = r.y0;
        tr.x = r.x1; tr.y = r.y0;
        bl.x = r.x0; bl.y = r.y1;
        br.x = r.x1; br.y = r.y1;
        tl.s = r.u0; tl.t = r.v0;
        br.s = r.u1; br.t = r.v1;
        tr.s = r.flip ? r.u0 : r.u1;
        tr.t = r.flip ? r.v1 : r.v0;
        bl.s = r.flip ? r.u1 : r.u0;
        bl.t = r.flip ? r.v0 : r.v1;
        pushScreenVertex(tl, false);
        pushScreenVertex(tr, false);
        pushScreenVertex(bl, false);
        pushScreenVertex(br, false);
        pushScreenVertex(tr, false);
        pushScreenVertex(bl, false);
        statRects++;
    }

    void Renderer::flush() {
        if (batchOpen && !batchVertices.empty()) {
            submitter->draw(batchState, batchVertices, reinterpret_cast<const uint8_t *>(state->rdp->TMEM), state->rdp->tmemLoadTag);
            statBatches++;
        }
        batchVertices.clear();
        batchOpen = false;
        dirty = true;
    }
}
