// CPU decode of RDP tiles from TMEM into RGBA8, following RT64's TextureDecoder.hlsli addressing.

#pragma once

#include <cstdint>

namespace vg {
    struct TileState;

    // Sampled size as RT64 computes it (min of the clamp extent and the mask), capped for sanity.
    void tileSampleSize(const TileState &tile, bool textureOn, uint16_t &width, uint16_t &height);

    // Bytes and update calls fed to the texture hash, for the parser bench.
    extern uint64_t g_hashBytes;
    extern uint32_t g_hashUpdates;

    // Hash of every TMEM byte (texels and palette) the decode reads, plus the parameters that shape it.
    uint64_t hashTile(const uint8_t *TMEM, const TileState &tile, uint32_t tlut, uint16_t width, uint16_t height);

    // Writes width * height RGBA8 texels (row-major, top row first).
    void decodeTile(const uint8_t *TMEM, const TileState &tile, uint32_t tlut, uint16_t width, uint16_t height, uint8_t *out);
}
