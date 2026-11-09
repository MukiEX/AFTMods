#include "config.hpp"
#include "util.hpp"
#include <string>

extern "C" void divalinux_migrate_config(const wchar_t* plugins_dir, const wchar_t* config_file) {
    if (GetFileAttributesW(config_file) != INVALID_FILE_ATTRIBUTES)
        return;
    const std::wstring dir = plugins_dir;
    for (const wchar_t* old : { L"DivaVK.ini", L"DivaGL.ini" })
        if (CopyFileW((dir + old).c_str(), config_file, TRUE)) {
            dl::log("Created DivaLinux.ini from %ls", old);
            break;
        }
    const std::wstring movie_ini = dir + L"DivaMovie.ini";
    if (GetFileAttributesW(movie_ini.c_str()) != INVALID_FILE_ATTRIBUTES) {
        for (const wchar_t* key : { L"ffmpeg_player", L"movie_audio" }) {
            wchar_t value[16] = {};
            if (GetPrivateProfileStringW(L"general", key, L"", value, 16, movie_ini.c_str()))
                WritePrivateProfileStringW(L"movie", key, value, config_file);
        }
        dl::log("Copied movie settings from DivaMovie.ini");
    }
}
