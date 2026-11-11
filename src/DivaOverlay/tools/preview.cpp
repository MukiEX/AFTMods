// Linux preview: runs DivaOverlay's menu against sample settings files, feeds it scripted
// input, draws it with LauncherFS's CPU renderer over a game screenshot and writes PNGs.
// Used to review the layout without the game.
//
//   ov_preview <out-dir> [background.rgba W H] [--xr] [--windows]
//   (background: raw RGBA8, top row first)

#include "../ov_model.hpp"
#include "../ov_platform.hpp"
#include "../ov_ui.hpp"
#include "ini.hpp"
#include "soft_render.hpp"
#include <imgui.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <ctime>
#include <cstdlib>

extern const unsigned char dov_font_bold[];
extern const unsigned int dov_font_bold_size;
extern const unsigned char dov_font_regular[];
extern const unsigned int dov_font_regular_size;

// ------------------------------------------------------------ platform (POSIX)
static std::string host_path(const std::wstring& w) {
    std::string s = lfs::to_utf8(w);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}
namespace dov {
    bool read_file(const std::wstring& path, std::string& data) {
        std::ifstream f(host_path(path), std::ios::binary);
        if (!f) return false;
        std::stringstream ss; ss << f.rdbuf(); data = ss.str();
        return true;
    }
    bool write_file(const std::wstring& path, const std::string& data) {
        std::ofstream f(host_path(path), std::ios::binary);
        if (!f) return false;
        f << data;
        return true;
    }
    bool file_exists(const std::wstring& path) { struct stat st; return stat(host_path(path).c_str(), &st) == 0; }
    std::vector<std::wstring> list_dirs(const std::wstring& dir) {
        std::vector<std::wstring> out;
        if (DIR* d = opendir(host_path(dir).c_str())) {
            while (dirent* e = readdir(d))
                if (e->d_type == DT_DIR && e->d_name[0] != '.') out.push_back(lfs::from_utf8(e->d_name));
            closedir(d);
        }
        return out;
    }
}

