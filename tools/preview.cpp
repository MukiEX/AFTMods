// Linux preview: runs the launcher UI against sample config files, feeds it simulated
// controller input, rasterises ImGui's draw data on the CPU and writes PNG screenshots.
// Used to review the layout without a Windows machine.
//
//   preview <font-regular.ttf> <font-bold.ttf> <out-dir> [width height]

#include "../src/app.hpp"
#include "../src/ini.hpp"
#include <imgui.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>

using namespace lfs;

// ------------------------------------------------------------ rasteriser

struct Canvas {
    int w, h;
    std::vector<float> px; // RGB, linear 0..1 straight blend in sRGB space like the GPU path
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 3, 0.0f) {}
};

static void sample(ImTextureData* t, float u, float v, float out[4]) {
    if (!t) { out[0] = out[1] = out[2] = out[3] = 1.0f; return; }
    int x = std::min(t->Width - 1, std::max(0, (int)(u * t->Width)));
    int y = std::min(t->Height - 1, std::max(0, (int)(v * t->Height)));
    // bilinear
    float fx = u * t->Width - 0.5f, fy = v * t->Height - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float ax = fx - x0, ay = fy - y0;
    float acc[4] = { 0, 0, 0, 0 };
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int sx = std::min(t->Width - 1, std::max(0, x0 + i));
            int sy = std::min(t->Height - 1, std::max(0, y0 + j));
            float wgt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
            const unsigned char* p = (const unsigned char*)t->GetPixelsAt(sx, sy);
            if (t->Format == ImTextureFormat_Alpha8) {
                acc[0] += wgt; acc[1] += wgt; acc[2] += wgt; acc[3] += wgt * p[0] / 255.0f;
            }
            else
                for (int c = 0; c < 4; c++) acc[c] += wgt * p[c] / 255.0f;
        }
    (void)x; (void)y;
    memcpy(out, acc, sizeof(acc));
}

static void raster_tri(Canvas& cv, const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, ImTextureData* tex, const ImVec4& clip) {
    float minx = std::max(clip.x, std::floor(std::min({ a.pos.x, b.pos.x, c.pos.x })));
    float maxx = std::min(clip.z, std::ceil(std::max({ a.pos.x, b.pos.x, c.pos.x })));
    float miny = std::max(clip.y, std::floor(std::min({ a.pos.y, b.pos.y, c.pos.y })));
    float maxy = std::min(clip.w, std::ceil(std::max({ a.pos.y, b.pos.y, c.pos.y })));
    minx = std::max(minx, 0.0f); miny = std::max(miny, 0.0f);
    maxx = std::min(maxx, (float)cv.w); maxy = std::min(maxy, (float)cv.h);
    float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
    if (std::fabs(area) < 1e-8f) return;
    auto col = [](ImU32 c, int i) { return ((c >> (i * 8)) & 0xFF) / 255.0f; };
    for (int y = (int)miny; y < (int)maxy; y++)
        for (int x = (int)minx; x < (int)maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
            float w1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
            float u = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x;
            float v = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y;
            float t[4];
            sample(tex, u, v, t);
            float k[4];
            for (int i = 0; i < 4; i++)
                k[i] = (w0 * col(a.col, i) + w1 * col(b.col, i) + w2 * col(c.col, i)) * t[i];
            float* d = &cv.px[((size_t)y * cv.w + x) * 3];
            for (int i = 0; i < 3; i++)
                d[i] = d[i] * (1.0f - k[3]) + k[i] * k[3];
        }
}

static void process_textures(ImDrawData* dd) {
    if (dd->Textures)
        for (ImTextureData* t : *dd->Textures) {
            if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
                t->SetTexID((ImTextureID)(intptr_t)t);
                t->SetStatus(ImTextureStatus_OK);
            }
            else if (t->Status == ImTextureStatus_WantDestroy) {
                t->SetTexID(ImTextureID_Invalid);
                t->SetStatus(ImTextureStatus_Destroyed);
            }
        }
}

static void render(Canvas& cv, ImDrawData* dd) {
    process_textures(dd);
    for (ImDrawList* dl : dd->CmdLists) {
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            ImTextureData* tex = cmd.TexRef._TexData;
            if (!tex && cmd.TexRef._TexID != ImTextureID_Invalid)
                tex = (ImTextureData*)(intptr_t)cmd.TexRef._TexID;
            for (unsigned i = 0; i < cmd.ElemCount; i += 3) {
                const ImDrawIdx* idx = dl->IdxBuffer.Data + cmd.IdxOffset + i;
                const ImDrawVert* v = dl->VtxBuffer.Data + cmd.VtxOffset;
                raster_tri(cv, v[idx[0]], v[idx[1]], v[idx[2]], tex, cmd.ClipRect);
            }
        }
    }
}

