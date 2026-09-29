// State and display-list interpreter for the Vita renderer (shim of RT64's hle/rt64_state.cpp and rt64_interpreter.cpp).

#include <cstdio>

#include "gbi/rt64_gbi.h"
#include "gbi/rt64_gbi_extended.h"
#include "gbi/rt64_gbi_f3dfactor5.h"
#include "gbi/rt64_gbi_rdp.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"

#include "vg_prof.h"
#include "vg_renderer.h"

namespace RT64 {
    // DisplayList

    DisplayList::DisplayList() {
        w0 = w1 = 0;
    }

    uint32_t DisplayList::p0(uint8_t pos, uint8_t bits) const {
        return ((w0 >> pos) & ((0x01 << bits) - 1));
    }

    uint32_t DisplayList::p1(uint8_t pos, uint8_t bits) const {
        return ((w1 >> pos) & ((0x01 << bits) - 1));
    }

    // RT64 extended-GBI hooks are only used by games built against RT64; Rogue Squadron never emits them.
    namespace GBI_EXTENDED {
        void noOpHook(State *, DisplayList **) { }
    };

    // State

    State::State(uint8_t *RDRAM, vg::Renderer *renderer) : RDRAM(RDRAM), renderer(renderer) {
        rsp = std::make_unique<RSP>(this);
        rdp = std::make_unique<RDP>(this);
        ext.workloadQueue = &workloadQueue;
        reset();
    }

    State::~State() { }

    void State::reset() {
        rsp->reset();
        rdp->reset();
        returnAddressStack.clear();
    }

    void State::flush() {
        renderer->flush();
    }

    void State::fullSync() {
        renderer->flush();
    }

    // SP/DP completion is signalled by ultramodern after send_dl returns.
    void State::dpInterrupt() { }

    void State::updateDrawStatusAttribute(DrawAttribute) {
        renderer->markDirty();
    }

    void State::pushReturnAddress(DisplayList *dl) {
        returnAddressStack.push_back(dl);
    }

    DisplayList *State::popReturnAddress() {
        if (returnAddressStack.empty()) {
            return nullptr;
        }
        DisplayList *dl = returnAddressStack.back();
        returnAddressStack.pop_back();
        return dl;
    }

    // Interpreter

    void Interpreter::setup(State *state) {
        this->state = state;
        state->ext.interpreter = this;
    }

    // Graphics tasks only ever run the Factor 5 ucode, so its GBI is built once without RT64's ucode hash database.
    void Interpreter::loadUCodeGBI(uint32_t, uint32_t, bool resetFromTask) {
        if (hleGBI == nullptr) {
            factor5Gbi.ucode = GBIUCode::F3DFACTOR5;
            GBI_RDP::setup(&factor5Gbi, true);
            GBI_F3DFACTOR5::setup(&factor5Gbi);
            factor5Gbi.flags = {};
            hleGBI = &factor5Gbi;
            state->rsp->setGBI(hleGBI);
            state->rdp->setGBI();
        }

        GBIReset resetFunction = resetFromTask ? hleGBI->resetFromTask : hleGBI->resetFromLoad;
        if (resetFunction != nullptr) {
            resetFunction(state);
        }
    }

    void Interpreter::processDisplayLists(uint32_t dlStartAddress, DisplayList *dlStart) {
        state->displayListAddress = dlStartAddress;
        state->displayListCounter++;
        state->returnAddressStack.clear();
        DisplayList *dl = dlStart;
        while (dl != nullptr) {
            const uint8_t opCode = uint8_t(dl->w0 >> 24);
            const GBIFunction func = hleGBI->map[opCode];
            if (func != nullptr) {
                if (vg::g_opProfEnabled) {
                    const uint64_t t0 = vg::profNow();
                    func(state, &dl);
                    vg::g_opTicks[opCode] += vg::profNow() - t0;
                    vg::g_opCalls[opCode]++;
                }
                else {
                    func(state, &dl);
                }
            }
            if (dl != nullptr) {
                dl++;
            }
        }
    }
};

// Desync ring tracing lives in RT64's desktop interpreter; the Vita build keeps the hook as a no-op.
extern "C" void rt64_f5_desync_dump(const char *, uint32_t) { }
