// Folder listings: each folder is listed once, on first use, and lookups in it are answered
// from memory with what Windows would say. Archive folders come from archives.cpp.

#include "vfs_internal.hpp"

namespace vfs {
    void List(Dir& d, const std::wstring& key) {
        double t0 = dl::now_ms();
        WIN32_FIND_DATAW fd;
        HANDLE h = real_FindFirstFileExW((d.real_path + L"\\*").c_str(), FindExInfoBasic, &fd,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            d.exists = e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES;
            d.unknown = !d.exists;
        }
        else {
            d.exists = true;
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                    continue;
                Entry e;
                e.name = fd.cFileName;
                e.attributes = fd.dwFileAttributes;
                e.created = fd.ftCreationTime;
                e.accessed = fd.ftLastAccessTime;
                e.written = fd.ftLastWriteTime;
                e.size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
                std::wstring k = e.name;
                dl::lower(k);
                d.entries.emplace(std::move(k), std::move(e));
            } while (real_FindNextFileW(h, &fd));
            real_FindClose(h);
            InterlockedIncrement(&g_listed);
        }
        // Archives show up as the folders they stand for
        if (d.exists)
            for (Archive* a : g_archive_list) {
                if (!a->ok)
                    continue;
                size_t slash = a->root.find_last_of(L'\\');
                if (a->root.compare(0, slash, key) || slash != key.size())
                    continue;
                Entry e;
                e.name = a->root_name;
                e.attributes = FILE_ATTRIBUTE_DIRECTORY;
                e.created = e.accessed = e.written = a->written;
                d.entries[a->root.substr(slash + 1)] = e;
            }
        g_list_ms += dl::now_ms() - t0;
    }

    // The listing of folder `dir` (lower case), made on first use. Caller holds g_lock
    // exclusively. Parents are listed first, so a folder that doesn't exist costs nothing.
    Dir& GetDirLocked(const std::wstring& dir) {
        auto it = g_dirs.find(dir);
        if (it != g_dirs.end())
            return it->second;
        Dir d;
        if (ArchiveFor(dir)) {
            d.archived = true; // inside an archive, and not one of its folders: missing
        }
        else if (dir == g_base_dir) {
            d.real_path = g_base_real;
            List(d, dir);
        }
        else {
            size_t slash = dir.find_last_of(L'\\');
            Dir& parent = GetDirLocked(dir.substr(0, slash));
            auto e = parent.exists ? parent.entries.find(dir.substr(slash + 1)) : parent.entries.end();
            if (parent.unknown)
                d.unknown = true;
            else if (e != parent.entries.end() && (e->second.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                d.real_path = parent.real_path + L"\\" + e->second.name;
                List(d, dir);
            }
        }
        return g_dirs.emplace(dir, std::move(d)).first->second;
    }

    Answer Classify(const Dir& d, const std::wstring& name, Entry* out) {
        if (d.unknown)
            return PASS;
        if (!d.exists)
            return MISSING_PATH;
        auto e = d.entries.find(name);
        if (e == d.entries.end())
            return MISSING_FILE;
        if (out)
            *out = e->second;
        return FOUND;
    }

    // The cache answers real folders only when lookup_cache is on; archive folders, and the
    // folders that hold archives, always.
    bool Answerable(const std::wstring& key, const Dir& d) { return g_cache || d.archived || IsArchiveParent(key); }

    Answer Lookup(const wchar_t* path, Entry& out) {
        std::wstring dir, name;
        if (!Resolve(path, dir, name))
            return PASS;
        if (!g_cache && !ArchiveFor(dir) && !IsArchiveParent(dir))
            return PASS;
        AcquireSRWLockShared(&g_lock);
        auto it = g_dirs.find(dir);
        bool cached = it != g_dirs.end();
        Answer a = PASS;
        if (cached)
            a = Answerable(dir, it->second) ? Classify(it->second, name, &out) : PASS;
        ReleaseSRWLockShared(&g_lock);
        if (cached)
            return a;
        AcquireSRWLockExclusive(&g_lock);
        const Dir& d = GetDirLocked(dir);
        a = Answerable(dir, d) ? Classify(d, name, &out) : PASS;
        ReleaseSRWLockExclusive(&g_lock);
        return a;
    }

    void Drop(const wchar_t* path) {
        std::wstring dir, name;
        if (!Resolve(path, dir, name) || ArchiveFor(dir))
            return;
        AcquireSRWLockExclusive(&g_lock);
        if (g_dirs.erase(dir))
            InterlockedIncrement(&g_dropped);
        if (!ArchiveFor(dir + L"\\" + name))
            g_dirs.erase(dir + L"\\" + name);
        ReleaseSRWLockExclusive(&g_lock);
    }
}
