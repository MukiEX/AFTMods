// rom.zip and mdata\<pack>.zip: finding them, and turning their index into the folders they
// stand for.

#include "vfs_internal.hpp"
#include "zip_index.hpp"

namespace vfs {
    Archive* ArchiveFor(const std::wstring& dir) {
        for (Archive* a : g_archive_list)
            if (a->ok && (dir == a->root || (dir.size() > a->root.size() && !dir.compare(0, a->root.size(), a->root)
                && dir[a->root.size()] == L'\\')))
                return a;
        return nullptr;
    }

    // The folder holding an archive's stand-in folder (the game folder for rom.zip, the mdata
    // folder for packs): its listings must show the archives even with the lookup cache off.
    bool IsArchiveParent(const std::wstring& dir) {
        for (Archive* a : g_archive_list) {
            if (!a->ok)
                continue;
            size_t slash = a->root.find_last_of(L'\\');
            if (slash == dir.size() && !a->root.compare(0, slash, dir))
                return true;
        }
        return false;
    }

    bool ReadAt(HANDLE file, uint64_t offset, void* buffer, DWORD size) {
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD got = 0;
        return real_ReadFile(file, buffer, size, &got, &ov) && got == size;
    }

    static Dir& ArchiveDir(const std::wstring& dir) {
        Dir& d = g_dirs[dir];
        d.exists = true;
        d.archived = true;
        return d;
    }

    // Adds folder `rel` (archive-relative, as in the archive, backslashes) and its parents.
    static void AddArchiveDirs(Archive* a, const std::wstring& rel) {
        std::wstring lower = rel;
        dl::lower(lower);
        ArchiveDir(a->root);
        size_t pos = 0;
        while (pos < rel.size()) {
            size_t next = rel.find(L'\\', pos);
            if (next == std::wstring::npos)
                next = rel.size();
            std::wstring parent = pos ? a->root + L"\\" + lower.substr(0, pos - 1) : a->root;
            std::wstring key = lower.substr(pos, next - pos);
            Dir& p = ArchiveDir(parent);
            if (!p.entries.count(key)) {
                Entry e;
                e.name = rel.substr(pos, next - pos);
                e.attributes = FILE_ATTRIBUTE_DIRECTORY;
                e.created = e.accessed = e.written = a->written;
                p.entries.emplace(key, e);
            }
            ArchiveDir(a->root + L"\\" + lower.substr(0, next));
            pos = next + 1;
        }
    }

