// The kernel32 replacements the file layer puts into the game's and plugins' import tables.

#include "vfs_internal.hpp"

namespace vfs {
    // ------------------------------------------------------------ lookups

    struct FindCtxW { LPCWSTR path; FINDEX_INFO_LEVELS level; LPVOID data; FINDEX_SEARCH_OPS op; LPVOID filter; DWORD flags; };
    struct FindCtxA { LPCSTR path; FINDEX_INFO_LEVELS level; LPVOID data; FINDEX_SEARCH_OPS op; LPVOID filter; DWORD flags; };

    HANDLE PassFindW(void* c) {
        FindCtxW* x = (FindCtxW*)c;
        return real_FindFirstFileExW(x->path, x->level, x->data, x->op, x->filter, x->flags);
    }
    HANDLE PassFindA(void* c) {
        FindCtxA* x = (FindCtxA*)c;
        return real_FindFirstFileExA(x->path, x->level, x->data, x->op, x->filter, x->flags);
    }

    HANDLE FindFirstW(bool full, LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
        FindCtxW ctx = { path, level, data, op, filter, flags };
        FindState* s = nullptr;
        Answer a = data ? FindFirst(path, level, op, filter, flags, full, &s) : PASS;
        if (a == FOUND)
            FillW((WIN32_FIND_DATAW*)data, s->entries[s->next++]);
        return FinishFind(a, s, PassFindW, &ctx);
    }

    HANDLE FindFirstA(bool full, LPCSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags) {
        FindCtxA ctx = { path, level, data, op, filter, flags };
        FindState* s = nullptr;
        Answer a = data && path ? FindFirst(dl::widen_acp(path).c_str(), level, op, filter, flags, full, &s) : PASS;
        if (a == FOUND)
            FillA((WIN32_FIND_DATAA*)data, s->entries[s->next++]);
        return FinishFind(a, s, PassFindA, &ctx);
    }

    HANDLE WINAPI Hook_FindFirstFileExW(LPCWSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
        return FindFirstW(true, p, l, d, o, f, fl);
    }
    HANDLE WINAPI Hook_FindFirstFileExW_Lookups(LPCWSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
        return FindFirstW(false, p, l, d, o, f, fl);
    }
    HANDLE WINAPI Hook_FindFirstFileExA(LPCSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
        return FindFirstA(true, p, l, d, o, f, fl);
    }
    HANDLE WINAPI Hook_FindFirstFileExA_Lookups(LPCSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
        return FindFirstA(false, p, l, d, o, f, fl);
    }

