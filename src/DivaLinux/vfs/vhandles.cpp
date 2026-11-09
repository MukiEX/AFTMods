// Handles the file layer gives out: find handles from a reserved address range, and archive
// members (duplicates of the archive's handle, with their own position and size).

#include "vfs_internal.hpp"
#include "zip_index.hpp"

namespace vfs {
    // ------------------------------------------------------------ find handles

    // Handles come from a reserved, never-committed range, so they can't collide with
    // anything Wine hands out.
    uint8_t* g_find_base = nullptr;
    static FindState* volatile g_find[FIND_SLOTS];
    static volatile LONG g_find_next = 0;

    HANDLE NewFind(FindState* s) {
        if (!g_find_base)
            return nullptr;
        for (size_t tries = 0; tries < FIND_SLOTS; tries++) {
            size_t i = (size_t)InterlockedIncrement(&g_find_next) % FIND_SLOTS;
            if (!InterlockedCompareExchangePointer((PVOID volatile*)&g_find[i], s, nullptr))
                return (HANDLE)(g_find_base + i * 16);
        }
        return nullptr;
    }

    FindState* GetFind(HANDLE h, size_t* index) {
        uint8_t* p = (uint8_t*)h;
        if (!g_find_base || p < g_find_base || p >= g_find_base + FIND_SLOTS * 16 || (p - g_find_base) % 16)
            return nullptr;
        *index = (size_t)(p - g_find_base) / 16;
        return g_find[*index];
    }

    bool CloseFind(HANDLE h) {
        size_t i;
        FindState* s = GetFind(h, &i);
        if (!s)
            return false;
        if (InterlockedCompareExchangePointer((PVOID volatile*)&g_find[i], nullptr, s) == s)
            delete s;
        return true;
    }

    void FillW(WIN32_FIND_DATAW* fd, const Entry& e) {
        memset(fd, 0, sizeof(*fd));
        fd->dwFileAttributes = e.attributes;
        fd->ftCreationTime = e.created;
        fd->ftLastAccessTime = e.accessed;
        fd->ftLastWriteTime = e.written;
        fd->nFileSizeHigh = (DWORD)(e.size >> 32);
        fd->nFileSizeLow = (DWORD)e.size;
        wcsncpy(fd->cFileName, e.name.c_str(), MAX_PATH - 1);
    }

    void FillA(WIN32_FIND_DATAA* fd, const Entry& e) {
        WIN32_FIND_DATAW w;
        FillW(&w, e);
        memset(fd, 0, sizeof(*fd));
        fd->dwFileAttributes = w.dwFileAttributes;
        fd->ftCreationTime = w.ftCreationTime;
        fd->ftLastAccessTime = w.ftLastAccessTime;
        fd->ftLastWriteTime = w.ftLastWriteTime;
        fd->nFileSizeHigh = w.nFileSizeHigh;
        fd->nFileSizeLow = w.nFileSizeLow;
        WideCharToMultiByte(CP_ACP, 0, w.cFileName, -1, fd->cFileName, MAX_PATH, nullptr, nullptr);
    }

    // FindFirstFileEx for a single name or a search. Returns PASS when Wine should do it.
    // answer_found: the caller's FindClose and FindNextFile* go through our hooks.
    Answer FindFirst(const wchar_t* path, FINDEX_INFO_LEVELS level, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags,
        bool answer_found, FindState** state) {
        if (filter || (op != FindExSearchNameMatch && op != FindExSearchLimitToDirectories)
            || (flags & FIND_FIRST_EX_CASE_SENSITIVE) || (level != FindExInfoStandard && level != FindExInfoBasic))
            return PASS;
        std::wstring dir, pattern;
        if (ResolveSearch(path, dir, pattern)) {
            if (!answer_found || (!g_cache && !ArchiveFor(dir) && !IsArchiveParent(dir)))
                return PASS;
            AcquireSRWLockExclusive(&g_lock);
            const Dir& d = GetDirLocked(dir);
            Answer a = !Answerable(dir, d) || d.unknown ? PASS : !d.exists ? MISSING_PATH : FOUND;
            FindState* s = nullptr;
            if (a == FOUND) {
                s = new FindState();
                // "." and ".." come first, as Windows lists them
                for (const wchar_t* dot : { L".", L".." })
                    if (Match(pattern.c_str(), dot)) {
                        Entry e;
                        e.name = dot;
                        e.attributes = FILE_ATTRIBUTE_DIRECTORY;
                        s->entries.push_back(e);
                    }
                std::vector<std::pair<std::wstring, const Entry*>> sorted;
                for (const auto& kv : d.entries)
                    if (Match(pattern.c_str(), kv.first.c_str())
                        && (op != FindExSearchLimitToDirectories || (kv.second.attributes & FILE_ATTRIBUTE_DIRECTORY)))
                        sorted.push_back({ kv.first, &kv.second });
                std::sort(sorted.begin(), sorted.end());
                for (const auto& kv : sorted)
                    s->entries.push_back(*kv.second);
                if (s->entries.empty()) {
                    delete s;
                    s = nullptr;
                    a = MISSING_FILE;
                }
            }
            ReleaseSRWLockExclusive(&g_lock);
            if (a == FOUND)
                InterlockedIncrement(&g_enumerated);
            *state = s;
            return a;
        }
        Entry e;
        Answer a = Lookup(path, e);
        if (a == FOUND) {
            if (!answer_found)
                return PASS;
            FindState* s = new FindState();
            s->entries.push_back(e);
            *state = s;
        }
        return a;
    }

