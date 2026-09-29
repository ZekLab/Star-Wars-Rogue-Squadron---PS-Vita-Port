//
// RT64 (PS Vita shim)
//
// Only the draw-call types the RSP/RDP state headers use (lib/rt64/src/hle/rt64_draw_call.h pulls in the framebuffer system).

#pragma once

#include <stdint.h>

namespace RT64 {
    enum class DrawAttribute : uint32_t {
        Zero = 0,
        UID = 1,
        Tris = 2,
        Scissor = 5,
        Combine = 7,
        Texture = 8,
        OtherMode = 9,
        GeometryMode = 11,
        PrimColor = 12,
        EnvColor = 13,
        FogColor = 14,
        FillColor = 15,
        BlendColor = 16,
        Lights = 18,
        FramebufferPair = 21,
        PrimDepth = 22,
        Convert = 23,
        Key = 24,
        ObjRenderMode = 25,
        ExtendedType = 26,
        ExtendedFlags = 27,
        Count = 28
    };

    enum class DrawExtendedType : uint8_t {
        None,
        VertexTestZ,
        EndVertexTestZ
    };

    struct DrawVertexTestZ {
        uint32_t vertexIndex;
    };

    struct DrawExtendedData {
        union {
            DrawVertexTestZ vertexTestZ;
        };
    };

    struct DrawExtendedFlags {
        uint32_t forceUpscale2D : 1;
        uint32_t forceTrueBilerp : 2;
        uint32_t forceScaleLOD : 1;
    };
};