static void save_png(const Canvas& cv, const std::string& path) {
    std::vector<unsigned char> out((size_t)cv.w * cv.h * 3);
    for (size_t i = 0; i < out.size(); i++)
        out[i] = (unsigned char)std::lround(std::min(1.0f, std::max(0.0f, cv.px[i])) * 255.0f);
    stbi_write_png(path.c_str(), cv.w, cv.h, 3, out.data(), cv.w * 3);
    printf("wrote %s\n", path.c_str());
}

// ------------------------------------------------------------ sample data

static std::vector<char> read_file(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void write_text(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

static bool g_button_ran = false;
static void sample_button() { g_button_ran = true; }

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: preview <regular.ttf> <bold.ttf> <out-dir> [width height]\n");
        return 1;
    }
    std::string out = argv[3];
    int W = argc > 5 ? atoi(argv[4]) : 1280, H = argc > 5 ? atoi(argv[5]) : 800;
    mkdir(out.c_str(), 0755);
    std::string game = out + "/game";
    mkdir(game.c_str(), 0755);
    mkdir((game + "/plugins").c_str(), 0755);
    write_text(game + "/plugins/config.ini",
        "[resolution]\ndisplay=1\nwidth=-1\nheight=-1\n\n[plugins]\nDivaGL.dva=1\nDivaVK.dva=1\nNovidia.dva=0\n\n[patches]\nquick_start=1\n");
    write_text(game + "/plugins/playerdata.ini", "[playerdata]\nplayer_name=\xEF\xBC\xAE\xEF\xBC\xAF\xEF\xBC\x8D\xEF\xBC\xAE\xEF\xBC\xA1\xEF\xBC\xAD\xEF\xBC\xA5\n");

    Paths paths;
    std::wstring g = from_utf8(game);
    paths.game_dir = g;
    paths.plugins_dir = g + L"/plugins";
    paths.patches_dir = g + L"/patches";
    paths.config = g + L"/plugins/config.ini";
    paths.components = g + L"/plugins/components.ini";
    paths.playerdata = g + L"/plugins/playerdata.ini";
    paths.keyconfig = g + L"/plugins/keyconfig.ini";

    ScreenInfo screen;
    screen.resolutions = { {800,600}, {1024,768}, {1280,720}, {1280,800}, {1600,900}, {1920,1080}, {2560,1440} };
    screen.refresh_rates = { 60, 90, 120, 144 };

    std::vector<Page> pages;
    Page home; home.title = "Home"; home.kind = PageKind::Home;
    pages.push_back(home);
    for (Page& p : build_builtin_pages(paths, screen))
        pages.push_back(std::move(p));

    Page plugins; plugins.title = "Plugins"; plugins.kind = PageKind::Plugins;
    const std::wstring& C = paths.config;
    plugins.options = {
        make_plugin(L"DivaGL.dva", C, L"DivaGL", L"Replaces the game's renderer with an OpenGL 4.3 core one.", "Oct 15 2026", {
            make_section("Stereoscopic 3D"),
            make_bool(L"stereo", L"DivaGL", C, L"Side-by-side 3D", L"Renders the left and right eye next to each other.", false, false),
            make_int(L"stereo_separation", L"DivaGL", C, L"Eye separation", L"Distance between the eyes, in millimetres.", 63, 0, 200),
            make_choice(L"stereo_layout", L"DivaGL", C, L"Layout:", L"How the two views are packed.", 0, { L"Full width", L"Half width" }),
            make_button(L"Reset stereo settings", L"Puts the stereo options back to their defaults.", sample_button),
        }),
        make_plugin(L"DivaVK.dva", C, L"DivaVK", L"Runs DivaGL's OpenGL calls on Vulkan.", "Oct 15 2026", {}),
        make_plugin(L"DivaSound.dva", C, L"DivaSound", L"WASAPI/ASIO audio output.", "Jun 2 2024", {}),
        make_plugin(L"Novidia.dva", C, L"Novidia", L"Workarounds for non-Nvidia GPUs.", "Jan 3 2023", {}),
        make_plugin(L"ShaderPatch.dva", C, L"ShaderPatch", L"Patches shaders at load time.", "Jan 3 2023", {}),
        make_plugin(L"DivaImGui.dva", C, L"DivaImGui", L"In-game overlay and FPS counter.", "Mar 9 2024", {}),
    };
    pages.push_back(plugins);
    Page patches; patches.title = "Patches"; patches.kind = PageKind::Patches;
    patches.empty_text = "No custom patches found. Put .p or .p2 files in the patches folder to list them here.";
    pages.push_back(patches);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)W, (float)H);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_HasGamepad;
    io.BackendRendererName = "preview";

    auto reg = read_file(argv[1]), bold = read_file(argv[2]);
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    Fonts fonts;
    fonts.regular = io.Fonts->AddFontFromMemoryTTF(reg.data(), (int)reg.size(), 21.0f, &cfg);
    fonts.bold = io.Fonts->AddFontFromMemoryTTF(bold.data(), (int)bold.size(), 21.0f, &cfg);

    Host host;
    host.gpu = "AMD Custom GPU 0405 (RADV VANGOGH)";
    host.wine = "9.0 (Proton)";
    bool launched = false, quit = false;
    host.launch = [&]() { launched = true; };
    host.quit = [&]() { quit = true; };
    App app(std::move(pages), host, fonts);

    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    auto frame = [&](int n = 1) {
        for (int i = 0; i < n; i++) {
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            app.frame((float)W, (float)H);
            ImGui::Render();
            // let the atlas updates through without drawing
            ImDrawData* dd = ImGui::GetDrawData();
            process_textures(dd);
        }
    };
    auto press = [&](ImGuiKey k) {
        io.AddKeyEvent(k, true);
        frame();
        io.AddKeyEvent(k, false);
        frame();
    };
    auto shot = [&](const char* name) {
        frame(30); // let animations settle
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        app.frame((float)W, (float)H);
        ImGui::Render();
        Canvas cv(W, H);
        render(cv, ImGui::GetDrawData());
        char buf[64];
        snprintf(buf, sizeof(buf), "_%dx%d.png", W, H);
        save_png(cv, out + "/" + name + buf);
    };

    frame(3);
    shot("01_home");
    press(ImGuiKey_GamepadR1);          // Display
    shot("02_display");
    press(ImGuiKey_GamepadDpadDown);    // Resolution
    press(ImGuiKey_GamepadFaceDown);    // open picker
    shot("03_resolution_picker");
    press(ImGuiKey_GamepadFaceRight);
    press(ImGuiKey_GamepadR1);          // Graphics
    for (int i = 0; i < 11; i++) press(ImGuiKey_GamepadDpadDown);
    shot("04_graphics_scrolled");
    press(ImGuiKey_GamepadR1);          // Options
    shot("05_options");
    press(ImGuiKey_GamepadR1);          // Player
    press(ImGuiKey_GamepadFaceDown);    // keyboard for Player Name
    press(ImGuiKey_GamepadDpadDown);
    press(ImGuiKey_GamepadDpadRight);
    shot("06_keyboard");
    press(ImGuiKey_GamepadFaceRight);
    press(ImGuiKey_GamepadR1);          // Components
    press(ImGuiKey_GamepadR1);          // Plugins
    shot("07_plugins");
    // Mouse / touch: the button prompts work as buttons
    auto click = [&](float x, float y) {
        io.AddMousePosEvent(x, y);
        frame(2);
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
    };
    io.AddMousePosEvent(200, 770);      // hover "Y Plugin options" in the bottom bar
    shot("07b_prompt_hover");
    click(200, 770);
    shot("08_plugin_options");          // opened by the click
    click(170, 770);                    // "B Back"
    shot("08b_back_by_click");
    click(1110, 120);                   // the word "Options" on the focused plugin row
    shot("08c_row_options_click");
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    press(ImGuiKey_GamepadFaceRight);
    press(ImGuiKey_GamepadR1);          // Patches
    shot("09_patches");
    press(ImGuiKey_GamepadStart);       // Play -> blocked (DivaGL + DivaVK)
    shot("10_blocked");
    press(ImGuiKey_GamepadFaceDown);
    press(ImGuiKey_GamepadR1);          // Home
    press(ImGuiKey_GamepadBack);        // close with unsaved changes? (none yet) -> quits
    printf("launched=%d quit=%d button=%d\n", launched, quit, g_button_ran);

    ImGui::DestroyContext();
    return 0;
}
