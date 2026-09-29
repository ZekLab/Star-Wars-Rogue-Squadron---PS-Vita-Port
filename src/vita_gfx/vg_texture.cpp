#include "vg_texture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include "shared/rt64_f3d_defines.h"

#include "vg_renderer.h"

namespace vg {
    uint64_t g_hashBytes = 0;
    uint32_t g_hashUpdates = 0;

    constexpr uint32_t TMEMBytes = 0x1000;
    constexpr uint32_t TMEMPalette = 0x800;
    constexpr uint32_t TMEMMask8 = 0xFFF;
    constexpr uint32_t TMEMMask16 = 0x7FF;
    constexpr uint16_t MaxTextureSize = 1024;

    void tileSampleSize(const TileState &t, bool textureOn, uint16_t &width, uint16_t &height) {
        if (!textureOn) {
            width = 1;
            height = 1;
            return;
        }

        const bool clampS = (t.masks == 0) || (t.cms & G_TX_CLAMP);
        const bool clampT = (t.maskt == 0) || (t.cmt & G_TX_CLAMP);
        const int32_t tileWidth = clampS ? std::max((int32_t(t.lrs) - int32_t(t.uls) + 4) / 4, 1) : 0xFFFF;
        const int32_t tileHeight = clampT ? std::max((int32_t(t.lrt) - int32_t(t.ult) + 4) / 4, 1) : 0xFFFF;
        const int32_t maskWidth = (t.masks > 0) ? (1 << std::min<int>(t.masks, 10)) : 0xFFFF;
        const int32_t maskHeight = (t.maskt > 0) ? (1 << std::min<int>(t.maskt, 10)) : 0xFFFF;
        width = uint16_t(std::clamp<int32_t>(std::min(tileWidth, maskWidth), 1, MaxTextureSize));
        height = uint16_t(std::clamp<int32_t>(std::min(tileHeight, maskHeight), 1, MaxTextureSize));
    }

    // ROGUESQ_VG_DECODE_VERIFY=1: every decode is repeated with RT64's per-texel row division and differences are logged.
    static bool s_referenceDecode = false;

    // rowBase = y * rowSize: a texel inside its own row (the usual case) finds its row start without a division, which the
    // Cortex-A9 does in software.
    static inline uint32_t loadTMEM(const uint8_t *TMEM, uint32_t relativeAddress, uint32_t maskAddress, uint32_t orAddress, bool oddRow, uint32_t textureStart, uint32_t rowSize, uint32_t rowBase) {
        uint32_t finalAddress;
        if (oddRow && (rowSize > 0)) {
            const uint32_t rowStart = (!s_referenceDecode && ((relativeAddress - rowBase) < rowSize)) ? rowBase : (relativeAddress / rowSize) * rowSize;
            const uint32_t wordIndex = (relativeAddress - rowStart) / 4;
            finalAddress = textureStart + rowStart + ((wordIndex ^ 1) * 4) + (relativeAddress & 0x3);
        }
        else {
            finalAddress = textureStart + relativeAddress;
        }
        return TMEM[((finalAddress & maskAddress) | orAddress) & TMEMMask8];
    }

    static inline void put(uint8_t *o, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
        o[0] = uint8_t(r);
        o[1] = uint8_t(g);
        o[2] = uint8_t(b);
        o[3] = uint8_t(a);
    }

