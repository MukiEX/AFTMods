// DivaLinux's own features, installed from DivaGL's DllMain (src/DivaGL/dllmain.cpp) once it
// has read the renderer's settings. Each feature reads its own section of DivaLinux.ini.
//
//   gl_bridge/   the game's GL calls to DivaVK (Vulkan), WGL emulation; installed by divagl_main
//   movie/       [movie]   the FFmpeg movie player
//   audio/       [audio]   headphone surround (LakeHP's engine) on the game's sound
//   overlay/     [cursor]  the drawn mouse pointer, and the overlay compositor
//   ../DivaOverlay [overlay] the in-game settings menu
//   game/        config.ini [patches] normal_stages / protected_stages: songs per credit
//   vfs/         [loading] the file lookup cache and archives
//   debug/       [debug]   the file I/O trace
//   common/      helpers they share (import patching, logging, DivaLinux.ini migration)
#include <windows.h>

extern "C" void divalinux_movie_install(const wchar_t* config_file);                // movie/movie.cpp
extern "C" void divalinux_cursor_install(const wchar_t* config_file);               // overlay/cursor.cpp
extern "C" void divalinux_audio_install(const wchar_t* config_file);                // audio/headphones.cpp
extern "C" void divaoverlay_install(HMODULE self, const wchar_t* divalinux_ini);    // ../DivaOverlay/ov_host.cpp
extern "C" void divalinux_stages_install(const wchar_t* game_dir);                  // game/stages.cpp
extern "C" void divalinux_vfs_install(HMODULE self, const wchar_t* config_file);    // vfs/vfs.cpp
extern "C" void divalinux_io_trace_install(HMODULE self, const wchar_t* config_file); // debug/io_trace.cpp

extern "C" void divalinux_install(HMODULE self, const wchar_t* config_file, const wchar_t* game_dir) {
    divalinux_movie_install(config_file);
    divalinux_cursor_install(config_file);
    divalinux_audio_install(config_file);
    divaoverlay_install(self, config_file);   // the in-game settings menu
    divalinux_stages_install(game_dir);
    // The file layer's hooks go in first, so the trace (when on) measures what it does.
    divalinux_vfs_install(self, config_file);
    divalinux_io_trace_install(self, config_file);
}
