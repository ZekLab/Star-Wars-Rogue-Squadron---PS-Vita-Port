// RSP side of the Vita renderer: RT64's RSP (lib/rt64/src/hle/rt64_rsp.cpp) with its GPU workload pipeline replaced by
// CPU vertex processing. Matrix/viewport/geometry-mode handling is ported as is; per-vertex transform, lighting, fog and
// texgen follow RT64's RSPProcessCS.hlsl, and triangle culling/clipping follows RT64's drawIndexedTri.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "../include/rt64_extended_gbi.h"
#include "common/rt64_common.h"
#include "common/rt64_math.h"
#include "gbi/rt64_f3d.h"
#include "gbi/rt64_gbi.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"

#include "vg_prof.h"
#include "vg_renderer.h"

namespace RT64 {
    constexpr float DepthRange = 1024.0f;
    // setVertex writes the task's position arrays as plain floats.
    static_assert(sizeof(hlslpp::float4) == 16 && sizeof(hlslpp::float3) == 16, "hlslpp scalar vectors are 4 packed floats");

    static bool envFlag(const char *name, bool def) {
        const char *e = std::getenv(name);
        if (e == nullptr || e[0] == '\0') {
            return def;
        }
        return e[0] != '0';
    }

    RSP::RSP(State *state) {
        this->state = state;
        NoN = false;
        cullBothMask = 0;
        cullFrontMask = 0;
        projMask = 0;
        loadMask = 0;
        pushMask = 0;
        shadingSmoothMask = 0;
        segments.fill(0);
        reset();
    }

    void RSP::reset() {
        vg::ProfScope prof(vg::PROF_RSP_RESET);
        modelMatrixStackSize = 1;
        projectionMatrixStackSize = 1;
        viewportStackSize = 1;
        geometryModeStackSize = 1;
        otherModeStackSize = 1;
        modelMatrixStack.fill(hlslpp::float4x4(0.0f));
        modelMatrixSegmentedAddressStack.fill(0);
        modelMatrixPhysicalAddressStack.fill(0);
        viewMatrixStack[0] = hlslpp::float4x4(0.0f);
        projMatrixStack[0] = hlslpp::float4x4(0.0f);
        viewProjMatrixStack[0] = hlslpp::float4x4(0.0f);
        invViewProjMatrixStack[0] = hlslpp::float4x4(0.0f);
        vertices.fill({});
        indices.fill(0);
        used.reset();
        slotTransformGen.fill(0);
        bumpTransformGen();
        viewportGen++;
        faceSourceSlots = nullptr;
        lights.fill({});
        segments.fill(0);
        viewportStack[0] = {};
        clipRatios[0] = 1;
        clipRatios[1] = 1;
        clipRatios[2] = -1;
        clipRatios[3] = -1;
        textureState = {};
        projectionIndex = -1;
        projectionMatrixChanged = false;
        projectionMatrixInversed = false;
        viewportChanged = false;
        modelViewProjMatrix = hlslpp::float4x4(0.0f);
        modelViewProjChanged = false;
        modelViewProjInserted = false;
        lightCount = 0;
        lightsChanged = false;
        vertexColorPDAddress = 0;
        fog.mul = 0.0f;
        fog.offset = 0.0f;
        lookAt.x = { 0.0f, 0.0f, 0.0f };
        lookAt.y = { 0.0f, 0.0f, 0.0f };
        otherModeStack[0].L = 0x0;
        otherModeStack[0].H = 0x080CFF;
        geometryModeStack[0] = G_CLIPPING;
        fogChanged = true;
        lookAtChanged = true;
        clearExtended();
    }

    uint32_t RSP::fromSegmented(uint32_t segAddress) {
        return segments[((segAddress) >> 24) & 0x0F] + ((segAddress) & 0x00FFFFFF);
    }

    uint32_t RSP::fromSegmentedMasked(uint32_t segAddress) {
        return fromSegmented(segAddress) & 0x00FFFFF8;
    }

