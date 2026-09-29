// Overlay for the game's Controller Settings screen on the Vita: every action of the controller preset being shown, with the
// Vita controls that press its N64 button(s), plus the state of the rebinding editor (vita_controls_ui.cpp). The game still draws
// its title and BACK above and below the panel.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vg_backend_gl.h"

namespace vg {
    // Rebinding editor state shown on the panel: choosing an action, or waiting for the controls to assign to it.
    struct ControlsPanelState {
        enum class Mode : uint8_t { Choose, Listen };
        Mode mode = Mode::Choose;
        int selected = 0;
        std::string message;
    };

    int controlsActionCount();
    const char *controlsActionLabel(int action);
    // N64 buttons (OSContPad bits) of an action in the preset being shown; 0 when RDRAM holds no valid preset.
    uint16_t controlsActionMask(const uint8_t *rdram, int action);
    // The N64 button a rebinding of this action changes: its first button no other action of the preset uses (brakes share R
    // with roll in some presets), else its first button.
    uint16_t controlsActionBit(const uint8_t *rdram, int action);

    // buttonNames(bit) lists the Vita controls bound to one N64 button bit, empty when none can be pressed.
    std::vector<OverlayItem> controlsPanel(const uint8_t *rdram, const std::function<std::string(uint16_t)> &buttonNames,
        const ControlsPanelState &state);
}
