// RDP side of the Vita renderer: RT64's RDP (lib/rt64/src/hle/rt64_rdp.cpp) without the framebuffer manager.
// Texture loads are applied to TMEM immediately (RT64 defers them to the GPU); the open batch is flushed first so it
// samples the TMEM contents it was recorded with.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../include/rt64_extended_gbi.h"
#include "gbi/rt64_f3d.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"

#include "vg_prof.h"
#include "vg_renderer.h"

namespace RT64 {
    static uint8_t getCommandLength(uint32_t commandId) {
        if (commandId == (G_TEXRECT & 0x3F) || commandId == (G_TEXRECTFLIP & 0x3F)) {
            return 2;
        }
        else if (commandId >= G_RDPTRI_BASE && commandId <= (uint32_t)RDPTriangle::MaxValue) {
            const bool shaded = (commandId & (uint32_t)RDPTriangle::Shaded) != 0;
            const bool textured = (commandId & (uint32_t)RDPTriangle::Textured) != 0;
            const bool zBuffered = (commandId & (uint32_t)RDPTriangle::Depth) != 0;
            uint8_t commandLength = triangleBaseWords;
            if (shaded) {
                commandLength += triangleShadeWords;
            }
            if (textured) {
                commandLength += triangleTexWords;
            }
            if (zBuffered) {
                commandLength += triangleDepthWords;
            }
            return commandLength;
        }
        else {
            return 1;
        }
    }

    RDP::RDP(State *state) : state(state) {
        memset(TMEM, 0, sizeof(TMEM));
        crashed = false;
        crashReason = CrashReason::None;
        gbi = nullptr;
        for (uint32_t commandId = 0; commandId < commandWordLengths.size(); commandId++) {
            commandWordLengths[commandId] = getCommandLength(commandId);
        }
    }

    void RDP::setGBI() {
        gbi = state->ext.interpreter->hleGBI;
    }

    void RDP::reset() {
        colorCombinerStackSize = 1;
        envColorStackSize = 1;
        primColorStackSize = 1;
        primDepthStackSize = 1;
        blendColorStackSize = 1;
        fogColorStackSize = 1;
        fillColorStackSize = 1;
        scissorStackSize = 1;
        colorCombinerStack[0] = { 0, 0 };
        otherMode = { 0, 0 };
        envColorStack[0] = { 0.0f, 0.0f, 0.0f, 0.0f };
        primColorStack[0] = { 0.0f, 0.0f, 0.0f, 0.0f };
        primLODStack[0] = { 0.0f, 0.0f };
        primDepthStack[0] = { 0.0f, 0.0f };
        fogColorStack[0] = { 0.0f, 0.0f, 0.0f, 0.0f };
        fillColorStack[0] = 0;
        blendColorStack[0] = { 0.0f, 0.0f, 0.0f, 0.0f };
        keyCenter = { 0.0f, 0.0f, 0.0f };
        keyScale = { 0.0f, 0.0f, 0.0f };
        for (int32_t &k : convertK) {
            k = 0;
        }
        scissorRectStack[0] = { 0, 0, 8192, 8192 };
        scissorModeStack[0] = 0;
        pendingCommandCurrentBytes = 0;
        pendingCommandRemainingBytes = 0;
        clearExtended();
    }

    void RDP::crash(CrashReason reason) {
        crashReason = reason;
        crashed = true;
    }

    void RDP::checkFramebufferPair() { }

    void RDP::checkFramebufferOverlap(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool, bool) { }

    void RDP::checkImageOverlap(uint32_t, uint32_t) { }

    int32_t RDP::movedFromOrigin(int32_t x, uint16_t ori) {
        if (ori < G_EX_ORIGIN_NONE) {
            return x + ((ori * colorImage.width * 4) / G_EX_ORIGIN_RIGHT);
        }
        return x;
    }

    uint32_t RDP::maskAddress(uint32_t address) {
        return address & RDP_ADDRESS_MASK;
    }