    static inline void putRGBA16(uint8_t *o, uint32_t c) {
        const uint32_t r = (c >> 11) & 0x1F, g = (c >> 6) & 0x1F, b = (c >> 1) & 0x1F;
        put(o, (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (c & 1) ? 255 : 0);
    }

    static inline void putIA16(uint8_t *o, uint32_t c) {
        const uint32_t i = (c >> 8) & 0xFF;
        put(o, i, i, i, c & 0xFF);
    }

    static void decodeTexel(const uint8_t *TMEM, const TileState &t, uint32_t tlut, int x, int y, uint8_t *o) {
        const uint32_t address = uint32_t(t.tmem) << 3;
        const uint32_t stride = uint32_t(t.line) << 3;
        const bool oddRow = (y & 1) != 0;
        const bool oddColumn = (x & 1) != 0;
        const bool isRgba32 = (t.fmt == G_IM_FMT_RGBA) && (t.siz == G_IM_SIZ_32b);
        const bool usesTlut = tlut > 0;
        const uint32_t tmemShift = isRgba32 ? 2 : t.siz;
        const uint32_t addressMask = (isRgba32 || usesTlut) ? TMEMMask16 : TMEMMask8;
        const uint32_t rowBase = uint32_t(y) * stride;
        const uint32_t pixelAddress = rowBase + ((uint32_t(x) << tmemShift) >> 1);
        const uint32_t p0 = loadTMEM(TMEM, pixelAddress + 0, addressMask, 0, oddRow, address, stride, rowBase);
        const uint32_t p1 = loadTMEM(TMEM, pixelAddress + 1, addressMask, 0, oddRow, address, stride, rowBase);
        const uint32_t p4 = (p0 >> (oddColumn ? 0 : 4)) & 0xF;
        if (usesTlut) {
            const uint32_t paletteAddress = (t.siz == G_IM_SIZ_4b) ? (TMEMPalette + (uint32_t(t.palette) << 7) + (p4 << 3)) : (TMEMPalette + (p0 << 3));
            const uint32_t value = TMEM[(paletteAddress + 1) & TMEMMask8] | (uint32_t(TMEM[paletteAddress & TMEMMask8]) << 8);
            if (tlut == G_TT_RGBA16) {
                putRGBA16(o, value);
            }
            else if (tlut == G_TT_IA16) {
                putIA16(o, value);
            }
            else {
                put(o, 0, 0, 0, 255);
            }
            return;
        }

        const uint32_t pixelAddress2 = isRgba32 ? pixelAddress : pixelAddress + 2;
        const uint32_t orAddress = isRgba32 ? (TMEMBytes >> 1) : 0;
        switch (t.siz) {
        case G_IM_SIZ_4b:
            if (t.fmt == G_IM_FMT_CI) {
                const uint32_t v = (uint32_t(t.palette) << 4) | p4;
                put(o, v, v, v, v);
            }
            else if (t.fmt == G_IM_FMT_IA) {
                uint32_t i = p4 & 0xE;
                i = (i << 4) | (i << 1) | (i >> 2);
                put(o, i, i, i, (p4 & 1) ? 255 : 0);
            }
            else if ((t.fmt == G_IM_FMT_I) || (t.fmt == G_IM_FMT_RGBA)) {
                const uint32_t i = (p4 << 4) | p4;
                put(o, i, i, i, i);
            }
            else {
                put(o, 0, 0, 0, 255);
            }
            break;
        case G_IM_SIZ_8b:
            if (t.fmt == G_IM_FMT_IA) {
                const uint32_t i = (p0 >> 4) & 0xF, a = p0 & 0xF;
                put(o, (i << 4) | i, (i << 4) | i, (i << 4) | i, (a << 4) | a);
            }
            else if (t.fmt != G_IM_FMT_YUV) {
                put(o, p0, p0, p0, p0);
            }
            else {
                put(o, 0, 0, 0, 255);
            }
            break;
        case G_IM_SIZ_16b: {
            const uint32_t v = p1 | (p0 << 8);
            if (t.fmt == G_IM_FMT_RGBA) {
                putRGBA16(o, v);
            }
            else if (t.fmt == G_IM_FMT_IA) {
                putIA16(o, v);
            }
            else if ((t.fmt == G_IM_FMT_CI) || (t.fmt == G_IM_FMT_I)) {
                put(o, p0, p1, p0, p1);
            }
            else {
                put(o, 0, 0, 0, 255);
            }
            break;
        }
        case G_IM_SIZ_32b: {
            const uint32_t p2 = loadTMEM(TMEM, pixelAddress2 + 0, addressMask, orAddress, oddRow, address, stride, rowBase);
            const uint32_t p3 = loadTMEM(TMEM, pixelAddress2 + 1, addressMask, orAddress, oddRow, address, stride, rowBase);
            if (t.fmt == G_IM_FMT_RGBA) {
                put(o, p0, p1, p2, p3);
            }
            else if (t.fmt != G_IM_FMT_YUV) {
                if (oddColumn) {
                    put(o, p0, p1, p0, p1);
                }
                else {
                    put(o, p2, p3, p2, p3);
                }
            }
            else {
                put(o, 0, 0, 0, 255);
            }
            break;
        }
        default:
            put(o, 0, 0, 0, 255);
            break;
        }
    }

    // Row-wise decode loop: the row start and the odd-row swizzle (XOR 4 inside the row) are worked out once per row. A texel whose
    // bytes (up to lastByte past its offset) leave an odd TMEM row goes through decodeTexel, which handles that case.
    template<typename Texel>
    static void decodeRows(const uint8_t *TMEM, const TileState &t, uint32_t tlut, uint16_t width, uint16_t height, uint8_t *out,
        uint32_t tmemShift, uint32_t mask, uint32_t lastByte, Texel texel)
    {
        const uint32_t address = uint32_t(t.tmem) << 3;
        const uint32_t stride = uint32_t(t.line) << 3;
        for (int y = 0; y < height; y++) {
            const uint32_t rowStart = address + uint32_t(y) * stride;
            const bool swizzle = ((y & 1) != 0) && (stride > 0);
            const uint32_t oddXor = swizzle ? 4 : 0;
            for (int x = 0; x < width; x++, out += 4) {
                const uint32_t offset = (uint32_t(x) << tmemShift) >> 1;
                if (swizzle && ((offset + lastByte) >= stride)) {
                    decodeTexel(TMEM, t, tlut, x, y, out);
                    continue;
                }
                auto at = [&](uint32_t k, uint32_t orAddress) -> uint32_t {
                    return TMEM[(((rowStart + ((offset + k) ^ oddXor)) & mask) | orAddress) & TMEMMask8];
                };
                texel(at, x, out);
            }
        }
    }

    // Palette entry to RGBA8, as decodeTexel converts it.
    static void paletteEntry(const uint8_t *TMEM, uint32_t tlut, uint32_t paletteAddress, uint8_t *o) {
        const uint32_t value = TMEM[(paletteAddress + 1) & TMEMMask8] | (uint32_t(TMEM[paletteAddress & TMEMMask8]) << 8);
        if (tlut == G_TT_RGBA16) {
            putRGBA16(o, value);
        }
        else {
            putIA16(o, value);
        }
    }

    // The game's formats (CI4/CI8 with a palette, I4, IA4, I8, IA8, RGBA16, IA16, RGBA32) with their conversion hoisted out of the
    // texel loop and palettes converted once. Same texels as decodeTexel (ROGUESQ_VG_DECODE_VERIFY); false for anything else.
    static bool decodeFast(const uint8_t *TMEM, const TileState &t, uint32_t tlut, uint16_t width, uint16_t height, uint8_t *out) {
        const bool usesTlut = tlut > 0;
        if (usesTlut) {
            if ((tlut != G_TT_RGBA16) && (tlut != G_TT_IA16)) {
                return false;
            }
            const bool isRgba32 = (t.fmt == G_IM_FMT_RGBA) && (t.siz == G_IM_SIZ_32b);
            const uint32_t tmemShift = isRgba32 ? 2 : t.siz;
            uint8_t palette[256][4];
            if (t.siz == G_IM_SIZ_4b) {
                for (uint32_t e = 0; e < 16; e++) {
                    paletteEntry(TMEM, tlut, TMEMPalette + (uint32_t(t.palette) << 7) + (e << 3), palette[e]);
                }
                decodeRows(TMEM, t, tlut, width, height, out, tmemShift, TMEMMask16, 0, [&](auto at, int x, uint8_t *o) {
                    const uint32_t p0 = at(0, 0);
                    memcpy(o, palette[(p0 >> ((x & 1) ? 0 : 4)) & 0xF], 4);
                });
            }
            else {
                for (uint32_t e = 0; e < 256; e++) {
                    paletteEntry(TMEM, tlut, TMEMPalette + (e << 3), palette[e]);
                }
                decodeRows(TMEM, t, tlut, width, height, out, tmemShift, TMEMMask16, 0, [&](auto at, int, uint8_t *o) {
                    memcpy(o, palette[at(0, 0)], 4);
                });
            }
            return true;
        }

        switch (t.siz) {
        case G_IM_SIZ_4b:
            if ((t.fmt == G_IM_FMT_I) || (t.fmt == G_IM_FMT_RGBA)) {
                decodeRows(TMEM, t, tlut, width, height, out, 0, TMEMMask8, 0, [](auto at, int x, uint8_t *o) {
                    const uint32_t p4 = (at(0, 0) >> ((x & 1) ? 0 : 4)) & 0xF;
                    const uint32_t i = (p4 << 4) | p4;
                    put(o, i, i, i, i);
                });
                return true;
            }
            if (t.fmt == G_IM_FMT_IA) {
                decodeRows(TMEM, t, tlut, width, height, out, 0, TMEMMask8, 0, [](auto at, int x, uint8_t *o) {
                    const uint32_t p4 = (at(0, 0) >> ((x & 1) ? 0 : 4)) & 0xF;
                    uint32_t i = p4 & 0xE;
                    i = (i << 4) | (i << 1) | (i >> 2);
                    put(o, i, i, i, (p4 & 1) ? 255 : 0);
                });
                return true;
            }
            return false;
        case G_IM_SIZ_8b:
            if (t.fmt == G_IM_FMT_IA) {
                decodeRows(TMEM, t, tlut, width, height, out, 1, TMEMMask8, 0, [](auto at, int, uint8_t *o) {
                    const uint32_t p0 = at(0, 0);
                    const uint32_t i = (p0 >> 4) & 0xF, a = p0 & 0xF;
                    put(o, (i << 4) | i, (i << 4) | i, (i << 4) | i, (a << 4) | a);
                });
                return true;
            }
            if ((t.fmt == G_IM_FMT_I) || (t.fmt == G_IM_FMT_RGBA) || (t.fmt == G_IM_FMT_CI)) {
                decodeRows(TMEM, t, tlut, width, height, out, 1, TMEMMask8, 0, [](auto at, int, uint8_t *o) {
                    const uint32_t p0 = at(0, 0);
                    put(o, p0, p0, p0, p0);
                });
                return true;
            }
            return false;
        case G_IM_SIZ_16b:
            if (t.fmt == G_IM_FMT_RGBA) {
                decodeRows(TMEM, t, tlut, width, height, out, 2, TMEMMask8, 1, [](auto at, int, uint8_t *o) {
                    putRGBA16(o, at(1, 0) | (at(0, 0) << 8));
                });
                return true;
            }
            if (t.fmt == G_IM_FMT_IA) {
                decodeRows(TMEM, t, tlut, width, height, out, 2, TMEMMask8, 1, [](auto at, int, uint8_t *o) {
                    putIA16(o, at(1, 0) | (at(0, 0) << 8));
                });
                return true;
            }
            return false;
        case G_IM_SIZ_32b:
            if (t.fmt == G_IM_FMT_RGBA) {
                decodeRows(TMEM, t, tlut, width, height, out, 2, TMEMMask16, 1, [](auto at, int, uint8_t *o) {
                    put(o, at(0, 0), at(1, 0), at(0, TMEMBytes >> 1), at(1, TMEMBytes >> 1));
                });
                return true;
            }
            return false;
        default:
            return false;
        }
    }

    static void decodeTexels(const uint8_t *TMEM, const TileState &tile, uint32_t tlut, uint16_t width, uint16_t height, uint8_t *out) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                decodeTexel(TMEM, tile, tlut, x, y, out);
                out += 4;
            }
        }
    }

    void decodeTile(const uint8_t *TMEM, const TileState &tile, uint32_t tlut, uint16_t width, uint16_t height, uint8_t *out) {
        if (!decodeFast(TMEM, tile, tlut, width, height, out)) {
            decodeTexels(TMEM, tile, tlut, width, height, out);
        }
        static const bool s_verify = []() {
            const char *v = std::getenv("ROGUESQ_VG_DECODE_VERIFY");
            return v && (v[0] == '1');
        }();
        if (s_verify) {
            static uint32_t decodes = 0, mismatches = 0;
            std::vector<uint8_t> reference(size_t(width) * height * 4);
            s_referenceDecode = true;
            decodeTexels(TMEM, tile, tlut, width, height, reference.data());
            s_referenceDecode = false;
            decodes++;
            if ((memcmp(reference.data(), out, reference.size()) != 0) && (++mismatches <= 10)) {
                fprintf(stderr, "[decode-verify] decode #%u differs (fmt %u siz %u %ux%u)\n", decodes, tile.fmt, tile.siz, width, height);
            }
            if ((decodes % 100) == 0) {
                fprintf(stderr, "[decode-verify] %u decodes checked, %u differ\n", decodes, mismatches);
            }
        }
    }

    // Hashes only the TMEM bytes the decode reads: each row's texels (8-byte aligned, wrapping like the decoder) plus the palette.
    // Hashing whole TMEM ranges picked up stale bytes from other loads and made static textures look new every frame.
    uint64_t hashTile(const uint8_t *TMEM, const TileState &t, uint32_t tlut, uint16_t width, uint16_t height) {
        XXH3_state_t st;
        XXH3_64bits_reset(&st);
        auto update = [&st](const void *data, size_t size) {
            XXH3_64bits_update(&st, data, size);
            g_hashBytes += size;
            g_hashUpdates++;
        };
        const uint32_t params[8] = { t.fmt, t.siz, t.palette, t.line, t.tmem, tlut, width, height };
        update(params, sizeof(params));

        const bool isRgba32 = (t.fmt == G_IM_FMT_RGBA) && (t.siz == G_IM_SIZ_32b);
        const uint32_t mask = (isRgba32 || (tlut > 0)) ? TMEMMask16 : TMEMMask8;
        const uint32_t start = uint32_t(t.tmem) << 3;
        const uint32_t stride = uint32_t(t.line) << 3;
        const uint32_t rowBytes = std::min<uint32_t>((((uint32_t(width) << (isRgba32 ? 2 : t.siz)) >> 1) + 7) & ~7U, mask + 1);
        const uint32_t rows = (stride == 0) ? 1 : std::min<uint32_t>(height, (mask + 1) / std::min(stride, mask + 1) + 1);
        auto hashRange = [&](uint32_t base, uint32_t orAddress) {
            const uint32_t a = base & mask;
            const uint32_t first = std::min(rowBytes, mask + 1 - a);
            update(TMEM + (a | orAddress), first);
            if (rowBytes > first) {
                update(TMEM + orAddress, rowBytes - first);
            }
        };
        const uint32_t startMasked = start & mask;
        if ((stride == rowBytes) && (startMasked + rows * stride <= mask + 1)) {
            // Rows packed back to back without wrapping: one span, same bytes as the per-row loop.
            update(TMEM + startMasked, rows * stride);
            if (isRgba32) {
                update(TMEM + (startMasked | (TMEMBytes >> 1)), rows * stride);
            }
        }
        else for (uint32_t y = 0; y < rows; y++) {
            hashRange(start + y * stride, 0);
            if (isRgba32) {
                hashRange(start + y * stride, TMEMBytes >> 1);
            }
        }
        if (tlut > 0) {
            if (t.siz == G_IM_SIZ_4b) {
                update(TMEM + TMEMPalette + (uint32_t(t.palette) << 7), 128);
            }
            else {
                update(TMEM + TMEMPalette, TMEMBytes >> 1);
            }
        }
        return XXH3_64bits_digest(&st);
    }
}