    uint32_t RSP::fromSegmentedMaskedPD(uint32_t segAddress) {
        return fromSegmented(segAddress) & 0x00FFFFFC;
    }

    void RSP::setSegment(uint32_t seg, uint32_t address) {
        segments[seg & (RSP_MAX_SEGMENTS - 1)] = address;
    }

    void RSP::matrixCommon(const hlslpp::float4x4 &floatMatrix, uint32_t address, uint8_t params) {
        hlslpp::float4x4 &viewMatrix = viewMatrixStack[projectionMatrixStackSize - 1];
        hlslpp::float4x4 &projMatrix = projMatrixStack[projectionMatrixStackSize - 1];
        hlslpp::float4x4 &viewProjMatrix = viewProjMatrixStack[projectionMatrixStackSize - 1];
        if (params & projMask) {
            if (params & loadMask) {
                viewProjMatrix = floatMatrix;
                if (isMatrixViewProj(floatMatrix)) {
                    matrixDecomposeViewProj(floatMatrix, viewMatrix, projMatrix);
                }
                else {
                    projMatrix = floatMatrix;
                    viewMatrix = hlslpp::float4x4::identity();
                }
            }
            else {
                viewProjMatrix = hlslpp::mul(floatMatrix, viewProjMatrix);
                if (isMatrixAffine(floatMatrix) && !isMatrixIdentity(floatMatrix)) {
                    viewMatrix = hlslpp::mul(floatMatrix, viewMatrix);
                }
                else {
                    projMatrix = hlslpp::mul(floatMatrix, projMatrix);
                }
            }

            projectionMatrixSegmentedAddressStack[projectionMatrixStackSize - 1] = address;
            projectionMatrixPhysicalAddressStack[projectionMatrixStackSize - 1] = fromSegmentedMasked(address);
            projectionMatrixChanged = true;
            projectionMatrixInversed = false;
        }
        else {
            if ((params & pushMask) && (modelMatrixStackSize < RSP_MATRIX_STACK_SIZE)) {
                modelMatrixStackSize++;
                modelMatrixStack[modelMatrixStackSize - 1] = modelMatrixStack[modelMatrixStackSize - 2];
            }

            if (params & loadMask) {
                modelMatrixStack[modelMatrixStackSize - 1] = floatMatrix;
            }
            else {
                modelMatrixStack[modelMatrixStackSize - 1] = hlslpp::mul(floatMatrix, modelMatrixStack[modelMatrixStackSize - 1]);
            }

            modelMatrixSegmentedAddressStack[modelMatrixStackSize - 1] = address;
            modelMatrixPhysicalAddressStack[modelMatrixStackSize - 1] = fromSegmentedMasked(address);
        }

        modelViewProjChanged = true;
    }

    void RSP::matrix(uint32_t address, uint8_t params) {
        const uint32_t rdramAddress = fromSegmentedMasked(address);
        const FixedMatrix *fixedMatrix = reinterpret_cast<FixedMatrix *>(state->fromRDRAM(rdramAddress));
        matrixCommon(fixedMatrix->toMatrix4x4(), address, params);
    }

    void RSP::popMatrix(uint32_t count) {
        while (count--) {
            if (modelMatrixStackSize > 1) {
                modelMatrixStackSize--;
                modelViewProjChanged = true;
            }
        }
    }

    void RSP::forceMatrix(uint32_t address) {
        const uint32_t rdramAddress = fromSegmentedMasked(address);
        const FixedMatrix *fixedMatrix = reinterpret_cast<FixedMatrix *>(state->fromRDRAM(rdramAddress));
        modelViewProjMatrix = fixedMatrix->toMatrix4x4();
        modelViewProjInserted = true;
        modelViewProjChanged = false;
        bumpTransformGen();
    }

    void RSP::computeModelViewProj() {
        const hlslpp::float4x4 &viewProjMatrix = viewProjMatrixStack[projectionMatrixStackSize - 1];
        modelViewProjMatrix = hlslpp::mul(modelMatrixStack[modelMatrixStackSize - 1], viewProjMatrix);
        modelViewProjInserted = false;
        modelViewProjChanged = false;
        bumpTransformGen();
    }

