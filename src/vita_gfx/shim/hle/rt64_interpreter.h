//
// RT64 (PS Vita shim)
//
// Replaces lib/rt64/src/hle/rt64_interpreter.h: the display-list walk over the Factor 5 GBI table, without RT64's
// ucode database (the game only runs the Factor 5 ucode).

#pragma once

#include "hle/rt64_state.h"

#include "gbi/rt64_f3d.h"
#include "gbi/rt64_gbi.h"

namespace RT64 {
    struct Interpreter {
        State *state = nullptr;
        GBI factor5Gbi;
        GBI *hleGBI = nullptr;

        void setup(State *state);
        void loadUCodeGBI(uint32_t textAddress, uint32_t dataAddress, bool resetFromTask);
        void processDisplayLists(uint32_t dlStartAddress, DisplayList *dlStart);
    };
};
