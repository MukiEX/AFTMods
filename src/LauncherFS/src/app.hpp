// The launcher UI: platform-independent (Dear ImGui draw lists + ImGui key state).
#pragma once

#include "model.hpp"
#include <functional>

struct ImFont;

namespace lfs {
    struct Host {
        std::string gpu;           // adapter name, may be empty
        std::string wine;          // Wine version, empty on Windows
        std::function<void()> launch;   // start the game; does not return on success
        std::function<void()> quit;     // close without launching
        std::function<void()> toggle_windowed; // optional
        // In-game overlay ([launcher] overlay=1): the menu sits over the running game. "Play"
        // restarts the game (launch), "Close" goes back to it (quit), and exit_game ends it.
        bool in_game = false;
        std::function<void()> exit_game;
    };

    struct Fonts {
        ImFont* regular = nullptr;
        ImFont* bold = nullptr;
    };

    class App {
    public:
        App(std::vector<Page> pages, Host host, Fonts fonts);
        ~App();

        // Draws one frame. Call between ImGui::NewFrame() and ImGui::Render().
        void frame(float width, float height);

        // Requests the close flow (window close button, Alt+F4).
        void request_close();

        // For the preview tool: show a given overlay state.
        struct Impl;
        Impl* impl() { return m; }

    private:
        Impl* m;
    };
}