    // Per-vertex processing (RSPProcessCS.hlsl on the CPU). Matrices use the row-vector convention of the CPU side
    // (p * Model * ViewProj); the shader's mul(v, M) on the transposed upload becomes mul(M, v) here.
    static hlslpp::float3 normalizeSafe(const hlslpp::float3 &v) {
        const float l = hlslpp::length(v);
        return (l > 0.0f) ? hlslpp::float3(v / l) : v;
    }

    void RSP::setVertex(uint32_t address, uint32_t vtxCount, uint32_t dstIndex) {
        const uint32_t *sourceSlots = faceSourceSlots;
        faceSourceSlots = nullptr;
        if ((dstIndex >= RSP_MAX_VERTICES) || ((dstIndex + vtxCount) > RSP_MAX_VERTICES)) {
            return;
        }

        vg::ProfScope prof(vg::PROF_SET_VERTEX);
        const uint32_t rdramAddress = fromSegmentedMasked(address);
        const Vertex *dlVerts = reinterpret_cast<const Vertex *>(state->fromRDRAM(rdramAddress));
        memcpy(&vertices[dstIndex], dlVerts, sizeof(Vertex) * vtxCount);

        if (modelViewProjChanged) {
            computeModelViewProj();
        }

        const uint32_t geometryMode = geometryModeStack[geometryModeStackSize - 1];
        const bool usesLighting = (geometryMode & G_LIGHTING) != 0;
        const bool usesFog = (geometryMode & G_FOG) != 0;
        const bool usesTextureGen = (geometryMode & (G_LIGHTING | G_TEXTURE_GEN)) == (G_LIGHTING | G_TEXTURE_GEN);
        const bool textureGenLinear = (geometryMode & G_TEXTURE_GEN_LINEAR) != 0;
        const hlslpp::float4x4 &mvp = modelViewProjMatrix;
        // Only lighting and texgen need the world matrix; a 4x4 product per call is noticeable on the Vita.
        hlslpp::float4x4 worldMatrix;
        if (usesLighting || usesTextureGen) {
            worldMatrix = hlslpp::mul(modelMatrixStack[modelMatrixStackSize - 1], extended.viewProjMatrix);
        }
        const interop::RSPViewport &viewport = viewportStack[viewportStackSize - 1];

        // Light directions in model space are shared by the whole batch.
        hlslpp::float3 localLightDirs[RSP_MAX_LIGHTS];
        hlslpp::float3 lightColors[RSP_MAX_LIGHTS + 1];
        int dirLights = 0;
        if (usesLighting) {
            dirLights = std::min<int>(lightCount, RSP_MAX_LIGHTS);
            for (int l = 0; l <= dirLights; l++) {
                const Light &light = lights[l];
                lightColors[l] = hlslpp::float3(light.dir.colr / 255.0f, light.dir.colg / 255.0f, light.dir.colb / 255.0f);
                if (l < dirLights) {
                    const hlslpp::float3 dir(float(light.dir.dirx), float(light.dir.diry), float(light.dir.dirz));
                    localLightDirs[l] = normalizeSafe(hlslpp::mul(worldMatrix, hlslpp::float4(dir, 0.0f)).xyz);
                }
            }
        }

        hlslpp::float3 lookAtX, lookAtY;
        if (usesTextureGen) {
            lookAtX = normalizeSafe(hlslpp::mul(worldMatrix, hlslpp::float4(hlslpp::float3(lookAt.x), 0.0f)).xyz);
            lookAtY = normalizeSafe(hlslpp::mul(worldMatrix, hlslpp::float4(hlslpp::float3(lookAt.y), 0.0f)).xyz);
        }

        Workload &workload = state->ext.workloadQueue->workloads[state->ext.workloadQueue->writeCursor];
        auto &posTransformed = workload.drawData.posTransformed;
        auto &posScreen = workload.drawData.posScreen;
        vg::Renderer &renderer = *state->renderer;
        const float sc = float(textureState.sc);
        const float tc = float(textureState.tc);
        // Divisions are slow on the Vita's VFP: multiply by reciprocals (differences stay within an ulp).
        constexpr float Inv127 = 1.0f / 127.0f;
        constexpr float Inv255 = 1.0f / 255.0f;
        // Exact: the int32 product is rounded once either way, and the divisor is a power of two.
        constexpr float InvTexDivisor = 1.0f / (65536.0f * 32.0f);
        // Outputs are written in place: the matrix and viewport stay in registers (-fno-strict-aliasing makes every store a possible alias) and no temporaries go through the stack.
        renderer.reserveVertices(vtxCount);
        const size_t firstOut = posTransformed.size();
        posTransformed.resize(firstOut + vtxCount);
        posScreen.resize(firstOut + vtxCount);
        float *ptOut = reinterpret_cast<float *>(&posTransformed[firstOut]);
        float *psOut = reinterpret_cast<float *>(&posScreen[firstOut]);
        uint32_t globalIndex = 0;
        vg::Vertex *vOut = renderer.addVertices(vtxCount, globalIndex);
        float m[16];
        memcpy(m, &mvp, sizeof(m));
        const float scaleX = viewport.scale.x, scaleY = viewport.scale.y, scaleZ = viewport.scale.z;
        const float transX = viewport.translate.x, transY = viewport.translate.y, transZ = viewport.translate.z;
        // A face copies its vertices from cache slots a load transformed moments ago; under the same matrix and viewport their positions
        // are taken from there (same inputs, same results), and only color, alpha and texcoords are worked out again.
        statVertices += vtxCount;
        for (uint32_t i = dstIndex; i < dstIndex + vtxCount; i++, ptOut += 4, psOut += 4, vOut++) {
            const Vertex &v = vertices[i];
            float tx, ty, tz, tw, w, invW, psx, psy, psz;
            const uint32_t src = (sourceSlots != nullptr) ? sourceSlots[i - dstIndex] : RSP_MAX_VERTICES;
            if ((src < dstIndex) && (slotTransformGen[src] == transformGen) && (vertices[src].x == v.x) && (vertices[src].y == v.y) && (vertices[src].z == v.z)) {
                const uint32_t g = indices[src];
                const float *pt = reinterpret_cast<const float *>(&posTransformed[g]);
                const float *ps = reinterpret_cast<const float *>(&posScreen[g]);
                tx = pt[0];
                ty = pt[1];
                tz = pt[2];
                tw = pt[3];
                psx = ps[0];
                psy = ps[1];
                psz = ps[2];
                w = renderer.vertex(g).w;
                invW = usesFog ? (1.0f / w) : 0.0f;
                statReusedVertices++;
            }
            else {
                // hlslpp's mul(float4(x, y, z, 1), mvp) in the same order: ((row0 * x + row1 * y) + row2 * z) + row3, each step rounded.
                const float fx = float(v.x), fy = float(v.y), fz = float(v.z);
                tx = ((m[0] * fx + m[4] * fy) + m[8] * fz) + m[12];
                ty = ((m[1] * fx + m[5] * fy) + m[9] * fz) + m[13];
                tz = ((m[2] * fx + m[6] * fy) + m[10] * fz) + m[14];
                tw = ((m[3] * fx + m[7] * fy) + m[11] * fz) + m[15];
                w = tw;
                if (w == 0.0f) {
                    w = 1e-6f;
                }

                invW = 1.0f / w;
                psx = (tx * invW) * scaleX + transX;
                psy = (ty * -invW) * scaleY + transY;
                psz = (tz * invW) * scaleZ + transZ;
            }
            ptOut[0] = tx;
            ptOut[1] = ty;
            ptOut[2] = tz;
            ptOut[3] = tw;
            psOut[0] = psx;
            psOut[1] = psy;
            psOut[2] = psz;

            vg::Vertex &out = *vOut;
            out.x = psx;
            out.y = psy;
            out.z = psz;
            out.w = w;

            const hlslpp::float3 norm(v.normal.x * Inv127, v.normal.y * Inv127, v.normal.z * Inv127);
            if (usesLighting) {
                hlslpp::float3 result = lightColors[dirLights];
                for (int l = 0; l < dirLights; l++) {
                    result += hlslpp::max(hlslpp::dot(norm, localLightDirs[l]), hlslpp::float1(0.0f)) * lightColors[l];
                }
                result = hlslpp::min(result, hlslpp::float3(1.0f, 1.0f, 1.0f));
                out.r = float(result.x);
                out.g = float(result.y);
                out.b = float(result.z);
            }
            else {
                out.r = v.color.r * Inv255;
                out.g = v.color.g * Inv255;
                out.b = v.color.b * Inv255;
            }

            if (usesFog) {
                const float fogAlpha = (std::max(tz, 0.0f) * invW) * fog.mul + fog.offset;
                out.a = std::clamp(fogAlpha * Inv255, 0.0f, 1.0f);
            }
            else {
                out.a = v.color.a * Inv255;
            }

            if (usesTextureGen) {
                float gu = std::clamp(float(hlslpp::dot(norm, lookAtX)), -1.0f, 1.0f);
                float gv = std::clamp(float(hlslpp::dot(norm, lookAtY)), -1.0f, 1.0f);
                if (textureGenLinear) {
                    gu = std::acos(-gu) * 325.94932f;
                    gv = std::acos(-gv) * 325.94932f;
                }
                else {
                    gu = (gu + 1.0f) * 512.0f;
                    gv = (gv + 1.0f) * 512.0f;
                }
                out.s = (sc / 65536.0f) * gu;
                out.t = (tc / 65536.0f) * gv;
            }
            else {
                out.s = float(int32_t(v.s) * int32_t(textureState.sc)) * InvTexDivisor;
                out.t = float(int32_t(v.t) * int32_t(textureState.tc)) * InvTexDivisor;
            }

            indices[i] = globalIndex++;
            used[i] = false;
            slotTransformGen[i] = transformGen;
        }
    }

