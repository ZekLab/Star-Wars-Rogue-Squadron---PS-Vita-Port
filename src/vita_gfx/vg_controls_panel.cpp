#include "vg_controls_panel.h"

namespace vg {
    // gGameSettings.controllerSetting, and the table of the four presets' action -> N64 button masks (18 u16 each).
    constexpr uint32_t ControllerSettingAddress = 0x130B45;
    constexpr uint32_t PresetTableAddress = 0x109AD0;
    constexpr uint32_t PresetCount = 4;

    struct Action {
        const char *label;
        uint8_t first, last;
    };

    // Indices into a preset's array; the three brake slots show as one row.
    static const Action Actions[] = {
        { "PAUSE", 0, 0 },
        { "THRUST", 12, 12 },
        { "BRAKES", 9, 11 },
        { "FIRE BLASTERS", 13, 13 },
        { "FIRE SECONDARY", 14, 14 },
        { "FIRE MODE", 15, 15 },
        { "ROLL", 16, 16 },
        { "SPECIAL", 17, 17 },
        { "LOOK AROUND", 3, 3 },
        { "SWITCH VIEW", 1, 1 },
        { "COCKPIT VIEW", 4, 4 },
        { "CLOSE VIEW", 5, 5 },
        { "STANDARD VIEW", 6, 6 },
        { "DROP CAMERA", 2, 2 },
    };
    constexpr int ActionCount = int(sizeof(Actions) / sizeof(Actions[0]));

    static uint32_t read8(const uint8_t *rdram, uint32_t address) {
        return rdram[(address & 0xFFFFFF) ^ 3];
    }

    static uint32_t read16(const uint8_t *rdram, uint32_t address) {
        return (read8(rdram, address) << 8) | read8(rdram, address + 1);
    }

    static uint32_t read32(const uint8_t *rdram, uint32_t address) {
        return (read16(rdram, address) << 16) | read16(rdram, address + 2);
    }

    // Address of the preset being shown, 0 when RDRAM holds none.
    static uint32_t presetTable(const uint8_t *rdram) {
        const uint32_t preset = read8(rdram, ControllerSettingAddress);
        if (preset >= PresetCount) {
            return 0;
        }
        const uint32_t table = read32(rdram, PresetTableAddress + 4 * preset);
        return ((table & 0xFF800000) == 0x80000000) ? table : 0;
    }

    int controlsActionCount() {
        return ActionCount;
    }

    const char *controlsActionLabel(int action) {
        return ((action >= 0) && (action < ActionCount)) ? Actions[action].label : "";
    }

    uint16_t controlsActionMask(const uint8_t *rdram, int action) {
        const uint32_t table = presetTable(rdram);
        if ((table == 0) || (action < 0) || (action >= ActionCount)) {
            return 0;
        }
        uint16_t mask = 0;
        for (uint32_t i = Actions[action].first; i <= Actions[action].last; i++) {
            mask |= uint16_t(read16(rdram, table + 2 * i));
        }
        return mask;
    }

    uint16_t controlsActionBit(const uint8_t *rdram, int action) {
        const uint16_t mask = controlsActionMask(rdram, action);
        uint16_t others = 0;
        for (int i = 0; i < ActionCount; i++) {
            if (i != action) {
                others |= controlsActionMask(rdram, i);
            }
        }
        uint16_t first = 0;
        for (int bit = 15; bit >= 0; bit--) {
            const uint16_t b = uint16_t(1u << bit);
            if ((mask & b) == 0) {
                continue;
            }
            if ((others & b) == 0) {
                return b;
            }
            if (first == 0) {
                first = b;
            }
        }
        return first;
    }

    std::vector<OverlayItem> controlsPanel(const uint8_t *rdram, const std::function<std::string(uint16_t)> &buttonNames,
        const ControlsPanelState &state)
    {
        std::vector<OverlayItem> items;
        if (presetTable(rdram) == 0) {
            return items;
        }

        // A translucent rounded panel between the game's title and BACK, wide enough to dim the game's N64 callouts; actions end at the
        // divider and their controls start after it.
        constexpr float PanelLeft = 0.04f, PanelRight = 0.96f, PanelTop = 0.18f, PanelBottom = 0.885f;
        constexpr float HighlightLeft = 0.25f, HighlightRight = 0.75f;
        constexpr float Divider = 0.49f, Gap = 0.016f, Top = PanelTop + 0.025f, TextHeight = 0.038f, RowStep = 0.042f;
        constexpr float RowsBottom = Top + RowStep * ActionCount, FooterHeight = 0.03f;
        auto panel = [&](float x, float y, float w, float h, float radius, uint32_t color) {
            OverlayItem item;
            item.kind = OverlayItem::Kind::Panel;
            item.x = x;
            item.y = y;
            item.w = w;
            item.h = h;
            item.radius = radius;
            item.color = color;
            items.push_back(item);
        };
        auto text = [&](float x, float y, float h, uint32_t color, OverlayItem::Align align, const std::string &s) {
            OverlayItem item;
            item.align = align;
            item.x = x;
            item.y = y;
            item.h = h;
            item.color = color;
            item.text = s;
            items.push_back(item);
        };

        panel(PanelLeft, PanelTop, PanelRight - PanelLeft, PanelBottom - PanelTop, 0.03f, 0x080B14D8);
        panel(HighlightLeft, Top + RowStep * state.selected - 0.004f, HighlightRight - HighlightLeft, RowStep, 0.012f, 0x3D66C8A0);
        panel(Divider - 0.0008f, Top, 0.0016f, RowsBottom - Top - 0.004f, 0.0f, 0xFFFFFF28);

        const bool listening = state.mode == ControlsPanelState::Mode::Listen;
        for (int a = 0; a < ActionCount; a++) {
            const float y = Top + RowStep * a;
            const uint16_t mask = controlsActionMask(rdram, a);
            std::string controls;
            for (int bit = 15; bit >= 0; bit--) {
                if ((mask & (1u << bit)) == 0) {
                    continue;
                }
                const std::string names = buttonNames(uint16_t(1u << bit));
                if (!names.empty()) {
                    controls += controls.empty() ? names : (" / " + names);
                }
            }

            const bool selected = a == state.selected;
            text(Divider - Gap, y, TextHeight, selected ? 0xFFFFFFFF : 0xD2D7E0FF, OverlayItem::Align::Right, Actions[a].label);
            if (selected && listening) {
                text(Divider + Gap, y, TextHeight, 0x6FD3FFFF, OverlayItem::Align::Left, "PRESS, THEN RELEASE");
            }
            else {
                text(Divider + Gap, y, TextHeight, controls.empty() ? 0xFF6A5AFF : 0xF2C443FF, OverlayItem::Align::Left, controls.empty() ? "NOT SET" : controls);
            }
        }

        const float center = (PanelLeft + PanelRight) * 0.5f;
        const char *help = listening ? "ONE BUTTON, OR TWO HELD TOGETHER (E.G. L + R)" : "CROSS: CHANGE   TRIANGLE: VITA DEFAULTS   LEFT/RIGHT: PRESET   CIRCLE: BACK";
        text(center, RowsBottom + 0.008f, FooterHeight, 0x9FA8B8FF, OverlayItem::Align::Center, help);
        if (!state.message.empty()) {
            text(center, RowsBottom + 0.008f + FooterHeight + 0.004f, FooterHeight, 0x8CE68CFF, OverlayItem::Align::Center, state.message);
        }
        return items;
    }
}