    void RDP::setColorImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) {
        const uint32_t newAddress = maskAddress(address);
        if ((colorImage.fmt != fmt) || (colorImage.siz != siz) || (colorImage.width != width) || (colorImage.address != newAddress)) {
            state->renderer->flush();
            colorImage.fmt = fmt;
            colorImage.siz = siz;
            colorImage.width = width;
            colorImage.address = newAddress;
            colorImage.changed = true;
        }
    }

    void RDP::setDepthImage(uint32_t address) {
        const uint32_t newAddress = maskAddress(address);
        if (depthImage.address != newAddress) {
            depthImage.address = newAddress;
            depthImage.changed = true;
        }
    }

    void RDP::setTextureImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) {
        texture.fmt = fmt;
        texture.siz = siz;
        texture.width = width;
        texture.address = maskAddress(address);
    }

    void RDP::setCombine(uint64_t combine) {
        const interop::uint combineL = combine & 0xFFFFFFFFULL;
        const interop::uint combineH = (combine >> 32ULL) & 0xFFFFFFFFULL;
        interop::ColorCombiner &colorCombiner = colorCombinerStack[colorCombinerStackSize - 1];
        if (colorCombiner.L != combineL || colorCombiner.H != combineH) {
            colorCombiner.L = combineL;
            colorCombiner.H = combineH;
            state->updateDrawStatusAttribute(DrawAttribute::Combine);
        }
    }

    void RDP::pushCombine() {
        if (colorCombinerStackSize < RDP_EXTENDED_STACK_SIZE) {
            colorCombinerStack[colorCombinerStackSize] = colorCombinerStack[colorCombinerStackSize - 1];
            colorCombinerStackSize++;
        }
    }

    void RDP::popCombine() {
        if (colorCombinerStackSize > 1) {
            colorCombinerStackSize--;
            state->updateDrawStatusAttribute(DrawAttribute::Combine);
        }
    }

    void RDP::setTile(uint8_t tile, uint8_t fmt, uint8_t siz, uint16_t line, uint16_t tmem, uint8_t palette, uint8_t cmt, uint8_t cms, uint8_t maskt, uint8_t masks, uint8_t shiftt, uint8_t shifts) {
        auto &t = tiles[tile & (RDP_TILES - 1)];
        t.fmt = fmt;
        t.siz = siz;
        t.line = line;
        t.tmem = tmem;
        t.palette = palette;
        t.cmt = cmt;
        t.cms = cms;
        t.masks = masks;
        t.maskt = maskt;
        t.shifts = shifts;
        t.shiftt = shiftt;
        state->updateDrawStatusAttribute(DrawAttribute::Texture);
    }

    void RDP::setTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
        auto &t = tiles[tile & (RDP_TILES - 1)];
        t.uls = uls;
        t.ult = ult;
        t.lrs = lrs;
        t.lrt = lrt;
        state->updateDrawStatusAttribute(DrawAttribute::Texture);
    }

    void RDP::clearTileReplacementHash(uint8_t tile) {
        tileReplacementHashes[tile & (RDP_TILES - 1)] = 0;
    }

    void RDP::setTileReplacementHash(uint8_t tile, uint64_t replacementHash) {
        tileReplacementHashes[tile & (RDP_TILES - 1)] = replacementHash;
    }

    // ROGUESQ_VG_FAST_TMEM=0 falls back to RT64's byte-by-byte word copy.
    static const bool s_fastTmem = []() {
        const char *v = std::getenv("ROGUESQ_VG_FAST_TMEM");
        return !(v && (v[0] == '0'));
    }();

    // ROGUESQ_VG_TMEM_VERIFY=1: every load is repeated with RT64's byte loop on a copy of TMEM and its tags, and differences are logged.
    static const bool s_verifyTmem = []() {
        const char *v = std::getenv("ROGUESQ_VG_TMEM_VERIFY");
        return v && (v[0] == '1');
    }();

    // REF forces RT64's byte loop (the verification reference).
    template<bool RGBA32 = false, bool TLUT = false, bool REF = false>
    __forceinline void loadWord(uint8_t *TMEM, uint32_t tmemAddress, uint32_t tmemXorMask, const uint8_t *RDRAM, uint32_t textureAddress) {
        const uint32_t offsetMask = TLUT ? 0x1 : 0x7;
        if constexpr (RGBA32 && !TLUT && !REF) {
            if (s_fastTmem && ((textureAddress & 3) == 0) && ((tmemAddress & 3) == 0)) {
                // Bytes 0,1,4,5 (R and G of both texels) go to the lower half and 2,3,6,7 (B and A) to the upper half, each run of 4 contiguous.
                uint32_t w0, w1;
                memcpy(&w0, RDRAM + textureAddress, 4);
                memcpy(&w1, RDRAM + textureAddress + 4, 4);
                const uint32_t a = __builtin_bswap32(w0);
                const uint32_t b = __builtin_bswap32(w1);
                const uint32_t lower = (a & 0xFFFF) | (b << 16);
                const uint32_t upper = (a >> 16) | (b & 0xFFFF0000);
                const uint32_t dst = tmemAddress ^ tmemXorMask;
                memcpy(TMEM + dst, &lower, 4);
                memcpy(TMEM + (dst | (RDP_TMEM_BYTES >> 1)), &upper, 4);
                return;
            }
        }
        if constexpr (RGBA32) {
            const uint32_t UpperTMEM = (RDP_TMEM_BYTES >> 1);
            TMEM[(tmemAddress + 0) ^ tmemXorMask] = RDRAM[(textureAddress + (0 & offsetMask)) ^ 3];
            TMEM[(tmemAddress + 1) ^ tmemXorMask] = RDRAM[(textureAddress + (1 & offsetMask)) ^ 3];
            TMEM[(tmemAddress + 2) ^ tmemXorMask] = RDRAM[(textureAddress + (4 & offsetMask)) ^ 3];
            TMEM[(tmemAddress + 3) ^ tmemXorMask] = RDRAM[(textureAddress + (5 & offsetMask)) ^ 3];
            TMEM[((tmemAddress + 0) ^ tmemXorMask) | UpperTMEM] = RDRAM[(textureAddress + (2 & offsetMask)) ^ 3];
            TMEM[((tmemAddress + 1) ^ tmemXorMask) | UpperTMEM] = RDRAM[(textureAddress + (3 & offsetMask)) ^ 3];
            TMEM[((tmemAddress + 2) ^ tmemXorMask) | UpperTMEM] = RDRAM[(textureAddress + (6 & offsetMask)) ^ 3];
            TMEM[((tmemAddress + 3) ^ tmemXorMask) | UpperTMEM] = RDRAM[(textureAddress + (7 & offsetMask)) ^ 3];
        }
        else if (!REF && !TLUT && s_fastTmem && ((textureAddress & 3) == 0) && ((tmemAddress & 3) == 0)) {
            // RDRAM keeps each big-endian word byte-swapped in a host word, so a word-aligned copy is one bswap per 4 bytes.
            uint32_t w0, w1;
            memcpy(&w0, RDRAM + textureAddress, 4);
            memcpy(&w1, RDRAM + textureAddress + 4, 4);
            w0 = __builtin_bswap32(w0);
            w1 = __builtin_bswap32(w1);
            memcpy(TMEM + (tmemAddress ^ tmemXorMask), &w0, 4);
            memcpy(TMEM + ((tmemAddress + 4) ^ tmemXorMask), &w1, 4);
        }
        else if (!REF && TLUT && s_fastTmem && ((tmemAddress & 3) == 0)) {
            // A palette entry fills its 8 TMEM bytes four times over (hi, lo, hi, lo, ...), so the XOR swizzle leaves them unchanged.
            const uint32_t hi = RDRAM[textureAddress ^ 3], lo = RDRAM[(textureAddress + 1) ^ 3];
            const uint32_t v = hi | (lo << 8) | (hi << 16) | (lo << 24);
            memcpy(TMEM + (tmemAddress ^ tmemXorMask), &v, 4);
            memcpy(TMEM + ((tmemAddress + 4) ^ tmemXorMask), &v, 4);
        }
        else {
            for (uint32_t i = 0; i < 8; i++) {
                TMEM[(tmemAddress + i) ^ tmemXorMask] = RDRAM[(textureAddress + (i & offsetMask)) ^ 3];
            }
        }
    }

    // Verbatim from RT64 except that RDRAM addresses are masked to 24 bits (the Vita RDRAM block is 32 MB).
    template<bool RGBA32 = false, bool BLOCK = false, bool TLUT = false, bool REF = false>
    __forceinline void loadToTMEMCommon(uint8_t *TMEM, const uint8_t *RDRAM, uint32_t textureStart, uint32_t textureStride, uint32_t tmemStart,
        uint32_t tmemStride, uint32_t wordsPerRow, uint32_t rowCount, uint32_t dxtIncrement, uint32_t *wordTags, uint32_t tag)
    {
        const uint32_t DXTSwap = 0x800;
        const uint32_t tmemMask = RGBA32 ? RDP_TMEM_MASK16 : RDP_TMEM_MASK8;
        const uint32_t tmemAdvance = RGBA32 ? 0x4 : 0x8;
        const uint32_t textureAdvance = TLUT ? 0x2 : 0x8;
        uint32_t textureAddress = 0, tmemAddress = 0, wordCount = 0;
        uint32_t tmemXorMask = 0x0;
        uint32_t dxtCounter = 0x0;
        auto loadWordStep = [&]() {
            if constexpr (BLOCK) {
                dxtCounter += dxtIncrement;
                while (dxtCounter >= DXTSwap) {
                    tmemAddress = (tmemAddress + tmemStride) & tmemMask;
                    dxtCounter -= DXTSwap;
                    tmemXorMask ^= 0x4;
                }
            }
            textureAddress += textureAdvance;
            tmemAddress = (tmemAddress + tmemAdvance) & tmemMask;
        };

        uint32_t textureAddressRow = textureStart;
        uint32_t tmemAddressRow = tmemStart & tmemMask;
        auto loadRowStep = [&]() {
            tmemAddressRow = (tmemAddressRow + tmemStride) & tmemMask;
            textureAddressRow += textureStride;
            tmemXorMask ^= 0x4;
        };

        if ((tmemStride > 0) && ((wordsPerRow * tmemAdvance) <= tmemStride)) {
            int32_t rowsToSkip = rowCount - (tmemMask + tmemStride) / tmemStride;
            if (rowsToSkip > 0) {
                // RT64 adds the start row twice here ("+="), which can index past the 4 KB TMEM array.
                tmemAddressRow = (tmemAddressRow + tmemStride * rowsToSkip) & tmemMask;
                textureAddressRow += textureStride * rowsToSkip;
                tmemXorMask = (rowsToSkip & 0x1) << 2;
                rowCount -= rowsToSkip;
            }
        }

        while (rowCount > 0) {
            textureAddress = textureAddressRow;
            tmemAddress = tmemAddressRow;
            wordCount = wordsPerRow;
            while (wordCount > 0) {
                loadWord<RGBA32, TLUT, REF>(TMEM, tmemAddress, tmemXorMask, RDRAM, textureAddress & RDP_ADDRESS_MASK);
                wordTags[((tmemAddress ^ tmemXorMask) & RDP_TMEM_MASK8) >> 3] = tag;
                if constexpr (RGBA32) {
                    wordTags[(((tmemAddress ^ tmemXorMask) | (RDP_TMEM_BYTES >> 1)) & RDP_TMEM_MASK8) >> 3] = tag;
                }
                loadWordStep();
                wordCount--;
            }
            loadRowStep();
            rowCount--;
        }
    }

    // Non-RGBA32 load block: between DXT row steps the words land back to back in TMEM under one XOR, so each run is copied as
    // byte-swapped 32-bit halves (swapped when the XOR is set) and its tags are filled together. False when the fast path does not apply.
    static bool loadBlockFast(uint8_t *TMEM, const uint8_t *RDRAM, uint32_t textureStart, uint32_t tmemStart, uint32_t tmemStride,
        uint32_t wordCount, uint32_t dxt, uint32_t *wordTags, uint32_t tag)
    {
        constexpr uint32_t DXTSwap = 0x800;
        constexpr uint32_t TmemMask = RDP_TMEM_MASK8;
        uint32_t textureAddress = textureStart & RDP_ADDRESS_MASK;
        uint32_t tmemAddress = tmemStart & TmemMask;
        if (((textureAddress & 3) != 0) || ((tmemAddress & 7) != 0) || ((tmemStride & 7) != 0) ||
            ((uint64_t(textureAddress) + uint64_t(wordCount) * 8) > (uint64_t(RDP_ADDRESS_MASK) + 1)))
        {
            return false;
        }

        uint32_t xorMask = 0, dxtCounter = 0;
        while (wordCount > 0) {
            uint32_t run = std::min(wordCount, (TmemMask + 1 - tmemAddress) >> 3);
            if (dxt > 0) {
                run = std::min(run, (DXTSwap - dxtCounter + dxt - 1) / dxt);
            }
            const uint8_t *src = RDRAM + textureAddress;
            uint8_t *dst = TMEM + tmemAddress;
            if (xorMask == 0) {
                for (uint32_t i = 0; i < run; i++) {
                    uint32_t a, b;
                    memcpy(&a, src + i * 8, 4);
                    memcpy(&b, src + i * 8 + 4, 4);
                    a = __builtin_bswap32(a);
                    b = __builtin_bswap32(b);
                    memcpy(dst + i * 8, &a, 4);
                    memcpy(dst + i * 8 + 4, &b, 4);
                }
            }
            else {
                for (uint32_t i = 0; i < run; i++) {
                    uint32_t a, b;
                    memcpy(&a, src + i * 8, 4);
                    memcpy(&b, src + i * 8 + 4, 4);
                    a = __builtin_bswap32(a);
                    b = __builtin_bswap32(b);
                    memcpy(dst + i * 8, &b, 4);
                    memcpy(dst + i * 8 + 4, &a, 4);
                }
            }
            uint32_t *runTags = wordTags + (tmemAddress >> 3);
            for (uint32_t i = 0; i < run; i++) {
                runTags[i] = tag;
            }

            textureAddress += run * 8;
            tmemAddress = (tmemAddress + run * 8) & TmemMask;
            dxtCounter += run * dxt;
            while (dxtCounter >= DXTSwap) {
                tmemAddress = (tmemAddress + tmemStride) & TmemMask;
                dxtCounter -= DXTSwap;
                xorMask ^= 0x4;
            }
            wordCount -= run;
        }
        return true;
    }

    struct TmemCopy {
        uint8_t bytes[RDP_TMEM_BYTES];
        uint32_t tags[RDP_TMEM_WORDS];
    };

    // Runs `load` on TMEM; under ROGUESQ_VG_TMEM_VERIFY also runs `reference` on a copy taken before and logs any difference.
    template<typename Load, typename Reference>
    static void checkedLoad(const char *kind, uint8_t *TMEM, uint32_t *tags, Load load, Reference reference) {
        if (!s_verifyTmem) {
            load(TMEM, tags);
            return;
        }

        static TmemCopy ref;
        static uint32_t loads = 0, mismatches = 0;
        memcpy(ref.bytes, TMEM, sizeof(ref.bytes));
        memcpy(ref.tags, tags, sizeof(ref.tags));
        load(TMEM, tags);
        reference(ref.bytes, ref.tags);
        loads++;
        const bool bytesSame = memcmp(ref.bytes, TMEM, sizeof(ref.bytes)) == 0;
        const bool tagsSame = memcmp(ref.tags, tags, sizeof(ref.tags)) == 0;
        if ((!bytesSame || !tagsSame) && (++mismatches <= 10)) {
            fprintf(stderr, "[tmem-verify] %s load #%u differs (bytes %d, tags %d)\n", kind, loads, bytesSame ? 0 : 1, tagsSame ? 0 : 1);
        }
        if ((loads % 20000) == 0) {
            fprintf(stderr, "[tmem-verify] %u loads checked, %u differ\n", loads, mismatches);
        }
    }

    void RDP::loadTileOperation(const LoadTile &loadTile, const LoadTexture &loadTexture, bool) {
        vg::ProfScope prof(vg::PROF_TMEM_LOAD);
        const uint32_t bytesOffset = (loadTile.uls >> 2) << loadTexture.siz >> 1;
        const uint32_t bytesPerRow = loadTexture.width << loadTexture.siz >> 1;
        const uint32_t textureStart = loadTexture.address + bytesOffset + bytesPerRow * (loadTile.ult >> 2);
        const uint32_t rowCount = 1 + ((loadTile.lrt >> 2) - (loadTile.ult >> 2));
        const uint32_t tileWidth = ((loadTile.lrs >> 2) - (loadTile.uls >> 2));
        const uint32_t wordsPerRow = (tileWidth >> (4 - loadTile.siz)) + 1;
        const uint32_t tmemStart = loadTile.tmem << 3;
        const uint32_t tmemStride = loadTile.line << 3;
        const bool RGBA32 = (loadTile.siz == G_IM_SIZ_32b) && (loadTile.fmt == G_IM_FMT_RGBA);
        const uint32_t tag = ++tmemLoadTag;
        const uint8_t *RDRAM = state->RDRAM;
        checkedLoad("tile", reinterpret_cast<uint8_t *>(TMEM), tmemWordTags,
            [&](uint8_t *T, uint32_t *tags) {
                if (RGBA32) {
                    loadToTMEMCommon<true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordsPerRow, rowCount, 0, tags, tag);
                }
                else {
                    loadToTMEMCommon<false>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordsPerRow, rowCount, 0, tags, tag);
                }
            },
            [&](uint8_t *T, uint32_t *tags) {
                if (RGBA32) {
                    loadToTMEMCommon<true, false, false, true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordsPerRow, rowCount, 0, tags, tag);
                }
                else {
                    loadToTMEMCommon<false, false, false, true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordsPerRow, rowCount, 0, tags, tag);
                }
            });
    }

    void RDP::loadBlockOperation(const LoadTile &loadTile, const LoadTexture &loadTexture, bool) {
        vg::ProfScope prof(vg::PROF_TMEM_LOAD);
        const uint32_t bytesOffset = loadTile.uls << loadTexture.siz >> 1;
        const uint32_t bytesPerRow = loadTexture.width << loadTexture.siz >> 1;
        const uint32_t textureStart = loadTexture.address + bytesOffset + bytesPerRow * loadTile.ult;
        const uint32_t wordCount = ((loadTile.lrs - loadTile.uls) >> (4 - loadTile.siz)) + 1;
        const uint32_t tmemStart = loadTile.tmem << 3;
        const uint32_t tmemStride = loadTile.line << 3;
        const uint32_t dxt = loadTile.lrt;
        const bool RGBA32 = (loadTile.siz == G_IM_SIZ_32b) && (loadTile.fmt == G_IM_FMT_RGBA);
        const uint32_t tag = ++tmemLoadTag;
        const uint8_t *RDRAM = state->RDRAM;
        checkedLoad("block", reinterpret_cast<uint8_t *>(TMEM), tmemWordTags,
            [&](uint8_t *T, uint32_t *tags) {
                if (RGBA32) {
                    loadToTMEMCommon<true, true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordCount, 1, dxt, tags, tag);
                }
                else if (!s_fastTmem || !loadBlockFast(T, RDRAM, textureStart, tmemStart, tmemStride, wordCount, dxt, tags, tag)) {
                    loadToTMEMCommon<false, true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordCount, 1, dxt, tags, tag);
                }
            },
            [&](uint8_t *T, uint32_t *tags) {
                if (RGBA32) {
                    loadToTMEMCommon<true, true, false, true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordCount, 1, dxt, tags, tag);
                }
                else {
                    loadToTMEMCommon<false, true, false, true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordCount, 1, dxt, tags, tag);
                }
            });
    }

    void RDP::loadTLUTOperation(const LoadTile &loadTile, const LoadTexture &loadTexture, bool) {
        vg::ProfScope prof(vg::PROF_TMEM_LOAD);
        const uint32_t bytesOffset = (loadTile.uls >> 2) << loadTexture.siz >> 1;
        const uint32_t bytesPerRow = loadTexture.width << loadTexture.siz >> 1;
        const uint32_t textureStart = loadTexture.address + bytesOffset + bytesPerRow * (loadTile.ult >> 2);
        const uint32_t rowCount = 1 + ((loadTile.lrt >> 2) - (loadTile.ult >> 2));
        const uint32_t wordsPerRow = ((loadTile.lrs >> 2) - (loadTile.uls >> 2)) + 1;
        const uint32_t tmemStart = loadTile.tmem << 3;
        const uint32_t tmemStride = loadTile.line << 5;
        const bool RGBA32 = (loadTile.siz == G_IM_SIZ_32b) && (loadTile.fmt == G_IM_FMT_RGBA);
        const uint32_t tag = ++tmemLoadTag;
        const uint8_t *RDRAM = state->RDRAM;
        checkedLoad("tlut", reinterpret_cast<uint8_t *>(TMEM), tmemWordTags,
            [&](uint8_t *T, uint32_t *tags) {
                if (RGBA32) {
                    loadToTMEMCommon<true, false, true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordsPerRow, rowCount, 0, tags, tag);
                }
                else {
                    loadToTMEMCommon<false, false, true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordsPerRow, rowCount, 0, tags, tag);
                }
            },
            [&](uint8_t *T, uint32_t *tags) {
                if (RGBA32) {
                    loadToTMEMCommon<true, false, true, true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordsPerRow, rowCount, 0, tags, tag);
                }
                else {
                    loadToTMEMCommon<false, false, true, true>(T, RDRAM, textureStart, bytesPerRow, tmemStart, tmemStride, wordsPerRow, rowCount, 0, tags, tag);
                }
            });
    }

    void RDP::loadTile(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
        auto &t = tiles[tile & (RDP_TILES - 1)];
        t.uls = uls;
        t.ult = ult;
        t.lrs = lrs;
        t.lrt = lrt;
        if (t.uls > t.lrs) {
            return;
        }
        state->renderer->flush();
        loadTileOperation(t, texture, false);
        state->updateDrawStatusAttribute(DrawAttribute::Texture);
    }

    bool RDP::loadTileCopyCheck(uint8_t, uint16_t, uint16_t, uint16_t, uint16_t) {
        return false;
    }

    bool RDP::loadTileReplacementCheck(uint8_t, uint16_t, uint16_t, uint16_t, uint16_t, uint8_t, uint8_t, uint16_t, uint16_t, uint64_t &) {
        return false;
    }

    void RDP::loadBlock(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t dxt) {
        auto &t = tiles[tile & (RDP_TILES - 1)];
        t.uls = uls;
        t.ult = ult;
        t.lrs = lrs;
        t.lrt = dxt;
        if ((t.uls > t.lrs) || (t.lrs >= 0x800)) {
            return;
        }
        state->renderer->flush();
        loadBlockOperation(t, texture, false);
        state->updateDrawStatusAttribute(DrawAttribute::Texture);
    }

    void RDP::loadTLUT(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
        auto &t = tiles[tile & (RDP_TILES - 1)];
        t.uls = uls;
        t.ult = ult;
        t.lrs = lrs;
        t.lrt = lrt;
        state->renderer->flush();
        loadTLUTOperation(t, texture, false);
        state->updateDrawStatusAttribute(DrawAttribute::Texture);
    }

    static void unpackColor(hlslpp::float4 &dst, uint32_t color) {
        dst.x = ((color >> 24) & 0xFF) / 255.0f;
        dst.y = ((color >> 16) & 0xFF) / 255.0f;
        dst.z = ((color >> 8) & 0xFF) / 255.0f;
        dst.w = ((color >> 0) & 0xFF) / 255.0f;
    }

    void RDP::setEnvColor(uint32_t color) {
        unpackColor(envColorStack[envColorStackSize - 1], color);
        state->updateDrawStatusAttribute(DrawAttribute::EnvColor);
    }

    void RDP::pushEnvColor() {
        if (envColorStackSize < RDP_EXTENDED_STACK_SIZE) {
            envColorStack[envColorStackSize] = envColorStack[envColorStackSize - 1];
            envColorStackSize++;
        }
    }

    void RDP::popEnvColor() {
        if (envColorStackSize > 1) {
            envColorStackSize--;
            state->updateDrawStatusAttribute(DrawAttribute::EnvColor);
        }
    }

    void RDP::setPrimColor(uint8_t lodFrac, uint8_t lodMin, uint32_t color) {
        hlslpp::float2 &primLOD = primLODStack[primColorStackSize - 1];
        primLOD.x = lodFrac / 256.0f;
        primLOD.y = lodMin / 32.0f;
        unpackColor(primColorStack[primColorStackSize - 1], color);
        state->updateDrawStatusAttribute(DrawAttribute::PrimColor);
    }

    void RDP::pushPrimColor() {
        if (primColorStackSize < RDP_EXTENDED_STACK_SIZE) {
            primColorStack[primColorStackSize] = primColorStack[primColorStackSize - 1];
            primLODStack[primColorStackSize] = primLODStack[primColorStackSize - 1];
            primColorStackSize++;
        }
    }

    void RDP::popPrimColor() {
        if (primColorStackSize > 1) {
            primColorStackSize--;
            state->updateDrawStatusAttribute(DrawAttribute::PrimColor);
        }
    }

    void RDP::setBlendColor(uint32_t color) {
        unpackColor(blendColorStack[blendColorStackSize - 1], color);
        state->updateDrawStatusAttribute(DrawAttribute::BlendColor);
    }

    void RDP::pushBlendColor() {
        if (blendColorStackSize < RDP_EXTENDED_STACK_SIZE) {
            blendColorStack[blendColorStackSize] = blendColorStack[blendColorStackSize - 1];
            blendColorStackSize++;
        }
    }

    void RDP::popBlendColor() {
        if (blendColorStackSize > 1) {
            blendColorStackSize--;
            state->updateDrawStatusAttribute(DrawAttribute::BlendColor);
        }
    }

    void RDP::setFogColor(uint32_t color) {
        unpackColor(fogColorStack[fogColorStackSize - 1], color);
        state->updateDrawStatusAttribute(DrawAttribute::FogColor);
    }

    void RDP::pushFogColor() {
        if (fogColorStackSize < RDP_EXTENDED_STACK_SIZE) {
            fogColorStack[fogColorStackSize] = fogColorStack[fogColorStackSize - 1];
            fogColorStackSize++;
        }
    }

    void RDP::popFogColor() {
        if (fogColorStackSize > 1) {
            fogColorStackSize--;
            state->updateDrawStatusAttribute(DrawAttribute::FogColor);
        }
    }

    void RDP::setFillColor(uint32_t color) {
        fillColorStack[fillColorStackSize - 1] = color;
        state->updateDrawStatusAttribute(DrawAttribute::FillColor);
    }

    void RDP::pushFillColor() {
        if (fillColorStackSize < RDP_EXTENDED_STACK_SIZE) {
            fillColorStack[fillColorStackSize] = fillColorStack[fillColorStackSize - 1];
            fillColorStackSize++;
        }
    }

    void RDP::popFillColor() {
        if (fillColorStackSize > 1) {
            fillColorStackSize--;
            state->updateDrawStatusAttribute(DrawAttribute::FillColor);
        }
    }

    void RDP::setOtherMode(uint32_t high, uint32_t low) {
        if (otherMode.H != high || otherMode.L != low) {
            otherMode.H = high;
            otherMode.L = low;
            state->updateDrawStatusAttribute(DrawAttribute::OtherMode);
        }
    }

    void RDP::setPrimDepth(uint16_t z, uint16_t dz) {
        hlslpp::float2 &primDepth = primDepthStack[primDepthStackSize - 1];
        primDepth.x = (z & 0x7FFFU) * (1.0f / 32767.0f);
        primDepth.y = (dz & 0xFFFFU) * (1.0f / 65535.0f);
        state->updateDrawStatusAttribute(DrawAttribute::PrimDepth);
    }

    void RDP::setScissor(uint8_t mode, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
        setScissor(mode, ulx, uly, lrx, lry, extended.global.scissor);
    }

    void RDP::setScissor(uint8_t mode, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, const ExtendedAlignment &extAlignment) {
        FixedRect &scissorRect = scissorRectStack[scissorStackSize - 1];
        scissorRect.ulx = std::clamp(movedFromOrigin(ulx + extAlignment.leftOffset, extAlignment.leftOrigin), extAlignment.leftBound, extAlignment.rightBound);
        scissorRect.uly = std::clamp(uly + extAlignment.topOffset, extAlignment.topBound, extAlignment.bottomBound);
        scissorRect.lrx = std::clamp(movedFromOrigin(lrx + extAlignment.rightOffset, extAlignment.rightOrigin), extAlignment.leftBound, extAlignment.rightBound);
        scissorRect.lry = std::clamp(lry + extAlignment.bottomOffset, extAlignment.topBound, extAlignment.bottomBound);
        scissorModeStack[scissorStackSize - 1] = mode;
        // ROGUESQ_VG_LOG_SCISSOR=1: each distinct scissor with the color image it applies to.
        static const bool s_logScissor = []() { const char *v = std::getenv("ROGUESQ_VG_LOG_SCISSOR"); return v && (v[0] == '1'); }();
        if (s_logScissor) {
            static uint64_t s_last = ~0ull;
            const uint64_t key = (uint64_t(scissorRect.ulx) << 48) ^ (uint64_t(scissorRect.uly) << 32) ^ (uint64_t(scissorRect.lrx) << 16) ^ uint64_t(scissorRect.lry) ^ (uint64_t(colorImage.address) << 8);
            if (key != s_last) {
                s_last = key;
                fprintf(stderr, "[vg-scissor] cimg 0x%06X w %u: (%d,%d)-(%d,%d)\n", colorImage.address, colorImage.width,
                    scissorRect.ulx >> 2, scissorRect.uly >> 2, scissorRect.lrx >> 2, scissorRect.lry >> 2);
            }
        }
        extended.scissorLeftOriginStack[scissorStackSize - 1] = extAlignment.leftOrigin;
        extended.scissorRightOriginStack[scissorStackSize - 1] = extAlignment.rightOrigin;
        state->updateDrawStatusAttribute(DrawAttribute::Scissor);
    }

    void RDP::pushScissor() {
        if (scissorStackSize < RDP_EXTENDED_STACK_SIZE) {
            scissorRectStack[scissorStackSize] = scissorRectStack[scissorStackSize - 1];
            scissorModeStack[scissorStackSize] = scissorModeStack[scissorStackSize - 1];
            extended.scissorLeftOriginStack[scissorStackSize] = extended.scissorLeftOriginStack[scissorStackSize - 1];
            extended.scissorRightOriginStack[scissorStackSize] = extended.scissorRightOriginStack[scissorStackSize - 1];
            scissorStackSize++;
        }
    }

    void RDP::popScissor() {
        if (scissorStackSize > 1) {
            scissorStackSize--;
            state->updateDrawStatusAttribute(DrawAttribute::Scissor);
        }
    }

    void RDP::setConvert(int32_t k0, int32_t k1, int32_t k2, int32_t k3, int32_t k4, int32_t k5) {
        convertK[0] = k0;
        convertK[1] = k1;
        convertK[2] = k2;
        convertK[3] = k3;
        convertK[4] = k4;
        convertK[5] = k5;
        state->updateDrawStatusAttribute(DrawAttribute::Convert);
    }

    void RDP::setKeyR(uint32_t cR, uint32_t sR, uint32_t) {
        keyCenter.x = cR / 255.0f;
        keyScale.x = sR / 255.0f;
        state->updateDrawStatusAttribute(DrawAttribute::Key);
    }

    void RDP::setKeyGB(uint32_t cG, uint32_t sG, uint32_t, uint32_t cB, uint32_t sB, uint32_t) {
        keyCenter.y = cG / 255.0f;
        keyCenter.z = cB / 255.0f;
        keyScale.y = sG / 255.0f;
        keyScale.z = sB / 255.0f;
        state->updateDrawStatusAttribute(DrawAttribute::Key);
    }

    void RDP::fillRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
        fillRect(ulx, uly, lrx, lry, extended.global.rect);
    }

    void RDP::fillRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, const ExtendedAlignment &extAlignment) {
        if ((lrx < ulx) || (lry < uly)) {
            return;
        }

        const uint32_t mode = (otherMode.H & (3U << G_MDSFT_CYCLETYPE));
        if ((mode == G_CYC_COPY) || (mode == G_CYC_FILL)) {
            lrx |= 3;
            lry |= 3;
        }

        drawRect(ulx, uly, lrx, lry, 0, 0, 0, 0, false, extAlignment);
    }

    void RDP::setRectAlign(const ExtendedAlignment &extAlignment) {
        extended.global.rect = extAlignment;
    }

    void RDP::setScissorAlign(const ExtendedAlignment &extAlignment) {
        extended.global.scissor = extAlignment;
    }

    void RDP::forceUpscale2D(bool force) {
        extended.drawExtendedFlags.forceUpscale2D = force;
    }

    void RDP::forceTrueBilerp(uint8_t mode) {
        extended.drawExtendedFlags.forceTrueBilerp = mode;
    }

    void RDP::forceScaleLOD(bool force) {
        extended.drawExtendedFlags.forceScaleLOD = force;
    }

    void RDP::clearExtended() {
        extended.scissorLeftOriginStack[0] = G_EX_ORIGIN_NONE;
        extended.scissorRightOriginStack[0] = G_EX_ORIGIN_NONE;
        extended.drawExtendedFlags = {};
        extended.global.rect = ExtendedAlignment();
        extended.global.scissor = ExtendedAlignment();
    }

    void RDP::drawTris(uint32_t triCount, const float *pos, const float *tc, const float *col, uint8_t tile, uint8_t levels) {
        vg::Renderer &renderer = *state->renderer;
        for (uint32_t i = 0; i < triCount; i++) {
            vg::Vertex v[3];
            for (int k = 0; k < 3; k++) {
                const uint32_t n = i * 3 + k;
                v[k].x = pos[n * 4 + 0];
                v[k].y = pos[n * 4 + 1];
                v[k].z = pos[n * 4 + 2];
                v[k].w = pos[n * 4 + 3];
                v[k].s = tc[n * 2 + 0];
                v[k].t = tc[n * 2 + 1];
                v[k].r = col[n * 4 + 0];
                v[k].g = col[n * 4 + 1];
                v[k].b = col[n * 4 + 2];
                v[k].a = col[n * 4 + 3];
            }
            renderer.addRawTriangle(v, tile, levels + 1);
        }
    }

    void RDP::drawRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
        drawRect(ulx, uly, lrx, lry, uls, ult, dsdx, dtdy, flip, extended.global.rect);
    }

    // Texcoord math follows RT64's drawRect; texcoords are in 10.5 texel units divided by 32.
    void RDP::drawRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip, const ExtendedAlignment &extAlignment) {
        const bool usesFillMode = (otherMode.cycleType() == G_CYC_FILL);
        const bool usesCopyMode = (otherMode.cycleType() == G_CYC_COPY);
        if (usesFillMode || usesCopyMode) {
            ulx &= ~3;
            uly &= ~3;
        }

        ulx += extAlignment.leftOffset;
        uly += extAlignment.topOffset;
        lrx += extAlignment.rightOffset;
        lry += extAlignment.bottomOffset;

        const FixedRect drawRect(movedFromOrigin(ulx, extAlignment.leftOrigin), uly, movedFromOrigin(lrx, extAlignment.rightOrigin), lry);
        if (drawRect.isEmpty()) {
            return;
        }

        const int32_t rectWidth = drawRect.width(true, true);
        const int32_t rectHeight = drawRect.height(true, true);
        const int32_t uvWidth = (flip ? rectHeight : rectWidth) << 2;
        const int32_t uvHeight = (flip ? rectWidth : rectHeight) << 2;
        const int32_t lrs = ((uls << 7) + dsdx * uvWidth) >> 7;
        const int32_t lrt = ((ult << 7) + dtdy * uvHeight) >> 7;
        const float vFractionOffset = (uly & 0x3) ? (dtdy >> 5) / 32.0f : 0.0f;
        vg::RectDraw rect;
        rect.x0 = float(drawRect.left(true));
        rect.y0 = float(drawRect.top(true));
        rect.x1 = float(drawRect.right(true));
        rect.y1 = float(drawRect.bottom(true));
        rect.u0 = uls / 32.0f;
        rect.v0 = ult / 32.0f + vFractionOffset;
        rect.u1 = lrs / 32.0f;
        rect.v1 = lrt / 32.0f + vFractionOffset;
        rect.flip = flip;
        rect.tile = rectTile;
        rect.textured = rectTextured;
        state->renderer->addRect(rect);
        rectTextured = false;
        rectTile = 0;
    }

    void RDP::drawTexRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
        drawTexRect(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, flip, extended.global.rect);
    }

    void RDP::drawTexRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip, const ExtendedAlignment &extAlignment) {
        if (otherMode.cycleType() == G_CYC_COPY) {
            dsdx >>= 2;
            lrx |= 3;
            lry |= 3;
        }

        rectTextured = true;
        rectTile = tile;
        drawRect(ulx, uly, lrx, lry, uls, ult, dsdx, dtdy, flip, extAlignment);
    }

    void RDP::updateCallTexcoords(float, float) { }
};