    void RSP::modifyVertex(uint16_t dstIndex, uint16_t dstAttribute, uint32_t value) {
        if (dstIndex >= RSP_MAX_VERTICES) {
            return;
        }

        slotTransformGen[dstIndex] = 0;

        vg::Renderer &renderer = *state->renderer;
        Workload &workload = state->ext.workloadQueue->workloads[state->ext.workloadQueue->writeCursor];
        auto &posTransformed = workload.drawData.posTransformed;
        auto &posScreen = workload.drawData.posScreen;
        uint32_t globalIndex = indices[dstIndex];
        if (used[dstIndex]) {
            // Already drawn this task: modify a copy so earlier triangles keep the old values.
            renderer.reserveVertices(1);
            posTransformed.emplace_back(posTransformed[globalIndex]);
            posScreen.emplace_back(posScreen[globalIndex]);
            const vg::Vertex copy = renderer.vertex(globalIndex);
            globalIndex = renderer.addVertex(copy);
            indices[dstIndex] = globalIndex;
            used[dstIndex] = false;
        }

        vg::Vertex &v = renderer.vertex(globalIndex);
        switch (dstAttribute) {
        case G_MWO_POINT_RGBA:
            v.r = ((value >> 24) & 0xFF) / 255.0f;
            v.g = ((value >> 16) & 0xFF) / 255.0f;
            v.b = ((value >> 8) & 0xFF) / 255.0f;
            v.a = (value & 0xFF) / 255.0f;
            break;
        case G_MWO_POINT_ST:
            v.s = int16_t((value >> 16) & 0xFFFF) / 32.0f;
            v.t = int16_t(value & 0xFFFF) / 32.0f;
            break;
        case G_MWO_POINT_XYSCREEN:
            v.x = int16_t((value >> 16) & 0xFFFF) / 4.0f;
            v.y = int16_t(value & 0xFFFF) / 4.0f;
            posScreen[globalIndex][0] = v.x;
            posScreen[globalIndex][1] = v.y;
            break;
        case G_MWO_POINT_ZSCREEN:
            v.z = value / 65536.0f;
            posScreen[globalIndex][2] = v.z;
            break;
        default:
            break;
        }
    }

