#include "vita_controls_ui.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include <SDL.h>

#include "game_state.h"
#include "main.h"

namespace rs64::vita {
    namespace {
        using rs64::input::Bindings;
        using rs64::input::Source;
        using rs64::input::SourceKind;
        using rs64::input::Target;

        // Every Vita control the editor can assign. The left stick is left out: it steers.
        struct Control {
            Source source;
            const char* name;
        };
        const Control Controls[] = {
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_A, 0 }, "CROSS" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_B, 0 }, "CIRCLE" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_X, 0 }, "SQUARE" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_Y, 0 }, "TRIANGLE" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_BACK, 0 }, "SELECT" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_START, 0 }, "START" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0 }, "L" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0 }, "R" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_DPAD_UP, 0 }, "UP" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0 }, "DOWN" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0 }, "LEFT" },
            { { SourceKind::PadButton, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0 }, "RIGHT" },
            { { SourceKind::PadAxis, SDL_CONTROLLER_AXIS_RIGHTY, -1 }, "RIGHT STICK UP" },
            { { SourceKind::PadAxis, SDL_CONTROLLER_AXIS_RIGHTY, +1 }, "RIGHT STICK DOWN" },
            { { SourceKind::PadAxis, SDL_CONTROLLER_AXIS_RIGHTX, -1 }, "RIGHT STICK LEFT" },
            { { SourceKind::PadAxis, SDL_CONTROLLER_AXIS_RIGHTX, +1 }, "RIGHT STICK RIGHT" },
        };
        constexpr int ControlCount = int(sizeof(Controls) / sizeof(Controls[0]));
        constexpr uint32_t Cross = 1u << 0, Circle = 1u << 1, Triangle = 1u << 3;
        // Menu directions from the D-pad or the left stick.
        constexpr uint32_t NavUp = 1u << 0, NavDown = 1u << 1, NavLeft = 1u << 2, NavRight = 1u << 3;
        // N64 buttons the game still gets on the screen: D-pad left/right switch the preset, B leaves.
        constexpr uint16_t N64DpadLeft = 0x0200, N64DpadRight = 0x0100, N64B = 0x4000;
        // A deflection past this fraction of the axis counts as a press.
        constexpr int StickPress = 32767 * 6 / 10;
        // Listening for a control gives up after this long with nothing pressed.
        constexpr uint32_t ListenTimeoutMs = 8000;

        std::mutex g_mutex;
        vg::ControlsPanelState g_state;
        uint32_t g_previous = 0;
        uint32_t g_previousNav = 0;
        bool g_armed = false;
        uint32_t g_captured = 0;
        uint32_t g_listenStart = 0;

        uint32_t read_controls(SDL_GameController* pad) {
            uint32_t pressed = 0;
            for (int i = 0; i < ControlCount; i++) {
                const Source& s = Controls[i].source;
                bool on = false;
                if (s.kind == SourceKind::PadButton) {
                    on = SDL_GameControllerGetButton(pad, SDL_GameControllerButton(s.code)) != 0;
                }
                else {
                    const int v = SDL_GameControllerGetAxis(pad, SDL_GameControllerAxis(s.code));
                    on = (s.dir < 0) ? (v < -StickPress) : (v > StickPress);
                }
                if (on) {
                    pressed |= 1u << i;
                }
            }
            return pressed;
        }

        uint32_t read_navigation(SDL_GameController* pad) {
            const int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
            const int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            uint32_t nav = 0;
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || (y < -StickPress)) {
                nav |= NavUp;
            }
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || (y > StickPress)) {
                nav |= NavDown;
            }
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || (x < -StickPress)) {
                nav |= NavLeft;
            }
            if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || (x > StickPress)) {
                nav |= NavRight;
            }
            return nav;
        }

        Target target_for_bit(uint16_t bit, bool& ok) {
            ok = true;
            switch (bit) {
                case 0x8000: return Target::A;
                case 0x4000: return Target::B;
                case 0x2000: return Target::Z;
                case 0x1000: return Target::Start;
                case 0x0800: return Target::DpadUp;
                case 0x0400: return Target::DpadDown;
                case 0x0200: return Target::DpadLeft;
                case 0x0100: return Target::DpadRight;
                case 0x0020: return Target::LTrig;
                case 0x0010: return Target::RTrig;
                case 0x0008: return Target::CUp;
                case 0x0004: return Target::CDown;
                case 0x0002: return Target::CLeft;
                case 0x0001: return Target::CRight;
                default: ok = false; return Target::A;
            }
        }

        std::string control_name(const Source& s) {
            for (const Control& c : Controls) {
                if (rs64::input::same_source(c.source, s)) {
                    return c.name;
                }
            }
            if ((s.kind == SourceKind::PadAxis) && ((s.code == SDL_CONTROLLER_AXIS_LEFTX) || (s.code == SDL_CONTROLLER_AXIS_LEFTY))) {
                return "LEFT STICK";
            }
            return "";
        }

        bool is_pad_source(const Source& s) {
            return (s.kind == SourceKind::PadButton) || (s.kind == SourceKind::PadAxis);
        }

        // Gives the selected action exactly the captured control(s); called with g_mutex held.
        void assign(const uint8_t* rdram, uint32_t captured) {
            std::vector<Source> sources;
            for (int i = 0; i < ControlCount; i++) {
                if (captured & (1u << i)) {
                    sources.push_back(Controls[i].source);
                }
            }
            const char* label = vg::controlsActionLabel(g_state.selected);
            if (sources.size() > 2) {
                g_state.message = "USE ONE BUTTON OR TWO TOGETHER";
                return;
            }
            bool ok = false;
            const Target target = target_for_bit(vg::controlsActionBit(rdram, g_state.selected), ok);
            if (!ok || sources.empty()) {
                g_state.message = std::string(label) + ": NOTHING CHANGED";
                return;
            }

            rs64_input_bindings_edit([&](Bindings& b) {
                auto& list = b.targets[(int)target];
                list.erase(std::remove_if(list.begin(), list.end(), is_pad_source), list.end());
                b.chords.erase(std::remove_if(b.chords.begin(), b.chords.end(), [&](const rs64::input::Chord& c) { return c.target == target; }),
                    b.chords.end());
                if (sources.size() == 1) {
                    // One control, one action: it leaves whatever it pressed before.
                    for (auto& other : b.targets) {
                        other.erase(std::remove_if(other.begin(), other.end(),
                            [&](const Source& s) { return rs64::input::same_source(s, sources[0]); }), other.end());
                    }
                    list.push_back(sources[0]);
                }
                else {
                    // A chord of the same two controls moves here.
                    b.chords.erase(std::remove_if(b.chords.begin(), b.chords.end(), [&](const rs64::input::Chord& c) {
                        if (c.sources.size() != 2) {
                            return false;
                        }
                        const bool direct = rs64::input::same_source(c.sources[0], sources[0]) && rs64::input::same_source(c.sources[1], sources[1]);
                        const bool swapped = rs64::input::same_source(c.sources[0], sources[1]) && rs64::input::same_source(c.sources[1], sources[0]);
                        return direct || swapped;
                    }), b.chords.end());
                    rs64::input::Chord chord;
                    chord.sources = sources;
                    chord.target = target;
                    b.chords.push_back(chord);
                }
            });

            std::string names = control_name(sources[0]);
            if (sources.size() == 2) {
                names += " + " + control_name(sources[1]);
            }
            g_state.message = std::string(label) + " = " + names;
            fprintf(stderr, "[controls] %s\n", g_state.message.c_str());
        }
    }

    std::string vita_button_names(const Bindings& b, uint16_t bit) {
        bool ok = false;
        const Target target = target_for_bit(bit, ok);
        if (!ok) {
            return "";
        }
        std::vector<std::string> names;
        auto add = [&](const std::string& name) {
            if (!name.empty() && (std::find(names.begin(), names.end(), name) == names.end())) {
                names.push_back(name);
            }
        };
        for (const Source& s : b.targets[(int)target]) {
            add(control_name(s));
        }
        for (const rs64::input::Chord& c : b.chords) {
            if (c.target != target) {
                continue;
            }
            std::string chord;
            for (const Source& s : c.sources) {
                const std::string name = control_name(s);
                if (name.empty()) {
                    chord.clear();
                    break;
                }
                chord += chord.empty() ? name : (" + " + name);
            }
            add(chord);
        }
        std::string joined;
        for (const std::string& n : names) {
            joined += joined.empty() ? n : (" / " + n);
        }
        return joined;
    }

    bool controls_ui_poll(const uint8_t* rdram, _SDL_GameController* pad, uint16_t* gameButtons) {
        std::lock_guard<std::mutex> lock(g_mutex);
        *gameButtons = 0;
        const bool onScreen = (rdram != nullptr) && (std::strcmp(rs64_state_current_id(), "menu.controller_settings") == 0);
        const uint32_t now = (pad != nullptr) ? read_controls(pad) : 0;
        const uint32_t nav = (pad != nullptr) ? read_navigation(pad) : 0;
        const uint32_t pressedNow = now & ~g_previous;
        const uint32_t navPressed = nav & ~g_previousNav;
        g_previous = now;
        g_previousNav = nav;
        if (!onScreen || (pad == nullptr)) {
            g_state = vg::ControlsPanelState{};
            return false;
        }

        using Mode = vg::ControlsPanelState::Mode;
        const int count = vg::controlsActionCount();
        switch (g_state.mode) {
            case Mode::Choose:
                if (navPressed & NavUp) {
                    g_state.selected = (g_state.selected + count - 1) % count;
                }
                if (navPressed & NavDown) {
                    g_state.selected = (g_state.selected + 1) % count;
                }
                if (pressedNow & Cross) {
                    g_state.mode = Mode::Listen;
                    g_state.message.clear();
                    g_armed = false;
                    g_captured = 0;
                    g_listenStart = SDL_GetTicks();
                    return true;
                }
                if (pressedNow & Triangle) {
                    rs64_input_bindings_edit([](Bindings& b) { rs64::input::apply_vita_pad_defaults(b); });
                    g_state.message = "VITA DEFAULT CONTROLS RESTORED";
                    fprintf(stderr, "[controls] Vita defaults restored\n");
                }
                if (nav & NavLeft) {
                    *gameButtons |= N64DpadLeft;
                }
                if (nav & NavRight) {
                    *gameButtons |= N64DpadRight;
                }
                if (now & Circle) {
                    *gameButtons |= N64B;
                }
                return true;
            case Mode::Listen:
                // Wait for the CROSS that started listening to be released, then collect until everything is released again.
                if (!g_armed) {
                    g_armed = (now == 0);
                    return true;
                }
                if (now != 0) {
                    g_captured |= now;
                    return true;
                }
                if (g_captured != 0) {
                    assign(rdram, g_captured);
                    g_state.mode = Mode::Choose;
                }
                else if ((SDL_GetTicks() - g_listenStart) > ListenTimeoutMs) {
                    g_state.mode = Mode::Choose;
                    g_state.message = "NO BUTTON PRESSED";
                }
                return true;
        }
        return false;
    }

    vg::ControlsPanelState controls_ui_state() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_state;
    }
}
