// The few things DivaOverlay's portable code needs from where it runs (DivaLinux.dva on
// Windows or the DivaRecomp runtime; the Linux preview tool).
#pragma once

#include <string>
#include <vector>

namespace dov {
    // Whole-file access; paths are the game's (Windows) paths.
    bool read_file(const std::wstring& path, std::string& data);
    bool write_file(const std::wstring& path, const std::string& data);
    bool file_exists(const std::wstring& path);
    // Names of the folders inside a folder (for the custom driver list), sorted.
    std::vector<std::wstring> list_dirs(const std::wstring& dir);
}
