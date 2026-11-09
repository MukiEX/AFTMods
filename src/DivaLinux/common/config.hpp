// DivaLinux.ini: carrying settings over from the plugins DivaLinux replaced.
#pragma once

// First run after switching from the separate plugins: start from their settings.
// DivaVK.ini (or DivaGL.ini) has the renderer and stereo options; DivaMovie.ini's [general]
// ffmpeg_player and movie_audio move to [movie]. Only done while DivaLinux.ini doesn't exist.
// plugins_dir ends with a separator.
extern "C" void divalinux_migrate_config(const wchar_t* plugins_dir, const wchar_t* config_file);
