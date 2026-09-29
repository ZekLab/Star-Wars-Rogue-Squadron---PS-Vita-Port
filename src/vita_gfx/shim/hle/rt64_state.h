//
// RT64 (PS Vita shim)
//
// Replaces lib/rt64/src/hle/rt64_state.h for the Vita renderer: only the members RT64's GBI parsers use.
// The implementation lives in src/vita_gfx (vg_state.cpp) and forwards drawing to the vitaGL backend.

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <vector>
#include <stdint.h>

#include "common/rt64_common.h"
#include "hle/rt64_microcode.h"
#include "hle/rt64_draw_call.h"
#include "hle/rt64_rdp.h"
#include "hle/rt64_rsp.h"

namespace vg {
    struct Renderer;
}

namespace RT64 {
    struct Interpreter;

    const uint32_t RDRAMSize = 0x7FFFFF;

    // Per-task transformed vertices, indexed by the global vertex index the RSP stores in RSP::indices.
    // The Factor 5 parser reads posScreen/posTransformed right after a vertex load (sprite depth, overlay widening).
    struct DrawData {
        std::vector<hlslpp::float4> posTransformed;
        std::vector<hlslpp::float3> posScreen;

        uint32_t vertexCount() const {
            return uint32_t(posScreen.size());
        }
    };

    struct Workload {
        DrawData drawData;
    };

    struct WorkloadQueue {
        std::array<Workload, 1> workloads;
        int writeCursor = 0;
    };

    struct State {
        struct External {
            Interpreter *interpreter = nullptr;
            WorkloadQueue *workloadQueue = nullptr;
        };

        uint8_t *RDRAM;
        Microcode microcode;
        std::unique_ptr<RSP> rsp;
        std::unique_ptr<RDP> rdp;
        std::vector<DisplayList *> returnAddressStack;
        uint32_t displayListAddress = 0;
        uint64_t displayListCounter = 0;
        External ext;
        WorkloadQueue workloadQueue;
        vg::Renderer *renderer = nullptr;

        State(uint8_t *RDRAM, vg::Renderer *renderer);
        ~State();
        void reset();
        void flush();
        void fullSync();
        void dpInterrupt();
        void updateDrawStatusAttribute(DrawAttribute attribute);
        void pushReturnAddress(DisplayList *dl);
        DisplayList *popReturnAddress();
        uint8_t *fromRDRAM(uint32_t rdramAddress) const {
            return &RDRAM[rdramAddress];
        }
    };
};
