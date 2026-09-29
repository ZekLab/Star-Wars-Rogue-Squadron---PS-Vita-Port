// Rebinding editor of the game's Controller Settings screen on the Vita, active for as long as the screen is shown. UP/DOWN pick an
// action, CROSS then waits for one control or two held together (a chord), TRIANGLE restores the Vita defaults; LEFT/RIGHT switch
// the game's preset and CIRCLE leaves (N64 B). Every change is saved to roguesq_input.json at once. The panel is drawn by vg::controlsPanel.

#pragma once

#include <cstdint>
#include <string>

#include "input_bindings.h"
#include "vg_controls_panel.h"

struct _SDL_GameController;

namespace rs64::vita {
    // Called with every controller poll. Reads the pad directly and returns true while the editor holds the game's input; the
    // caller then reports only gameButtons (N64 bits the editor passes on) with a centered stick.
    bool controls_ui_poll(const uint8_t* rdram, _SDL_GameController* pad, uint16_t* gameButtons);

    vg::ControlsPanelState controls_ui_state();

    // Vita controls bound to one N64 button bit, chords included ("L + R"), joined with " / "; empty when none can be pressed.
    std::string vita_button_names(const rs64::input::Bindings& b, uint16_t bit);
}