    void RSP::branchZ(uint32_t branchDl, uint16_t vtxIndex, uint32_t zValue, DisplayList **dl) {
        const Workload &workload = state->ext.workloadQueue->workloads[state->ext.workloadQueue->writeCursor];
        const uint32_t globalIndex = indices[vtxIndex];
        const float screenZ = workload.drawData.posScreen[globalIndex][2] * DepthRange;
        if (extended.forceBranch || (screenZ < zValue / 65536.0f)) {
            *dl = reinterpret_cast<DisplayList *>(state->fromRDRAM(fromSegmentedMasked(branchDl))) - 1;
        }
    }

    void RSP::setGeometryMode(uint32_t mask) {
        geometryModeStack[geometryModeStackSize - 1] |= mask;
        state->updateDrawStatusAttribute(DrawAttribute::GeometryMode);
    }

    void RSP::clearGeometryMode(uint32_t mask) {
        geometryModeStack[geometryModeStackSize - 1] &= ~mask;
        state->updateDrawStatusAttribute(DrawAttribute::GeometryMode);
    }

    void RSP::setViewport(uint32_t address) {
        const uint32_t rdramAddress = fromSegmentedMasked(address);
        const Vp_t *vp = reinterpret_cast<const Vp_t *>(state->fromRDRAM(rdramAddress));
        interop::RSPViewport &viewport = viewportStack[viewportStackSize - 1];
        viewport.scale.x = float(vp->vscale[1]) / 4.0f;
        viewport.scale.y = float(vp->vscale[0]) / 4.0f;
        viewport.scale.z = float(vp->vscale[3]) / DepthRange;
        bumpTransformGen();
        viewportGen++;
        viewport.translate.x = float(vp->vtrans[1] + extended.global.viewportOffsetX) / 4.0f;
        viewport.translate.y = float(vp->vtrans[0] + extended.global.viewportOffsetY) / 4.0f;
        viewport.translate.z = float(vp->vtrans[3]) / DepthRange;
        viewportChanged = true;
    }