    // Reads the central directory. False (with the reason logged) leaves the folder in use.
    static bool LoadArchive(Archive* a) {
        a->file = real_CreateFileW(a->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (a->file == INVALID_HANDLE_VALUE) {
            dl::log("Archive %s: can't open it (error %lu)", dl::narrow(a->path).c_str(), GetLastError());
            return false;
        }
        LARGE_INTEGER size;
        if (!real_GetFileSizeEx(a->file, &size) || size.QuadPart < 22) {
            dl::log("Archive %s: too small to be a zip", dl::narrow(a->path).c_str());
            return false;
        }
        a->file_size = (uint64_t)size.QuadPart;
        real_GetFileTime(a->file, nullptr, nullptr, &a->written);

        std::vector<zipidx::Member> members;
        zipidx::Result r = zipidx::read_central_directory(a->file_size,
            [a](uint64_t offset, void* buffer, uint32_t size) { return ReadAt(a->file, offset, buffer, size); }, members);
        const std::string path_s = dl::narrow(a->path);
        const char* path8 = path_s.c_str();
        switch (r.error) {
        case zipidx::Error::None: break;
        case zipidx::Error::TooSmall: dl::log("Archive %s: too small to be a zip", path8); return false;
        case zipidx::Error::ReadError: dl::log("Archive %s: read error", path8); return false;
        case zipidx::Error::NotZip: dl::log("Archive %s: not a zip (no end of central directory)", path8); return false;
        case zipidx::Error::BrokenZip64: dl::log("Archive %s: broken zip64 end record", path8); return false;
        case zipidx::Error::OutOfRange: dl::log("Archive %s: central directory out of range", path8); return false;
        case zipidx::Error::DirectoryRead: dl::log("Archive %s: read error in the central directory", path8); return false;
        case zipidx::Error::BrokenDirectory: dl::log("Archive %s: broken central directory", path8); return false;
        case zipidx::Error::NotStored:
            dl::log("Archive %s: %s is %s; DivaLinux reads only stored (uncompressed) zips, so the folder is used "
                "instead. Repack with tools/divalinux_pack.py, or 7-Zip with compression level 0 (Store).",
                path8, r.member.c_str(), r.encrypted ? "encrypted" : "compressed");
            return false;
        }

        struct Raw { std::wstring name; uint64_t header, size; uint16_t date, time; bool dir; };
        std::vector<Raw> raw;
        raw.reserve(members.size());
        for (const zipidx::Member& m : members) {
            int wn = MultiByteToWideChar(CP_UTF8, 0, m.name.c_str(), (int)m.name.size(), nullptr, 0);
            std::wstring name(wn, L'\0');
            if (wn)
                MultiByteToWideChar(CP_UTF8, 0, m.name.c_str(), (int)m.name.size(), &name[0], wn);
            raw.push_back({ name, m.header_offset, m.size, m.dos_date, m.dos_time, m.dir });
        }

        // Archives carry their place in the game folder, so "extract here" in the game folder
        // rebuilds it: rom.zip holds rom\..., mdata\MZKO.zip holds mdata\MZKO\.... Zips of just
        // the folder (MZKO\...) or of its contents are read too. Strip whichever prefix every
        // entry shares, dropping the entries for the prefix's own folders.
        {
            std::vector<std::wstring> prefixes;
            std::wstring top = a->root.substr(g_base.size()); // "rom" or "mdata\mzko"
            prefixes.push_back(top + L"\\");
            if (top.compare(0, 6, L"mdata\\") == 0)
                prefixes.push_back(top.substr(6) + L"\\");
            for (const std::wstring& prefix : prefixes) {
                bool all = !raw.empty(), any = false;
                for (const Raw& r : raw) {
                    std::wstring n = r.name + L"\\";
                    if (n.size() > prefix.size() && !_wcsnicmp(n.c_str(), prefix.c_str(), prefix.size()))
                        any = true;
                    else if (!(r.dir && n.size() <= prefix.size() && !_wcsnicmp(prefix.c_str(), n.c_str(), n.size()))) {
                        all = false; // outside the prefix (and not one of its own folders)
                        break;
                    }
                }
                if (!all || !any)
                    continue;
                std::vector<Raw> kept;
                for (Raw& r : raw)
                    if (r.name.size() >= prefix.size()) {
                        r.name.erase(0, prefix.size());
                        if (!r.name.empty())
                            kept.push_back(r);
                    }
                raw.swap(kept);
                break;
            }
        }

        ArchiveDir(a->root);
        uint32_t index = 0;
        for (const Raw& r : raw) {
            if (r.dir) {
                AddArchiveDirs(a, r.name);
                continue;
            }
            size_t slash = r.name.find_last_of(L'\\');
            std::wstring parent_rel = slash == std::wstring::npos ? std::wstring() : r.name.substr(0, slash);
            std::wstring file = slash == std::wstring::npos ? r.name : r.name.substr(slash + 1);
            if (!parent_rel.empty())
                AddArchiveDirs(a, parent_rel);
            std::wstring parent = a->root;
            if (!parent_rel.empty()) {
                std::wstring l = parent_rel;
                dl::lower(l);
                parent += L"\\" + l;
            }
            a->items.emplace_back(a, r.header, r.size, index++);
            Entry e;
            e.name = file;
            e.attributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
            FILETIME local;
            if (DosDateTimeToFileTime(r.date, r.time, &local))
                LocalFileTimeToFileTime(&local, &e.written);
            e.created = e.accessed = e.written;
            e.size = r.size;
            e.item = &a->items.back();
            std::wstring key = file;
            dl::lower(key);
            ArchiveDir(parent).entries[key] = e;
        }
        dl::log("Archive %s: %zu files, replaces the %s folder", dl::narrow(a->path).c_str(), a->items.size(),
            dl::narrow(a->root.substr(g_base.size())).c_str());
        return true;
    }

    // Finds rom.zip and mdata\*.zip (called once, at install).
    void FindArchives() {
        auto add = [](const std::wstring& zip, const std::wstring& root, const std::wstring& name) {
            Archive* a = new Archive();
            a->path = zip;
            a->root = root;
            a->root_name = name;
            a->ok = LoadArchive(a);
            if (!a->ok) {
                // Forget anything a failed load added; the folder is listed as usual
                for (auto it = g_dirs.begin(); it != g_dirs.end();)
                    if (it->first == root || !it->first.compare(0, root.size() + 1, root + L"\\"))
                        it = g_dirs.erase(it);
                    else
                        ++it;
            }
            g_archive_list.push_back(a);
        };

        WIN32_FIND_DATAW fd;
        HANDLE h = real_FindFirstFileExW((g_base_real + L"\\*.zip").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!_wcsicmp(fd.cFileName, L"rom.zip") && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    add(g_base_real + L"\\" + fd.cFileName, g_base + L"rom", L"rom");
            } while (real_FindNextFileW(h, &fd));
            real_FindClose(h);
        }

        // mdata: find the folder's real name first (case)
        std::wstring mdata_real;
        h = real_FindFirstFileExW((g_base_real + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!_wcsicmp(fd.cFileName, L"mdata") && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    mdata_real = g_base_real + L"\\" + fd.cFileName;
            } while (real_FindNextFileW(h, &fd));
            real_FindClose(h);
        }
        if (mdata_real.empty())
            return;
        h = real_FindFirstFileExW((mdata_real + L"\\*.zip").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                std::wstring file = fd.cFileName;
                if (file.size() <= 4 || _wcsicmp(file.c_str() + file.size() - 4, L".zip"))
                    continue;
                std::wstring name = file.substr(0, file.size() - 4), lower = name;
                dl::lower(lower);
                add(mdata_real + L"\\" + file, g_base + L"mdata\\" + lower, name);
            } while (real_FindNextFileW(h, &fd));
            real_FindClose(h);
        }
    }
}
