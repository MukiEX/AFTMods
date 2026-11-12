// [audio] headphones: the game's sound rendered for headphones, as if from two speakers in a
// small, medium or large room. The rendering is in hp_core.c (LakeHP's engine); this file
// hooks the game's mixer and reads the settings.
//
// Every output path pulls the game's sound through one function, the mixer's fill
// (0x140627370): the game's own WASAPI thread, PD Loader's DivaSound, and the DivaRecomp
// runtime's audio thread. The hook renders what it returns, in place.
//
// Under the DivaRecomp runtime (Android), the runtime renders the same rooms itself, compiled
// for the device (this code would run recompiled, several times slower, on the audio thread);
// it sets DIVARECOMP_HEADPHONES=1 and this hook only passes the sound through.
//
// The room filters come from a filter DLL in plugins: `filters` names it; with "auto" it's
// DolbyHph.dll when that's there (the original headphone engine's, from your own copy), else
// LakeHP_OpenRooms.dll (LakeHP's open stand-in, shipped with DivaLinux).

#include "hp_core.h"
#include "../common/util.hpp"
#include <Helpers.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
    // void fill(audioMixer* mixer, int16_t* buf, uint64_t frames, bool hp_volume, bool invert)
    constexpr uint64_t ADDR_AUDIO_FILL = 0x140627370;
    // audioMixer: +0 its init class; init class: +0x58 channels, +0x60 sample rate
    // (as the DivaRecomp runtime's divasound.c and PD Loader's DivaSound use them)
    constexpr size_t MIXER_CLASS = 0x00, CLASS_CHANNELS = 0x58, CLASS_RATE = 0x60;

    struct Setup {
        int room = 0;                                    // 0 off, 1 small, 2 medium, 3 large
        std::shared_ptr<const std::vector<uint8_t>> image;   // the filter DLL
        std::string name;                                // its file name, for the log
    };

    std::mutex g_setup_lock;
    Setup g_setup;                   // under g_setup_lock
    std::atomic<uint32_t> g_setup_gen{ 0 };
    bool g_hooked = false;
    bool g_runtime_renders = false;   // DIVARECOMP_HEADPHONES=1

    // The audio thread's own
    struct Audio {
        uint32_t gen = 0;
        Setup setup;
        hp_state* hp = nullptr;
        uint32_t rate = 0;
        bool active = false;
        bool failed_logged = false;
    } g_audio;

    std::shared_ptr<const std::vector<uint8_t>> read_file(const std::wstring& path) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return nullptr;
        LARGE_INTEGER size = {};
        auto data = std::make_shared<std::vector<uint8_t>>();
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20)) {
            data->resize((size_t)size.QuadPart);
            DWORD got = 0;
            if (!ReadFile(f, data->data(), (DWORD)data->size(), &got, nullptr) || got != data->size())
                data.reset();
        }
        else
            data.reset();
        CloseHandle(f);
        return data;
    }

    std::wstring plugins_dir(const wchar_t* config_file) {
        std::wstring p = config_file;
        const size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
    }

    // Audio thread: (re)loads the room for the stream's rate when the settings or the rate change.
    void configure(uint32_t rate) {
        Audio& a = g_audio;
        const uint32_t gen = g_setup_gen.load(std::memory_order_acquire);
        if (gen == a.gen && rate == a.rate)
            return;
        if (gen != a.gen) {
            std::lock_guard<std::mutex> lock(g_setup_lock);
            a.setup = g_setup;
            a.gen = gen;
            a.failed_logged = false;
        }
        a.rate = rate;
        a.active = false;
        if (!a.setup.room || !a.setup.image)
            return;
        const uint8_t* blob = nullptr;
        size_t len = 0;
        if (hp_find_filter(a.setup.image->data(), a.setup.image->size(), a.setup.room, rate, &blob, &len)) {
            if (!a.failed_logged)
                dl::log("[Audio] Headphones: %s has no room %d filter for %u Hz; sound left as it is",
                    a.setup.name.c_str(), a.setup.room, rate);
            a.failed_logged = true;
            return;
        }
        if (!a.hp && !(a.hp = hp_create()))
            return;
        if (hp_load(a.hp, blob, len)) {
            if (!a.failed_logged)
                dl::log("[Audio] Headphones: the room %d filter in %s didn't load", a.setup.room, a.setup.name.c_str());
            a.failed_logged = true;
            return;
        }
        a.active = true;
        static const char* const rooms[] = { "", "small", "medium", "large" };
        dl::log("[Audio] Headphones: %s room from %s at %u Hz (adds %.0f ms)",
            rooms[a.setup.room], a.setup.name.c_str(), rate, hp_latency() * 1000.0 / rate);
    }
}

HOOK(void, __fastcall, AudioFill, ADDR_AUDIO_FILL, void* mixer, int16_t* buf, uint64_t frames, bool hp_volume, bool invert) {
    originalAudioFill(mixer, buf, frames, hp_volume, invert);
    if (!mixer || !buf || !frames || frames > (1u << 20))
        return;
    const uint8_t* cls = *(const uint8_t* const*)((const uint8_t*)mixer + MIXER_CLASS);
    if (!cls)
        return;
    const int32_t channels = *(const int32_t*)(cls + CLASS_CHANNELS);
    const uint32_t rate = *(const uint32_t*)(cls + CLASS_RATE);
    if (channels < 2 || channels > 8 || !rate)
        return;
    configure(rate);
    if (g_audio.active)
        hp_process(g_audio.hp, buf, (int)frames, channels);
}

// Reads [audio] (at start-up and when the settings menu changes it). The audio thread picks
// the new settings up on its next fill.
extern "C" void divalinux_audio_install(const wchar_t* config_file) {
    char env[8] = {};
    g_runtime_renders = GetEnvironmentVariableA("DIVARECOMP_HEADPHONES", env, sizeof(env)) && env[0] == '1';
    if (g_runtime_renders) {
        static bool said = false;
        if (!said)
            dl::log("[Audio] Headphones: rendered by the DivaRecomp runtime");
        said = true;
        return;
    }
    int room = (int)GetPrivateProfileIntW(L"audio", L"headphones", 0, config_file);
    if (room < 0 || room > 3)
        room = 0;
    wchar_t name[MAX_PATH] = {};
    GetPrivateProfileStringW(L"audio", L"filters", L"auto", name, MAX_PATH, config_file);

    Setup s;
    s.room = room;
    if (room) {
        const std::wstring dir = plugins_dir(config_file);
        std::vector<std::wstring> tries;
        if (_wcsicmp(name, L"auto") && name[0])
            tries.push_back(name);
        else
            tries = { L"DolbyHph.dll", L"LakeHP_OpenRooms.dll" };
        for (const std::wstring& t : tries)
            if ((s.image = read_file(dir + t))) {
                s.name = dl::narrow(t);
                break;
            }
        if (!s.image)
            dl::log("[Audio] Headphones: no filter DLL in plugins (%s); sound left as it is",
                tries.size() == 1 ? dl::narrow(tries[0]).c_str() : "DolbyHph.dll or LakeHP_OpenRooms.dll");
    }
    {
        std::lock_guard<std::mutex> lock(g_setup_lock);
        g_setup = s;
    }
    g_setup_gen.fetch_add(1, std::memory_order_release);

    // Hooked at start-up, before the game's audio starts (so not while it runs); with the
    // effect off the hook just passes the sound through.
    if (!g_hooked) {
        INSTALL_HOOK(AudioFill);
        g_hooked = true;
    }
    if (!room)
        dl::log("[Audio] Headphones: off");
}