    void RSP::setLight(uint8_t index, uint32_t address) {
        if (index > RSP_MAX_LIGHTS) {
            return;
        }
        const uint8_t *data = reinterpret_cast<const uint8_t *>(state->fromRDRAM(fromSegmentedMasked(address)));
        memcpy(&lights[index], data, sizeof(Light));
        lightsChanged = true;
    }

    void RSP::setLightColor(uint8_t index, uint32_t value) {
        if (index > RSP_MAX_LIGHTS) {
            return;
        }
        lights[index].raw.words[0] = value;
        lights[index].raw.words[1] = value;
        lightsChanged = true;
    }

    void RSP::setLightCount(uint8_t count) {
        lightCount = count;
        lightsChanged = true;
    }

    void RSP::setClipRatioEdge(uint8_t index, int16_t value) {
        if (index < clipRatios.size()) {
            clipRatios[index] = value;
            viewportChanged = true;
        }
    }

    void RSP::setClipRatioAll(int16_t value) {
        setClipRatioEdge(0, value);
        setClipRatioEdge(1, value);
        setClipRatioEdge(2, -value);
        setClipRatioEdge(3, -value);
    }

    void RSP::setLookAt(uint8_t index, uint32_t address) {
        const DirLight *dirLight = reinterpret_cast<const DirLight *>(state->fromRDRAM(fromSegmentedMasked(address)));
        auto &dstLookAt = (index == 1) ? lookAt.y : lookAt.x;
        if ((dirLight->dirx != 0) || (dirLight->diry != 0) || (dirLight->dirz != 0)) {
            dstLookAt = hlslpp::normalize(hlslpp::float3(float(dirLight->dirx), float(dirLight->diry), float(dirLight->dirz)));
        }
        else {
            dstLookAt = { 0.0f, 0.0f, 0.0f };
        }
        lookAtChanged = true;
    }