// ------------------------------------------------------------ sample files
static void put(const std::string& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: ov_preview <out-dir> [bg.rgba W H] [--xr] [--windows]\n"); return 1; }
    std::string out = argv[1];
    std::vector<unsigned char> bg;
    int W = 1920, H = 1080;
    bool xr = false, windows = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--xr")) xr = true;
        else if (!strcmp(argv[i], "--windows")) windows = true;
        else if (i + 2 < argc) {
            std::ifstream f(argv[i], std::ios::binary);
            W = atoi(argv[i + 1]); H = atoi(argv[i + 2]);
            bg.assign((size_t)W * H * 4, 0);
            f.read((char*)bg.data(), bg.size());
            i += 2;
        }
    }
    mkdir(out.c_str(), 0755);
    std::string game = out + "/game";
    mkdir(game.c_str(), 0755);
    mkdir((game + "/plugins").c_str(), 0755);
    mkdir((game + "/drivers").c_str(), 0755);
    mkdir((game + "/drivers/turnip").c_str(), 0755);
    put(game + "/plugins/config.ini", "[resolution]\nwidth=1920\nheight=1080\nr.enable=1\nr.width=1920\nr.height=1088\n[graphics]\nfrm.motion.rate=300\n");
    put(game + "/plugins/components.ini", "[components]\nfast_loader=true\nfast_loader_speed=12\nframe_rate_manager=true\n");
    put(game + "/plugins/DivaLinux.ini", "[stereo]\nstereo_mode=1\nstereo_hud_parallax=-12\n[cursor]\npointer=1\n");
    put(game + "/divarecomp_env.txt", "# Odin 3\nDIVA_VULKAN_DRIVER=turnip   # Odin driver\n#DIVA_DOF=0\n");

    dov::ModelPaths p;
    p.game_dir = lfs::from_utf8(game);
    p.divalinux_ini = p.game_dir + L"/plugins/DivaLinux.ini";
    p.pd.game_dir = p.game_dir;
    p.pd.plugins_dir = p.game_dir + L"/plugins";
    p.pd.config = p.pd.plugins_dir + L"/config.ini";
    p.pd.components = p.pd.plugins_dir + L"/components.ini";
    p.pd.playerdata = p.pd.plugins_dir + L"/playerdata.ini";
    p.pd.keyconfig = p.pd.plugins_dir + L"/keyconfig.ini";
    if (!windows) p.env_file = p.game_dir + L"/divarecomp_env.txt";
    dov::LiveHooks hooks;
    hooks.set_option_int = [](const char* n, int v) { fprintf(stderr, "  live: SetOptionInt(%s, %d)\n", n, v); return true; };
    hooks.reload_cursor = []() { fprintf(stderr, "  live: pointer settings reloaded\n"); };
    hooks.reload_audio = []() { fprintf(stderr, "  live: audio settings reloaded\n"); };
    dov::Model model;
    model.build(p, !windows, xr, hooks);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)W, (float)H);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    ImFont* bold = io.Fonts->AddFontFromMemoryTTF((void*)dov_font_bold, (int)dov_font_bold_size, 30.0f, &cfg);
    ImFont* reg = io.Fonts->AddFontFromMemoryTTF((void*)dov_font_regular, (int)dov_font_regular_size, 24.0f, &cfg);

    dov::UiHost host;
    host.close_hint = windows ? "F10" : "Select+Start";
    host.slider_controls = getenv("OV_SLIDER") != nullptr;
    bool closed = false;
    host.close = [&]() { closed = true; fprintf(stderr, "  closed\n"); };
    host.restart = [&]() { fprintf(stderr, "  restart requested\n"); };
    dov::Ui ui(model, host, bold, reg);
    if (const char* rs = getenv("OV_ROW_SCALE"))
        ui.set_row_scale((float)atof(rs));
    if (const char* hs = getenv("OV_HELP_SCALE"))
        ui.set_help_scale((float)atof(hs));
    ui.open();

    lfs::SoftCanvas cv;
    cv.keep_alpha = true;
    int shot = 0;
    auto frame = [&](const dov::Input& in) {
        io.DeltaTime = in.dt;
        ImGui::NewFrame();
        ui.frame(in, (float)W, (float)H, ImGui::GetBackgroundDrawList());
        ImGui::Render();
        lfs::soft_update_textures(ImGui::GetDrawData());
    };
    auto snap = [&](const char* name) {
        cv.resize(W, H);
        cv.clear(0);
        lfs::soft_render(cv, ImGui::GetDrawData());
        std::vector<unsigned char> img((size_t)W * H * 3);
        for (size_t i = 0; i < (size_t)W * H; i++) {
            uint32_t c = cv.px[i];
            float a = (c >> 24) / 255.0f;
            unsigned char src[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
            for (int k = 0; k < 3; k++) {
                float b = bg.empty() ? (k == 2 ? 120.0f : 70.0f + 30.0f * ((i / W) / (float)H)) : bg[i * 4 + k];
                img[i * 3 + k] = (unsigned char)(src[k] * a + b * (1 - a) + 0.5f);
            }
        }
        char fn[512];
        snprintf(fn, sizeof(fn), "%s/%02d_%s.png", out.c_str(), shot++, name);
        stbi_write_png(fn, W, H, 3, img.data(), W * 3);
        fprintf(stderr, "wrote %s\n", fn);
    };
    auto idle = [&](int n, dov::Device dev = dov::Device::Pad) { dov::Input in; in.device = dev; for (int i = 0; i < n; i++) frame(in); };
    auto press = [&](bool dov::Input::*b, int hold = 2, dov::Device dev = dov::Device::Pad) {
        dov::Input in; in.device = dev; in.*b = true;
        for (int i = 0; i < hold; i++) frame(in);
        idle(2, dev);
    };
    auto tap = [&](float x, float y) {
        dov::Input in; in.device = dov::Device::Pointer; in.pointer_valid = true; in.px = x; in.py = y;
        frame(in);
        in.pointer_down = true; frame(in); frame(in);
        in.pointer_down = false; frame(in);
        idle(20, dov::Device::Pointer);
    };

    idle(30);
    if (getenv("OV_BENCH")) {
        auto t = [] { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; };
        double a = t(); for (int i = 0; i < 20; i++) idle(1); double b = t();
        cv.resize(W, H);
        double c = t(); for (int i = 0; i < 5; i++) { cv.clear(0); lfs::soft_render(cv, ImGui::GetDrawData()); } double d = t();
        ImDrawData* dd = ImGui::GetDrawData(); size_t vb = 0; for (int i = 0; i < dd->CmdListsCount; i++) vb += dd->CmdLists[i]->VtxBuffer.Size;
        fprintf(stderr, "bench: ui frame %.2f ms, soft_render %.2f ms, %zu vertices\n", (b - a) / 20, (d - c) / 5, vb);
    }
    snap("home");
    press(&dov::Input::ok);           // Stereo 3D
    idle(30);
    snap("stereo_top");               // its first setting: the longest help text
    for (int i = 0; i < 5; i++) press(&dov::Input::down);
    idle(30);
    snap("stereo");
    press(&dov::Input::left);
    press(&dov::Input::left);
    idle(30);
    snap("stereo_changed");
    press(&dov::Input::back);
    // to the Android Graphics section
    for (int i = 0; i < 40 && !closed; i++) {
        press(&dov::Input::down);
    }
    idle(30);
    snap("home_scrolled");
    press(&dov::Input::page_up);
    press(&dov::Input::page_up);
    idle(10);
    for (int i = 0; i < 3; i++) press(&dov::Input::up);
    idle(30);
    press(&dov::Input::ok);
    idle(30);
    press(&dov::Input::down);
    press(&dov::Input::right);
    idle(30);
    snap("section");
    ui.flush();
    std::string env;
    dov::read_file(p.env_file.empty() ? L"" : p.env_file, env);
    fprintf(stderr, "divarecomp_env.txt now:\n%s", env.c_str());
    // touch: back, then tap a row
    tap(40, 105);
    idle(30, dov::Device::Pointer);
    snap("touch_home");
    return 0;
}
