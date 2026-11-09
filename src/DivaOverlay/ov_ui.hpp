// DivaOverlay's menu, in the style of the game's Key Config screen: white, slightly see-through
// slanted bars on an arc, a value box with arrows on the right of each, the help for the
// selected row in a panel beside them. Drawn with Dear ImGui's draw lists (no ImGui windows),
// so it works with any ImGui renderer (DivaLinux uses LauncherFS's CPU one).
#pragma once

#include "ov_model.hpp"
#include <functional>
#include <string>

struct ImFont;
struct ImDrawList;

namespace dov {
    enum class Device { Pad, Keys, Pointer };

    // One frame of input, in layout pixels.
    struct Input {
        float dt = 1.0f / 60.0f;
        // held this frame
        bool up = false, down = false, left = false, right = false;
        bool ok = false, back = false, page_up = false, page_down = false;
        // pointer (mouse, touch, XR controller ray)
        bool pointer_valid = false, pointer_down = false;
        float px = 0, py = 0;
        float wheel = 0;   // + = up
        Device device = Device::Pad;   // what was used last (for the button hints)
    };

    struct UiHost {
        std::function<void()> close;     // hide the menu (settings are saved first)
        std::function<void()> restart;   // restart the game to apply the rest (saved first)
        std::string close_hint;          // how to close, e.g. "F10" or "Select + Start"
        bool pad_ps = false;             // PlayStation button names in the hints
    };

    class Ui {
    public:
        Ui(Model& model, UiHost host, ImFont* bold, ImFont* regular);
        ~Ui();
        Ui(const Ui&) = delete;
        Ui& operator=(const Ui&) = delete;

        // The menu opens: re-reads the files and goes back to the home page.
        void open();
        // Draws one frame into dl, laid out for a w x h picture.
        void frame(const Input& in, float w, float h, ImDrawList* dl);
        // Saves anything changed (also done on its own shortly after each change).
        void flush();
        // Something is still moving (keep drawing frames).
        bool animating() const;

        struct Impl;
        Impl* impl() { return m; }
    private:
        Impl* m;
    };
}
