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
    // Room for this many processed vertices per task before the store has to move (a dense mission frame uses ~10000).
    constexpr size_t VertexStoreReserve = 32768;

    Renderer::Renderer(Submitter *submitter) : submitter(submitter) {
        memset(&batchState, 0, sizeof(batchState));
        vertices.reserve(VertexStoreReserve);
    }

    void Renderer::beginTask(RT64::State *state) {
        this->state = state;
        vertices.clear();
        auto &drawData = state->workloadQueue.workloads[0].drawData;
        drawData.posTransformed.clear();
        drawData.posScreen.clear();
        drawData.posTransformed.reserve(vertices.capacity());
        drawData.posScreen.reserve(vertices.capacity());
        dirty = true;
    }

    void Renderer::endTask() {
        flush();
        submitter->endTask();
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

        flushBatch(false);
        batchState = s;
        batchOpen = true;
        if ((s.colorImageWidth > 0) && (s.colorImageHeight > 0)) {
            clipScaleX = 2.0f / float(s.colorImageWidth);
            clipScaleY = 2.0f / float(s.colorImageHeight);
        }
    }

    void Renderer::recordTriangle(uint32_t a, uint32_t b, uint32_t c) {
        const RT64::RSP::TextureState &ts = state->rsp->textureState;
        ensureBatch(DrawKind::Triangles, ts.on != 0, ts.tile, ts.levels);
        // Vertices clipping creates use the viewport of their triangle's time, so the batch keeps one entry per viewport change.
        const RT64::RSP &rsp = *state->rsp;
        if (batchGeometry.viewports.empty() || (batchViewportGen != rsp.viewportGen)) {
            const interop::RSPViewport &vp = rsp.viewportStack[rsp.viewportStackSize - 1];
            batchGeometry.viewports.push_back({ { vp.scale.x, vp.scale.y, vp.scale.z }, { vp.translate.x, vp.translate.y, vp.translate.z } });
            batchViewportGen = rsp.viewportGen;
        }
        batchGeometry.tris.push_back({ a, b, c, uint32_t(batchGeometry.viewports.size() - 1) << 8 });
    }

    void Renderer::addOwnTriangle(const Vertex &a, const Vertex &b, const Vertex &c) {
        const uint32_t first = uint32_t(batchGeometry.ownVertices.size());
        batchGeometry.ownVertices.push_back(a);
        batchGeometry.ownVertices.push_back(b);
        batchGeometry.ownVertices.push_back(c);
        batchGeometry.tris.push_back({ first, first + 1, first + 2, TriRef::Direct });
    }

    void Renderer::addRawTriangle(const Vertex v[3], uint8_t tile, uint8_t levels) {
        ensureBatch(DrawKind::Triangles, true, tile, levels);
        addOwnTriangle(v[0], v[1], v[2]);
    }

    void Renderer::addRect(const RectDraw &r) {
        ensureBatch(DrawKind::Rect, r.textured, r.tile, 1);
        // w = 1: rect corners are already in color-image pixels.
        Vertex tl{}, tr{}, bl{}, br{};
        tl.x = r.x0; tl.y = r.y0;
        tr.x = r.x1; tr.y = r.y0;
        bl.x = r.x0; bl.y = r.y1;
        br.x = r.x1; br.y = r.y1;
        tl.w = tr.w = bl.w = br.w = 1.0f;
        tl.s = r.u0; tl.t = r.v0;
        br.s = r.u1; br.t = r.v1;
        tr.s = r.flip ? r.u0 : r.u1;
        tr.t = r.flip ? r.v1 : r.v0;
        bl.s = r.flip ? r.u1 : r.u0;
        bl.t = r.flip ? r.v0 : r.v1;
        addOwnTriangle(tl, tr, bl);
        addOwnTriangle(br, tr, bl);
        statRects++;
    }

    void Renderer::reserveVertices(uint32_t count) {
        auto &drawData = state->workloadQueue.workloads[0].drawData;
        const size_t need = vertices.size() + count;
        if ((need <= vertices.capacity()) && (drawData.posTransformed.size() + count <= drawData.posTransformed.capacity()) &&
            (drawData.posScreen.size() + count <= drawData.posScreen.capacity()))
        {
            return;
        }

        // The resolve stage reads these arrays by pointer: let it finish the batches queued so far before they move.
        submitter->sync();
        const size_t grown = std::max(need, vertices.capacity() * 2);
        vertices.reserve(grown);
        drawData.posTransformed.reserve(grown);
        drawData.posScreen.reserve(grown);
    }

    void Renderer::flush() {
        flushBatch(true);
    }

    void Renderer::flushBatch(bool hard) {
        if (batchOpen && !batchGeometry.tris.empty()) {
            const auto &drawData = state->workloadQueue.workloads[0].drawData;
            batchGeometry.posTransformed = reinterpret_cast<const float *>(drawData.posTransformed.data());
            batchGeometry.posScreen = reinterpret_cast<const float *>(drawData.posScreen.data());
            batchGeometry.vertices = vertices.data();
            batchGeometry.clipScaleX = clipScaleX;
            batchGeometry.clipScaleY = clipScaleY;
            batchGeometry.rules = state->rsp->triRules();
            submitter->draw(batchState, batchGeometry, hard);
        }
        batchGeometry.clear();
        batchOpen = false;
        dirty = true;
    }

    void Renderer::queueLoad(const TmemLoad &load) {
        submitter->load(load, state->RDRAM);
    }

    // Resolve stage: RSP::drawIndexedTri's culling and clipping, then the color-image pixels to clip space step (y points down on the
    // N64 and up in GL; the backend flips on present). Same operations in the same order as on the parser before, so the same results.
    uint32_t buildBatchVertices(const DrawState &s, const BatchGeometry &g, std::vector<Vertex> &out) {
        constexpr uint32_t ClipBit = 0x80000000u;
        constexpr float kNearW = 1e-4f;
        constexpr float kGuard = 2.0f;
        const TriRules &r = g.rules;
        const uint32_t geometryMode = s.geometryMode;
        const bool doubleSided = !r.f5Cull && r.cullBothDraw && (geometryMode & r.cullBothMask) == r.cullBothMask;
        const uint32_t cullTestMask = r.f5Cull ? (r.cullBothMask & ~r.cullFrontMask) : r.cullBothMask;
        const bool usesCulling = !r.noCull && !doubleSided && (geometryMode & cullTestMask);
        const bool isDomeTri = r.cullBothMask != 0 && (geometryMode & r.cullBothMask) == r.cullBothMask && (r.pinAll || (s.otherModeL & G_ZS_PRIM) != 0);
        const bool zPrim = (s.otherModeL & (1U << G_MDSFT_ZSRCSEL)) != 0;
        const float primDepth = s.primDepth;
        const float sx = g.clipScaleX, sy = g.clipScaleY;

        // Vertices clipping creates, addressed with ClipBit set.
        std::vector<float> clipPT, clipPS;
        std::vector<Vertex> clipV;
        auto PT = [&](uint32_t i) -> const float * {
            return (i & ClipBit) ? &clipPT[size_t(i & ~ClipBit) * 4] : (g.posTransformed + size_t(i) * 4);
        };
        auto PS = [&](uint32_t i) -> const float * {
            return (i & ClipBit) ? &clipPS[size_t(i & ~ClipBit) * 4] : (g.posScreen + size_t(i) * 4);
        };
        auto V = [&](uint32_t i) -> const Vertex & {
            return (i & ClipBit) ? clipV[i & ~ClipBit] : g.vertices[i];
        };

        uint32_t drawn = 0;
        auto emit = [&](const Vertex &v) {
            const float w = v.w;
            const float z = zPrim ? primDepth : v.z;
            out.emplace_back((v.x * sx - 1.0f) * w, (1.0f - v.y * sy) * w, (z * 2.0f - 1.0f) * w, w, v.r, v.g, v.b, v.a, v.s, v.t);
        };
        auto addTriangle = [&](uint32_t a, uint32_t b, uint32_t c) {
            emit(V(a));
            emit(V(b));
            emit(V(c));
            drawn++;
        };
        // cross(V, U).z with U = p1 - p0, V = p2 - p0, as hlslpp computes it.
        auto visible = [&](uint32_t g0, uint32_t g1, uint32_t g2) {
            if (!usesCulling) {
                return true;
            }
            const float *p0 = PS(g0), *p1 = PS(g1), *p2 = PS(g2);
            const float ux = p1[0] - p0[0], uy = p1[1] - p0[1];
            const float vx = p2[0] - p0[0], vy = p2[1] - p0[1];
            return (vx * uy - vy * ux) >= 0.0f;
        };
        // RSP::appendClippedVertex: clip-space lerp between two processed vertices.
        auto appendClippedVertex = [&](uint32_t ga, uint32_t gb, float t, const BatchViewport &viewport) {
            const float *a4 = PT(ga), *b4 = PT(gb);
            const hlslpp::float4 A(a4[0], a4[1], a4[2], a4[3]), B(b4[0], b4[1], b4[2], b4[3]);
            const hlslpp::float4 pt = A + (B - A) * t;
            float w = float(pt.w);
            if (w == 0.0f) {
                w = 1e-6f;
            }

            const hlslpp::float3 scale(viewport.scale[0], viewport.scale[1], viewport.scale[2]);
            const hlslpp::float3 translate(viewport.translate[0], viewport.translate[1], viewport.translate[2]);
            const hlslpp::float3 ps = (pt.xyz / hlslpp::float3(w, -w, w)) * scale + translate;
            clipPT.insert(clipPT.end(), { float(pt.x), float(pt.y), float(pt.z), float(pt.w) });
            clipPS.insert(clipPS.end(), { float(ps.x), float(ps.y), float(ps.z), 0.0f });

            const Vertex a = V(ga);
            const Vertex b = V(gb);
            auto lf = [t](float x, float y) { return x + (y - x) * t; };
            Vertex o;
            o.x = float(ps.x);
            o.y = float(ps.y);
            o.z = float(ps.z);
            o.w = w;
            o.r = lf(a.r, b.r);
            o.g = lf(a.g, b.g);
            o.b = lf(a.b, b.b);
            o.a = lf(a.a, b.a);
            o.s = lf(a.s, b.s);
            o.t = lf(a.t, b.t);
            clipV.push_back(o);
            return ClipBit | uint32_t(clipV.size() - 1);
        };
        auto planeDist = [&](uint32_t i, int plane) -> float {
            const float *p = PT(i);
            const float x = p[0], y = p[1], w = p[3];
            switch (plane) {
                case 0:  return w - kNearW;
                case 1:  return kGuard * w + x;
                case 2:  return kGuard * w - x;
                case 3:  return kGuard * w + y;
                default: return kGuard * w - y;
            }
        };

        for (const TriRef &t : g.tris) {
            if (t.info & TriRef::Direct) {
                emit(g.ownVertices[t.a]);
                emit(g.ownVertices[t.b]);
                emit(g.ownVertices[t.c]);
                drawn++;
                continue;
            }

            const uint32_t gi[3] = { t.a, t.b, t.c };
            const bool degenerate = (gi[0] == gi[1]) || (gi[1] == gi[2]) || (gi[0] == gi[2]);
            bool extreme = false;
            bool anyNear = false;
            if (r.clip && !degenerate) {
                for (int i = 0; i < 3; i++) {
                    const float *p = PT(gi[i]);
                    const float pw = p[3], px = p[0], py = p[1];
                    if (pw <= kNearW) {
                        extreme = true;
                        anyNear = true;
                        break;
                    }
                    if (std::fabs(px) > kGuard * pw || std::fabs(py) > kGuard * pw) {
                        extreme = true;
                        break;
                    }
                }
            }

            if (anyNear && isDomeTri && r.domeNearDrop) {
                continue;
            }

            if (extreme) {
                const BatchViewport &viewport = g.viewports[t.info >> 8];
                uint32_t poly[10] = { gi[0], gi[1], gi[2] };
                int polyN = 3;
                for (int pl = 0; pl < 5 && polyN > 0; pl++) {
                    uint32_t clipped[10];
                    int outCnt = 0;
                    for (int i = 0; i < polyN; i++) {
                        const uint32_t gA = poly[i];
                        // Not (i + 1) % polyN: the A9 has no divide instruction.
                        const uint32_t gB = poly[(i + 1 < polyN) ? (i + 1) : 0];
                        const float dA = planeDist(gA, pl);
                        const float dB = planeDist(gB, pl);
                        const bool inA = dA >= 0.0f;
                        const bool inB = dB >= 0.0f;
                        if (inA && outCnt < 10) {
                            clipped[outCnt++] = gA;
                        }
                        if ((inA != inB) && outCnt < 10) {
                            clipped[outCnt++] = appendClippedVertex(gA, gB, dA / (dA - dB), viewport);
                        }
                    }
                    polyN = outCnt;
                    std::copy(clipped, clipped + polyN, poly);
                }

                for (int f = 1; f + 1 < polyN; f++) {
                    if (visible(poly[0], poly[f], poly[f + 1])) {
                        addTriangle(poly[0], poly[f], poly[f + 1]);
                    }
                }
                continue;
            }

            const bool nearCrossing = (PT(gi[0])[3] <= 0.0f) || (PT(gi[1])[3] <= 0.0f) || (PT(gi[2])[3] <= 0.0f);
            if (!nearCrossing && !visible(gi[0], gi[1], gi[2])) {
                continue;
            }

            addTriangle(gi[0], gi[1], gi[2]);
        }
        return drawn;
    }
}