    void RSP::setLookAtVectors(interop::float3 x, interop::float3 y) {
        lookAt.x = x;
        lookAt.y = y;
        lookAtChanged = true;
    }

    void RSP::setFog(int16_t mul, int16_t offset) {
        fog.mul = mul;
        fog.offset = offset;
        fogChanged = true;
    }

    void RSP::setTexture(uint8_t tile, uint8_t level, uint8_t on, uint16_t sc, uint16_t tc) {
        textureState.tile = tile;
        textureState.levels = level + 1;
        textureState.on = on;
        textureState.sc = sc;
        textureState.tc = tc;
        state->updateDrawStatusAttribute(DrawAttribute::Texture);
    }

    void RSP::setOtherModeL(uint32_t size, uint32_t off, uint32_t data) {
        interop::OtherMode &otherMode = otherModeStack[otherModeStackSize - 1];
        const uint32_t mask = uint32_t((((uint64_t)1 << size) - 1) << off);
        otherMode.L = (otherMode.L & (~mask)) | data;
        state->rdp->setOtherMode(otherMode.H, otherMode.L);
    }

    void RSP::setOtherModeH(uint32_t size, uint32_t off, uint32_t data) {
        interop::OtherMode &otherMode = otherModeStack[otherModeStackSize - 1];
        const uint32_t mask = uint32_t((((uint64_t)1 << size) - 1) << off);
        otherMode.H = (otherMode.H & (~mask)) | data;
        state->rdp->setOtherMode(otherMode.H, otherMode.L);
    }