    BOOL WINAPI Hook_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data) {
        size_t i;
        if (FindState* s = GetFind(h, &i)) {
            if (s->next >= s->entries.size()) {
                SetLastError(ERROR_NO_MORE_FILES);
                return FALSE;
            }
            FillW(data, s->entries[s->next++]);
            return TRUE;
        }
        return real_FindNextFileW(h, data);
    }

    BOOL WINAPI Hook_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data) {
        size_t i;
        if (FindState* s = GetFind(h, &i)) {
            if (s->next >= s->entries.size()) {
                SetLastError(ERROR_NO_MORE_FILES);
                return FALSE;
            }
            FillA(data, s->entries[s->next++]);
            return TRUE;
        }
        return real_FindNextFileA(h, data);
    }

    BOOL WINAPI Hook_FindClose(HANDLE h) {
        if (CloseFind(h))
            return TRUE;
        return real_FindClose(h);
    }

    DWORD AttributesCommon(const wchar_t* path, bool* pass) {
        Entry e;
        *pass = false;
        switch (Lookup(path, e)) {
        case FOUND:
            InterlockedIncrement(&g_hits);
            return e.attributes;
        case MISSING_FILE:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        case MISSING_PATH:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_PATH_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        default:
            *pass = true;
            InterlockedIncrement(&g_passed);
            return 0;
        }
    }

    DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR path) {
        bool pass;
        DWORD r = AttributesCommon(path, &pass);
        return pass ? real_GetFileAttributesW(path) : r;
    }

    DWORD WINAPI Hook_GetFileAttributesA(LPCSTR path) {
        bool pass;
        DWORD r = path ? AttributesCommon(dl::widen_acp(path).c_str(), &pass) : (pass = true, 0);
        return pass ? real_GetFileAttributesA(path) : r;
    }

    // 1 answered, 0 failed (error set), -1 pass to Wine
    int AttributesExCommon(const wchar_t* path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
        if (level != GetFileExInfoStandard || !info)
            return -1;
        Entry e;
        switch (Lookup(path, e)) {
        case FOUND: {
            InterlockedIncrement(&g_hits);
            WIN32_FILE_ATTRIBUTE_DATA* d = (WIN32_FILE_ATTRIBUTE_DATA*)info;
            d->dwFileAttributes = e.attributes;
            d->ftCreationTime = e.created;
            d->ftLastAccessTime = e.accessed;
            d->ftLastWriteTime = e.written;
            d->nFileSizeHigh = (DWORD)(e.size >> 32);
            d->nFileSizeLow = (DWORD)e.size;
            return 1;
        }
        case MISSING_FILE:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return 0;
        case MISSING_PATH:
            InterlockedIncrement(&g_misses);
            SetLastError(ERROR_PATH_NOT_FOUND);
            return 0;
        default:
            InterlockedIncrement(&g_passed);
            return -1;
        }
    }

    BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
        int r = AttributesExCommon(path, level, info);
        return r < 0 ? real_GetFileAttributesExW(path, level, info) : r;
    }

    BOOL WINAPI Hook_GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
        int r = path ? AttributesExCommon(dl::widen_acp(path).c_str(), level, info) : -1;
        return r < 0 ? real_GetFileAttributesExA(path, level, info) : r;
    }

    // ------------------------------------------------------------ files

    HANDLE CreateW(bool full, LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ) {
        bool pass;
        HANDLE h = CreateFileCommon(path, access, disposition, flags, full, &pass);
        if (!pass)
            return h;
        h = real_CreateFileW(path, access, share, sa, disposition, flags, templ);
        if (h != INVALID_HANDLE_VALUE && disposition != OPEN_EXISTING && disposition != TRUNCATE_EXISTING) {
            DWORD e = GetLastError();
            Drop(path);
            SetLastError(e);
        }
        return h;
    }

    HANDLE CreateA(bool full, LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ) {
        bool pass = true;
        HANDLE h = INVALID_HANDLE_VALUE;
        std::wstring w = dl::widen_acp(path);
        if (path)
            h = CreateFileCommon(w.c_str(), access, disposition, flags, full, &pass);
        if (!pass)
            return h;
        h = real_CreateFileA(path, access, share, sa, disposition, flags, templ);
        if (path && h != INVALID_HANDLE_VALUE && disposition != OPEN_EXISTING && disposition != TRUNCATE_EXISTING) {
            DWORD e = GetLastError();
            Drop(w.c_str());
            SetLastError(e);
        }
        return h;
    }

    HANDLE WINAPI Hook_CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
        return CreateW(true, p, a, s, sa, d, f, t);
    }
    HANDLE WINAPI Hook_CreateFileW_Lookups(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
        return CreateW(false, p, a, s, sa, d, f, t);
    }
    HANDLE WINAPI Hook_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
        return CreateA(true, p, a, s, sa, d, f, t);
    }
    HANDLE WINAPI Hook_CreateFileA_Lookups(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) {
        return CreateA(false, p, a, s, sa, d, f, t);
    }

    BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buffer, DWORD n, LPDWORD read, LPOVERLAPPED ov) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_ReadFile(h, buffer, n, read, ov);
        uint64_t offset = ov ? ((uint64_t)ov->OffsetHigh << 32 | ov->Offset) : v.pos;
        DWORD count = offset >= v.size ? 0 : (DWORD)std::min<uint64_t>(n, v.size - offset);
        DWORD got = 0;
        BOOL ok = TRUE;
        if (count) {
            OVERLAPPED at = {};
            uint64_t where = v.data + offset;
            at.Offset = (DWORD)where;
            at.OffsetHigh = (DWORD)(where >> 32);
            ok = real_ReadFile(v.item->archive->file, buffer, count, &got, &at);
        }
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (ok && !ov)
            SetPos(h, offset + got);
        if (read)
            *read = got;
        if (ov) {
            ov->Internal = ok ? 0 : (ULONG_PTR)err; // completes at once
            ov->InternalHigh = got;
            if (ov->hEvent)
                SetEvent(ov->hEvent);
        }
        InterlockedExchangeAdd64(&g_bytes, got);
        SetLastError(err);
        return ok;
    }

    // New position for a seek, or false (error set) if it would be negative.
    bool Seek(HANDLE h, const VFile& v, int64_t distance, DWORD method, uint64_t* out) {
        int64_t base = method == FILE_BEGIN ? 0 : method == FILE_CURRENT ? (int64_t)v.pos : (int64_t)v.size;
        if (method > FILE_END) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        int64_t pos = base + distance;
        if (pos < 0) {
            SetLastError(ERROR_NEGATIVE_SEEK);
            return false;
        }
        SetPos(h, (uint64_t)pos);
        *out = (uint64_t)pos;
        SetLastError(ERROR_SUCCESS);
        return true;
    }

    DWORD WINAPI Hook_SetFilePointer(HANDLE h, LONG low, PLONG high, DWORD method) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_SetFilePointer(h, low, high, method);
        int64_t distance = high ? ((int64_t)*high << 32 | (uint32_t)low) : (int64_t)low;
        uint64_t pos;
        if (!Seek(h, v, distance, method, &pos))
            return INVALID_SET_FILE_POINTER;
        if (high)
            *high = (LONG)(pos >> 32);
        return (DWORD)pos;
    }

    BOOL WINAPI Hook_SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, PLARGE_INTEGER out, DWORD method) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_SetFilePointerEx(h, distance, out, method);
        uint64_t pos;
        if (!Seek(h, v, distance.QuadPart, method, &pos))
            return FALSE;
        if (out)
            out->QuadPart = (LONGLONG)pos;
        return TRUE;
    }

    DWORD WINAPI Hook_GetFileSize(HANDLE h, LPDWORD high) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_GetFileSize(h, high);
        if (high)
            *high = (DWORD)(v.size >> 32);
        SetLastError(ERROR_SUCCESS);
        return (DWORD)v.size;
    }

    BOOL WINAPI Hook_GetFileSizeEx(HANDLE h, PLARGE_INTEGER size) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_GetFileSizeEx(h, size);
        size->QuadPart = (LONGLONG)v.size;
        return TRUE;
    }

    BOOL WINAPI Hook_GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_GetFileInformationByHandle(h, info);
        if (!real_GetFileInformationByHandle(h, info)) // the archive's volume serial etc.
            memset(info, 0, sizeof(*info));
        info->dwFileAttributes = v.attributes;
        info->ftCreationTime = info->ftLastAccessTime = info->ftLastWriteTime = v.written;
        info->nFileSizeHigh = (DWORD)(v.size >> 32);
        info->nFileSizeLow = (DWORD)v.size;
        info->nNumberOfLinks = 1;
        // A file index of its own, so code that compares indices sees different files
        info->nFileIndexHigh ^= 0x80000000u | (DWORD)(uintptr_t)v.item->archive;
        info->nFileIndexLow ^= v.item->index + 1;
        return TRUE;
    }

    BOOL WINAPI Hook_GetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID buffer, DWORD size) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_GetFileInformationByHandleEx(h, cls, buffer, size);
        if (cls == FileStandardInfo && size >= sizeof(FILE_STANDARD_INFO)) {
            FILE_STANDARD_INFO* s = (FILE_STANDARD_INFO*)buffer;
            s->AllocationSize.QuadPart = (LONGLONG)v.size;
            s->EndOfFile.QuadPart = (LONGLONG)v.size;
            s->NumberOfLinks = 1;
            s->DeletePending = FALSE;
            s->Directory = FALSE;
            return TRUE;
        }
        if (cls == FileBasicInfo && size >= sizeof(FILE_BASIC_INFO)) {
            FILE_BASIC_INFO* b = (FILE_BASIC_INFO*)buffer;
            b->CreationTime.LowPart = b->LastAccessTime.LowPart = b->LastWriteTime.LowPart = b->ChangeTime.LowPart = v.written.dwLowDateTime;
            b->CreationTime.HighPart = b->LastAccessTime.HighPart = b->LastWriteTime.HighPart = b->ChangeTime.HighPart = (LONG)v.written.dwHighDateTime;
            b->FileAttributes = v.attributes;
            return TRUE;
        }
        return real_GetFileInformationByHandleEx(h, cls, buffer, size);
    }

    BOOL WINAPI Hook_GetFileTime(HANDLE h, LPFILETIME created, LPFILETIME accessed, LPFILETIME written) {
        VFile v;
        if (!GetVFile(h, &v))
            return real_GetFileTime(h, created, accessed, written);
        if (created) *created = v.written;
        if (accessed) *accessed = v.written;
        if (written) *written = v.written;
        return TRUE;
    }

    BOOL WINAPI Hook_CloseHandle(HANDLE h) {
        ForgetVFile(h);
        return real_CloseHandle(h);
    }

    // Mapping a member: an unnamed memory section holding a copy of it (mapping the
    // archive handle would show the wrong bytes).
    HANDLE MapMember(const VFile& v, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD max_high, DWORD max_low, bool* handled) {
        *handled = true;
        uint64_t size = ((uint64_t)max_high << 32 | max_low) ? ((uint64_t)max_high << 32 | max_low) : v.size;
        if (!size || (protect & 0xFF) == PAGE_READWRITE || (protect & 0xFF) == PAGE_EXECUTE_READWRITE) {
            SetLastError(!size ? ERROR_FILE_INVALID : ERROR_ACCESS_DENIED);
            return nullptr;
        }
        HANDLE m = real_CreateFileMappingW(INVALID_HANDLE_VALUE, sa, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD)size, nullptr);
        if (!m)
            return nullptr;
        if (void* view = MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, (SIZE_T)size)) {
            uint64_t done = 0, want = std::min(size, v.size);
            while (done < want) {
                DWORD chunk = (DWORD)std::min<uint64_t>(want - done, 64u << 20);
                if (!ReadAt(v.item->archive->file, v.data + done, (uint8_t*)view + done, chunk))
                    break;
                done += chunk;
            }
            UnmapViewOfFile(view);
            if (done == want)
                return m;
        }
        DWORD e = GetLastError();
        real_CloseHandle(m);
        SetLastError(e ? e : ERROR_READ_FAULT);
        return nullptr;
    }

    HANDLE WINAPI Hook_CreateFileMappingW(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name) {
        VFile v;
        if (h && h != INVALID_HANDLE_VALUE && !name && GetVFile(h, &v)) {
            bool handled;
            return MapMember(v, sa, protect, hi, lo, &handled);
        }
        return real_CreateFileMappingW(h, sa, protect, hi, lo, name);
    }

    HANDLE WINAPI Hook_CreateFileMappingA(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCSTR name) {
        VFile v;
        if (h && h != INVALID_HANDLE_VALUE && !name && GetVFile(h, &v)) {
            bool handled;
            return MapMember(v, sa, protect, hi, lo, &handled);
        }
        return real_CreateFileMappingA(h, sa, protect, hi, lo, name);
    }
}
