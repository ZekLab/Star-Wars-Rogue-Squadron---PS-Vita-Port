// main.h — the public C-linkage surface of main.cpp consumed by other host
// TUs (the window/fullscreen + quit hooks used by menu_config.cpp).
#ifndef RS64_MAIN_H
#define RS64_MAIN_H

extern "C" {
    int  rs64_get_fullscreen(void);
    void rs64_toggle_fullscreen(void);
    void rs64_menu_request_quit(void);
    void rs64_touch_layout_request(void);
}

#include "input_bindings.h"

#include <functional>

// Copy of the live input bindings (the Vita's controller-settings panel lists them).
rs64::input::Bindings rs64_input_bindings_snapshot();
// Edits the live bindings under their lock and saves them to roguesq_input.json.
void rs64_input_bindings_edit(const std::function<void(rs64::input::Bindings&)>& edit);

#endif // RS64_MAIN_H