    void RSP::setColorImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t segAddress) {
        state->rdp->setColorImage(fmt, siz, width, fromSegmented(segAddress));
    }

    void RSP::setDepthImage(uint32_t segAddress) {
        state->rdp->setDepthImage(fromSegmented(segAddress));
    }

    void RSP::setTextureImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t segAddress) {
        state->rdp->setTextureImage(fmt, siz, width, fromSegmented(segAddress));
    }

    // RT64's drawIndexedTri with Factor 5 cull semantics, a guard-band clip and the dome rules. The parser only records the triangle;
    // culling and clipping run on the resolve stage (vg::buildBatchVertices), with the rules from triRules().
    void RSP::drawIndexedTri(uint32_t a, uint32_t b, uint32_t c, bool rawGlobalIndices) {
        vg::ProfScope prof(vg::PROF_DRAW_TRI);
        const uint32_t geometryMode = geometryModeStack[geometryModeStackSize - 1];
        static const bool s_cullbothdraw = envFlag("ROGUESQ_F5_CULLBOTH_DRAW", true);
        if (!f5Cull && !s_cullbothdraw && (geometryMode & cullBothMask) == cullBothMask) {
            return;
        }

        if (!f5Cull && (geometryMode & cullBothMask) == cullFrontMask) {
            std::swap(a, c);
        }

        if (rawGlobalIndices) {
            state->renderer->recordTriangle(a, b, c);
            return;
        }

        used[a] = used[b] = used[c] = true;
        state->renderer->recordTriangle(indices[a], indices[b], indices[c]);
    }

    vg::TriRules RSP::triRules() const {
        static const bool s_cullbothdraw = envFlag("ROGUESQ_F5_CULLBOTH_DRAW", true);
        static const bool s_nocull = envFlag("ROGUESQ_SKY_NOCULL", false);
        static const bool s_clip = !envFlag("ROGUESQ_F5_NO_NEAR_CLIP", false);
        static const bool s_pinAll = envFlag("ROGUESQ_F5_SKY_PIN_ALL", false);
        static const bool s_domeNearDrop = envFlag("ROGUESQ_F5_DOME_NEAR_DROP", true);
        vg::TriRules r;
        r.cullBothMask = cullBothMask;
        r.cullFrontMask = cullFrontMask;
        r.f5Cull = f5Cull;
        r.cullBothDraw = s_cullbothdraw;
        r.noCull = s_nocull;
        r.clip = s_clip;
        r.pinAll = s_pinAll;
        r.domeNearDrop = s_domeNearDrop;
        return r;
    }

    void RSP::drawIndexedTri(uint32_t a, uint32_t b, uint32_t c) {
        drawIndexedTri(a, b, c, false);
    }

    void RSP::matrixId(uint32_t id, bool push, bool proj, bool, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, bool idIsAddress, bool) {
        // Transform ids only drive RT64's frame interpolation; keep the stacks balanced for popMatrixId.
        if (idIsAddress) {
            return;
        }
        auto &stack = proj ? extended.viewProjMatrixIdStack : extended.modelMatrixIdStack;
        int &stackSize = proj ? extended.viewProjMatrixIdStackSize : extended.modelMatrixIdStackSize;
        if (push && size_t(stackSize) < stack.size()) {
            stackSize++;
        }
        stack[stackSize - 1].matrixId = id;
    }

    void RSP::popMatrixId(uint8_t count, bool proj) {
        int &stackSize = proj ? extended.viewProjMatrixIdStackSize : extended.modelMatrixIdStackSize;
        while ((count > 0) && (stackSize > 1)) {
            count--;
            stackSize--;
        }
    }

    void RSP::clearExtended() {
        extended.drawExtendedType = DrawExtendedType::None;
        extended.drawExtendedData = {};
        extended.viewportOriginStack[0] = G_EX_ORIGIN_NONE;
        extended.global.viewportOrigin = G_EX_ORIGIN_NONE;
        extended.global.viewportOffsetX = 0;
        extended.global.viewportOffsetY = 0;
        extended.modelMatrixIdStack[0] = TransformGroup();
        extended.modelMatrixIdStackSize = 1;
        extended.modelMatrixIdStackChanged = false;
        extended.curModelMatrixIdGroupIndex = 0;
        extended.viewProjMatrixIdStack[0] = TransformGroup();
        extended.viewProjMatrixIdStackSize = 1;
        extended.viewProjMatrixIdStackChanged = false;
        extended.curViewProjMatrixIdGroupIndex = 0;
        extended.viewMatrix = hlslpp::float4x4::identity();
        extended.projMatrix = hlslpp::float4x4::identity();
        extended.viewProjMatrix = hlslpp::float4x4::identity();
        extended.invViewMatrix = hlslpp::float4x4::identity();
        extended.invProjMatrix = hlslpp::float4x4::identity();
        extended.invViewProjMatrix = hlslpp::float4x4::identity();
        extended.vertexAddresses = {};
        extended.baseSegmentAddresses = {};
        extended.vertexSegmentEnabled = {};
        extended.forceBranch = false;
    }

    void RSP::setGBI(GBI *gbi) {
        NoN = gbi->flags.NoN;
        if (gbi->ucode == GBIUCode::F3DFACTOR5) {
            if (envFlag("ROGUESQ_F5_NON", false)) {
                NoN = true;
            }
            f5Cull = envFlag("ROGUESQ_F5_CULL", true);
        }
        else {
            f5Cull = false;
        }
        cullBothMask = gbi->constants[F3DENUM::G_CULL_BOTH];
        cullFrontMask = gbi->constants[F3DENUM::G_CULL_FRONT];
        projMask = gbi->constants[F3DENUM::G_MTX_PROJECTION];
        loadMask = gbi->constants[F3DENUM::G_MTX_LOAD];
        pushMask = gbi->constants[F3DENUM::G_MTX_PUSH];
        shadingSmoothMask = gbi->constants[F3DENUM::G_SHADING_SMOOTH];
    }
};
