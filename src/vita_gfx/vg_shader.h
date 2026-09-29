// Generates the fragment shader for one RDP configuration: RT64's color combiner and blender
// (lib/rt64/src/shared/rt64_color_combiner.h, rt64_blender.h) resolved at generation time.

#pragma once

#include <cstdint>
#include <string>

namespace vg {
    struct DrawState;

    struct ShaderKey {
        uint32_t combineL, combineH;
        uint32_t otherModeL, otherModeH;
        uint32_t flags;

        bool operator==(const ShaderKey &o) const {
            return (combineL == o.combineL) && (combineH == o.combineH) && (otherModeL == o.otherModeL) &&
                   (otherModeH == o.otherModeH) && (flags == o.flags);
        }
    };

    struct ShaderKeyHash {
        size_t operator()(const ShaderKey &k) const {
            uint64_t h = 1469598103934665603ULL;
            const uint32_t words[5] = { k.combineL, k.combineH, k.otherModeL, k.otherModeH, k.flags };
            for (uint32_t w : words) {
                h = (h ^ w) * 1099511628211ULL;
            }
            return size_t(h ^ (h >> 32));
        }
    };

    enum ShaderKeyFlags : uint32_t {
        SHADER_FLAG_RECT = 1 << 0
    };

    ShaderKey makeShaderKey(const DrawState &s);

    struct ShaderSource {
        std::string vertex;
        std::string fragment;
        bool usesTexture[2];
        bool alphaBlend;
    };

    // cg selects the Vita (Cg) dialect; otherwise GLSL 1.20 for the desktop harness.
    ShaderSource generateShader(const ShaderKey &key, bool cg);

    // Whether the combiner reads TEXEL0 (index 0) or TEXEL1 (index 1) with this key.
    bool shaderUsesTexture(const ShaderKey &key, uint32_t index);
}