    HANDLE FinishFind(Answer a, FindState* s, HANDLE (*fill_and_pass)(void*), void* ctx) {
        switch (a) {
        case MISSING_FILE:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        case MISSING_PATH:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        case FOUND:
            if (HANDLE h = NewFind(s)) {
                InterlockedIncrement(&g_hits);
                SetLastError(ERROR_SUCCESS);
                return h;
            }
            delete s;
            break;
        default:
            break;
        }
        InterlockedIncrement(&g_passed);
        return fill_and_pass(ctx);
    }

    // ------------------------------------------------------------ archive member handles

    static SRWLOCK g_vfile_lock = SRWLOCK_INIT;
    static std::unordered_map<HANDLE, VFile> g_vfiles;
    static std::atomic<long> g_vfile_count{ 0 }; // quick check before taking the lock

    bool GetVFile(HANDLE h, VFile* out) {
        if (!g_vfile_count)
            return false;
        AcquireSRWLockShared(&g_vfile_lock);
        auto it = g_vfiles.find(h);
        bool found = it != g_vfiles.end();
        if (found && out)
            *out = it->second;
        ReleaseSRWLockShared(&g_vfile_lock);
        return found;
    }

    void ForgetVFile(HANDLE h) {
        if (!g_vfile_count)
            return;
        AcquireSRWLockExclusive(&g_vfile_lock);
        if (g_vfiles.erase(h))
            g_vfile_count--;
        ReleaseSRWLockExclusive(&g_vfile_lock);
    }

    void SetPos(HANDLE h, uint64_t pos) {
        AcquireSRWLockExclusive(&g_vfile_lock);
        auto it = g_vfiles.find(h);
        if (it != g_vfiles.end())
            it->second.pos = pos;
        ReleaseSRWLockExclusive(&g_vfile_lock);
    }

    bool IsReadOnlyAccess(DWORD access) {
        const DWORD read = GENERIC_READ | GENERIC_EXECUTE | FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA
            | FILE_EXECUTE | READ_CONTROL | SYNCHRONIZE;
        return !(access & ~read);
    }

    HANDLE OpenMember(const Entry& e) {
        ZipItem* item = e.item;
        Archive* a = item->archive;
        int64_t data = item->data_offset.load();
        if (data < 0) {
            data = zipidx::data_offset([a](uint64_t offset, void* buffer, uint32_t size) { return ReadAt(a->file, offset, buffer, size); },
                item->header_offset, item->size, a->file_size);
            if (data < 0) {
                dl::log("Archive %s: bad local header for %s", dl::narrow(a->path).c_str(), dl::narrow(e.name).c_str());
                SetLastError(ERROR_FILE_CORRUPT);
                return INVALID_HANDLE_VALUE;
            }
            item->data_offset.store(data);
        }
        HANDLE dup = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), a->file, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return INVALID_HANDLE_VALUE;
        VFile v;
        v.item = item;
        v.data = (uint64_t)data;
        v.size = item->size;
        v.written = e.written;
        v.attributes = e.attributes;
        AcquireSRWLockExclusive(&g_vfile_lock);
        g_vfiles[dup] = v;
        g_vfile_count++;
        ReleaseSRWLockExclusive(&g_vfile_lock);
        InterlockedIncrement(&g_opened);
        SetLastError(ERROR_SUCCESS);
        return dup;
    }

    // CreateFile for a path: PASS to Wine, or an answer. serve_members: the caller's ReadFile
    // and CloseHandle go through our hooks.
    HANDLE CreateFileCommon(const wchar_t* path, DWORD access, DWORD disposition, DWORD flags, bool serve_members, bool* pass) {
        *pass = true;
        Entry e;
        Answer a = Lookup(path, e);
        bool creates = disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS;
        if (a == MISSING_FILE || a == MISSING_PATH) {
            std::wstring dir, name;
            bool in_archive = Resolve(path, dir, name) && ArchiveFor(dir);
            if (creates && !in_archive)
                return INVALID_HANDLE_VALUE; // let Wine create it (and drop the listing)
            *pass = false;
            InterlockedIncrement(&g_misses);
            SetLastError(in_archive && creates ? ERROR_ACCESS_DENIED : a == MISSING_FILE ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        if (a != FOUND || !e.item)
            return INVALID_HANDLE_VALUE; // real files, folders: Wine opens them
        *pass = false;
        if (!serve_members) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        if (!IsReadOnlyAccess(access) || disposition == CREATE_NEW || disposition == CREATE_ALWAYS
            || disposition == TRUNCATE_EXISTING || (flags & FILE_FLAG_DELETE_ON_CLOSE)) {
            SetLastError(ERROR_ACCESS_DENIED); // archives are read-only
            return INVALID_HANDLE_VALUE;
        }
        InterlockedIncrement(&g_hits);
        return OpenMember(e);
    }
}
