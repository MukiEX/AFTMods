// Tests the file layer (../vfs) against a Wine-like fake kernel over Linux folders.
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <locale.h>
#include <algorithm>

static std::string g_root;             // Linux folder standing in for Z:\Game
static thread_local DWORD g_err;
DWORD GetLastError() { return g_err; }
void SetLastError(DWORD e) { g_err = e; }
static std::atomic<int> g_real_lookups{0};
static bool g_quiet = false;
extern "C" void divavk_log_text(const char* t) { if (!g_quiet) printf("    log: %s\n", t); }
DWORD GetModuleFileNameW(HMODULE, wchar_t* b, DWORD) { wcscpy(b, L"Z:\\Game\\diva.exe"); return (DWORD)wcslen(b); }
HMODULE GetModuleHandleW(const wchar_t*) { return (HMODULE)1; }
static int g_cache_setting = 1;
UINT GetPrivateProfileIntW(const wchar_t*, const wchar_t* key, int d, const wchar_t*) { return !wcscmp(key, L"lookup_cache") ? g_cache_setting : d; }
int WideCharToMultiByte(UINT, DWORD, const wchar_t* w, int wn, char* o, int n, void*, void*) {
    std::wstring s = wn < 0 ? std::wstring(w) : std::wstring(w, wn);
    std::string r; for (wchar_t c : s) r += (char)c;
    if (wn < 0) r += '\0';
    if (!o || !n) return (int)r.size();
    memcpy(o, r.data(), std::min((size_t)n, r.size())); return (int)std::min((size_t)n, r.size());
}
int MultiByteToWideChar(UINT, DWORD, const char* s, int sn, wchar_t* o, int n) {
    std::string a = sn < 0 ? std::string(s) : std::string(s, sn);
    std::wstring r; for (char c : a) r += (wchar_t)(unsigned char)c;
    if (sn < 0) r += L'\0';
    if (!o || !n) return (int)r.size();
    memcpy(o, r.data(), std::min((size_t)n, r.size()) * sizeof(wchar_t)); return (int)std::min((size_t)n, r.size());
}
DWORD GetFullPathNameW(const wchar_t* in, DWORD n, wchar_t* out, wchar_t**) {
    std::wstring p = in;
    for (auto& c : p) if (c == L'/') c = L'\\';
    if (!(p.size() > 1 && p[1] == L':')) p = L"Z:\\Game\\" + p;
    std::vector<std::wstring> parts; size_t i = 3; std::wstring drive = p.substr(0, 3);
    while (i <= p.size()) {
        size_t j = p.find(L'\\', i); if (j == std::wstring::npos) j = p.size();
        std::wstring s = p.substr(i, j - i);
        if (s == L"..") { if (!parts.empty()) parts.pop_back(); } else if (!s.empty() && s != L".") parts.push_back(s);
        i = j + 1;
    }
    std::wstring r = drive;
    for (size_t k = 0; k < parts.size(); k++) r += (k ? L"\\" : L"") + parts[k];
    wcsncpy(out, r.c_str(), n); return (DWORD)r.size();
}
static std::string W2N(const std::wstring& w) { std::string s; for (wchar_t c : w) s += (char)c; return s; }
static std::wstring N2W(const std::string& s) { std::wstring w; for (char c : s) w += (wchar_t)(unsigned char)c; return w; }
// Wine-like: 0 ok, 2 file missing, 3 path missing
static int ResolveCI(const wchar_t* win, std::string& out) {
    wchar_t full[1024]; GetFullPathNameW(win, 1024, full, nullptr);
    std::wstring p = full;
    if (wcsncasecmp(p.c_str(), L"Z:\\Game\\", 8) && wcscasecmp(p.c_str(), L"Z:\\Game")) return 3;
    std::string cur = g_root; std::wstring rest = p.size() > 7 ? p.substr(8) : L"";
    size_t i = 0;
    while (i < rest.size()) {
        size_t j = rest.find(L'\\', i); bool last = j == std::wstring::npos; if (last) j = rest.size();
        std::string want = W2N(rest.substr(i, j - i)), found;
        DIR* d = opendir(cur.c_str()); if (!d) return 3;
        while (dirent* e = readdir(d)) if (!strcasecmp(e->d_name, want.c_str())) { found = e->d_name; break; }
        closedir(d);
        if (found.empty()) return last ? 2 : 3;
        cur += "/" + found; i = j + 1;
    }
    out = cur; return 0;
}

// ---- fake kernel objects
struct Obj { int kind; int fd = -1; std::vector<std::string> names; std::string dir; size_t next = 0; std::vector<uint8_t> mem; };
enum { K_FILE = 1, K_FIND, K_MAP };
static std::mutex g_objs_m; static std::set<Obj*> g_objs;
static HANDLE NewObj(Obj* o) { std::lock_guard<std::mutex> l(g_objs_m); g_objs.insert(o); return (HANDLE)o; }
static Obj* GetObj(HANDLE h) { std::lock_guard<std::mutex> l(g_objs_m); return g_objs.count((Obj*)h) ? (Obj*)h : nullptr; }
BOOL CloseHandle(HANDLE h) {
    Obj* o = GetObj(h); if (!o) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    { std::lock_guard<std::mutex> l(g_objs_m); g_objs.erase(o); }
    if (o->fd >= 0) close(o->fd); delete o; return TRUE;
}
BOOL DuplicateHandle(HANDLE, HANDLE h, HANDLE, HANDLE* out, DWORD, BOOL, DWORD) {
    Obj* o = GetObj(h); if (!o || o->kind != K_FILE) return FALSE;
    Obj* n = new Obj{ K_FILE }; n->fd = dup(o->fd); *out = NewObj(n); return TRUE;
}
static void Fill(const std::string& path, const std::string& name, WIN32_FIND_DATAW* fd) {
    struct stat st; stat(path.c_str(), &st); memset(fd, 0, sizeof(*fd));
    fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    fd->nFileSizeLow = (DWORD)st.st_size; fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    fd->ftLastWriteTime.dwLowDateTime = (DWORD)st.st_mtime;
    wcsncpy(fd->cFileName, N2W(name).c_str(), MAX_PATH - 1);
}
static void W2A(const WIN32_FIND_DATAW* w, WIN32_FIND_DATAA* a) {
    memset(a, 0, sizeof(*a)); a->dwFileAttributes = w->dwFileAttributes; a->nFileSizeLow = w->nFileSizeLow; a->nFileSizeHigh = w->nFileSizeHigh;
    a->ftLastWriteTime = w->ftLastWriteTime; strncpy(a->cFileName, W2N(w->cFileName).c_str(), MAX_PATH - 1);
}
HANDLE WINAPI fake_FindFirstFileExW(LPCWSTR path, FINDEX_INFO_LEVELS, LPVOID data, FINDEX_SEARCH_OPS, LPVOID, DWORD) {
    g_real_lookups++;
    std::wstring p = path; for (auto& c : p) if (c == L'/') c = L'\\';
    WIN32_FIND_DATAW* fd = (WIN32_FIND_DATAW*)data;
    size_t slash = p.find_last_of(L'\\'); std::wstring last = p.substr(slash + 1);
    if (last.find_first_of(L"*?") != std::wstring::npos) {
        std::string dir; if (ResolveCI(p.substr(0, slash).c_str(), dir)) { SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
        Obj* f = new Obj{ K_FIND }; f->dir = dir;
        std::string pat = W2N(last); if (pat == "*.*") pat = "*";
        DIR* d = opendir(dir.c_str());
        std::vector<std::string> all; while (dirent* e = readdir(d)) all.push_back(e->d_name); closedir(d);
        std::sort(all.begin(), all.end());
        for (auto& n : all) if (!fnmatch(pat.c_str(), n.c_str(), FNM_CASEFOLD | FNM_PERIOD * 0)) f->names.push_back(n);
        if (f->names.empty()) { delete f; SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
        Fill(dir + "/" + f->names[0], f->names[0], fd); f->next = 1; return NewObj(f);
    }
    std::string real; int r = ResolveCI(path, real);
    if (r) { SetLastError(r); return INVALID_HANDLE_VALUE; }
    Obj* f = new Obj{ K_FIND }; f->next = 1; Fill(real, real.substr(real.find_last_of('/') + 1), fd); return NewObj(f);
}
HANDLE WINAPI fake_FindFirstFileExA(LPCSTR path, FINDEX_INFO_LEVELS l, LPVOID data, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
    WIN32_FIND_DATAW w; HANDLE h = fake_FindFirstFileExW(N2W(path).c_str(), l, &w, o, f, fl);
    if (h != INVALID_HANDLE_VALUE) W2A(&w, (WIN32_FIND_DATAA*)data); return h;
}
BOOL WINAPI fake_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW fd) {
    Obj* f = GetObj(h); if (!f || f->kind != K_FIND) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (f->next >= f->names.size()) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    Fill(f->dir + "/" + f->names[f->next], f->names[f->next], fd); f->next++; return TRUE;
}
BOOL WINAPI fake_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd) { WIN32_FIND_DATAW w; BOOL r = fake_FindNextFileW(h, &w); if (r) W2A(&w, fd); return r; }
BOOL WINAPI fake_FindClose(HANDLE h) { return CloseHandle(h); }
DWORD WINAPI fake_GetFileAttributesW(LPCWSTR path) {
    g_real_lookups++; std::string real; int r = ResolveCI(path, real);
    if (r) { SetLastError(r); return INVALID_FILE_ATTRIBUTES; }
    WIN32_FIND_DATAW fd; Fill(real, "", &fd); return fd.dwFileAttributes;
}
DWORD WINAPI fake_GetFileAttributesA(LPCSTR p) { return fake_GetFileAttributesW(N2W(p).c_str()); }
BOOL WINAPI fake_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS, LPVOID info) {
    g_real_lookups++; std::string real; int r = ResolveCI(path, real);
    if (r) { SetLastError(r); return FALSE; }
    WIN32_FIND_DATAW fd; Fill(real, "", &fd);
    WIN32_FILE_ATTRIBUTE_DATA* d = (WIN32_FILE_ATTRIBUTE_DATA*)info; memset(d, 0, sizeof(*d));
    d->dwFileAttributes = fd.dwFileAttributes; d->nFileSizeLow = fd.nFileSizeLow; d->nFileSizeHigh = fd.nFileSizeHigh; return TRUE;
}
BOOL WINAPI fake_GetFileAttributesExA(LPCSTR p, GET_FILEEX_INFO_LEVELS l, LPVOID i) { return fake_GetFileAttributesExW(N2W(p).c_str(), l, i); }
HANDLE WINAPI fake_CreateFileW(LPCWSTR path, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD disp, DWORD, HANDLE) {
    g_real_lookups++;
    std::string real; int r = ResolveCI(path, real);
    if (r == 2 && (disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == OPEN_ALWAYS)) {
        std::wstring p = path; size_t s = p.find_last_of(L"\\/"); std::string dir;
        if (ResolveCI(p.substr(0, s).c_str(), dir)) { SetLastError(3); return INVALID_HANDLE_VALUE; }
        real = dir + "/" + W2N(p.substr(s + 1)); r = 0;
    }
    if (r) { SetLastError(r); return INVALID_HANDLE_VALUE; }
    int fd = open(real.c_str(), disp == OPEN_EXISTING ? O_RDONLY : (O_RDWR | O_CREAT), 0644);
    if (fd < 0) { SetLastError(5); return INVALID_HANDLE_VALUE; }
    Obj* o = new Obj{ K_FILE }; o->fd = fd; return NewObj(o);
}
HANDLE WINAPI fake_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) { return fake_CreateFileW(N2W(p).c_str(), a, s, sa, d, f, t); }
BOOL WINAPI fake_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD nread, LPOVERLAPPED ov) {
    Obj* o = GetObj(h); if (!o || o->kind != K_FILE) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t got = ov ? pread(o->fd, b, n, (off_t)((uint64_t)ov->OffsetHigh << 32 | ov->Offset)) : read(o->fd, b, n);
    if (got < 0) { SetLastError(ERROR_READ_FAULT); return FALSE; }
    if (nread) *nread = (DWORD)got; if (ov) { ov->Internal = 0; ov->InternalHigh = (ULONG_PTR)got; } return TRUE;
}
BOOL WINAPI fake_SetFilePointerEx(HANDLE h, LARGE_INTEGER d, PLARGE_INTEGER out, DWORD m) {
    Obj* o = GetObj(h); off_t r = lseek(o->fd, d.QuadPart, m == FILE_BEGIN ? SEEK_SET : m == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    if (r < 0) { SetLastError(ERROR_NEGATIVE_SEEK); return FALSE; } if (out) out->QuadPart = r; return TRUE;
}
DWORD WINAPI fake_SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD m) {
    LARGE_INTEGER d; d.QuadPart = hi ? ((int64_t)*hi << 32 | (uint32_t)lo) : lo; LARGE_INTEGER out;
    if (!fake_SetFilePointerEx(h, d, &out, m)) return INVALID_SET_FILE_POINTER; if (hi) *hi = (LONG)(out.QuadPart >> 32); return (DWORD)out.QuadPart;
}
BOOL WINAPI fake_GetFileSizeEx(HANDLE h, PLARGE_INTEGER s) { struct stat st; fstat(GetObj(h)->fd, &st); s->QuadPart = st.st_size; return TRUE; }
DWORD WINAPI fake_GetFileSize(HANDLE h, LPDWORD hi) { LARGE_INTEGER s; fake_GetFileSizeEx(h, &s); if (hi) *hi = (DWORD)(s.QuadPart >> 32); return (DWORD)s.QuadPart; }
BOOL WINAPI fake_GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION i) {
    struct stat st; fstat(GetObj(h)->fd, &st); memset(i, 0, sizeof(*i)); i->dwVolumeSerialNumber = 1234;
    i->nFileSizeLow = (DWORD)st.st_size; i->nFileIndexLow = (DWORD)st.st_ino; i->nNumberOfLinks = 1; return TRUE;
}
BOOL WINAPI fake_GetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS c, LPVOID b, DWORD) {
    struct stat st; fstat(GetObj(h)->fd, &st);
    if (c == FileStandardInfo) { FILE_STANDARD_INFO* s = (FILE_STANDARD_INFO*)b; memset(s, 0, sizeof(*s)); s->EndOfFile.QuadPart = st.st_size; return TRUE; }
    SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
}
BOOL WINAPI fake_GetFileTime(HANDLE h, LPFILETIME a, LPFILETIME b, LPFILETIME c) {
    struct stat st; fstat(GetObj(h)->fd, &st); FILETIME t = { (DWORD)st.st_mtime, 0 }; if (a) *a = t; if (b) *b = t; if (c) *c = t; return TRUE;
}
HANDLE WINAPI fake_CreateFileMappingW(HANDLE h, LPSECURITY_ATTRIBUTES, DWORD, DWORD hi, DWORD lo, LPCWSTR) {
    Obj* m = new Obj{ K_MAP }; uint64_t size = (uint64_t)hi << 32 | lo;
    if (h == INVALID_HANDLE_VALUE) m->mem.assign(size, 0);
    else { Obj* f = GetObj(h); struct stat st; fstat(f->fd, &st); m->mem.resize(size ? size : st.st_size); pread(f->fd, m->mem.data(), m->mem.size(), 0); }
    return NewObj(m);
}
HANDLE WINAPI fake_CreateFileMappingA(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD p, DWORD hi, DWORD lo, LPCSTR) { return fake_CreateFileMappingW(h, sa, p, hi, lo, nullptr); }
void* MapViewOfFile(HANDLE m, DWORD, DWORD, DWORD, SIZE_T) { Obj* o = GetObj(m); return o && o->kind == K_MAP ? o->mem.data() : nullptr; }
BOOL UnmapViewOfFile(const void*) { return TRUE; }
FARPROC GetProcAddress(HMODULE, const char* n) {
    static std::map<std::string, void*> m = {
        {"FindFirstFileExW", (void*)fake_FindFirstFileExW}, {"FindFirstFileExA", (void*)fake_FindFirstFileExA},
        {"FindNextFileW", (void*)fake_FindNextFileW}, {"FindNextFileA", (void*)fake_FindNextFileA}, {"FindClose", (void*)fake_FindClose},
        {"GetFileAttributesW", (void*)fake_GetFileAttributesW}, {"GetFileAttributesA", (void*)fake_GetFileAttributesA},
        {"GetFileAttributesExW", (void*)fake_GetFileAttributesExW}, {"GetFileAttributesExA", (void*)fake_GetFileAttributesExA},
        {"CreateFileW", (void*)fake_CreateFileW}, {"CreateFileA", (void*)fake_CreateFileA}, {"ReadFile", (void*)fake_ReadFile},
        {"SetFilePointer", (void*)fake_SetFilePointer}, {"SetFilePointerEx", (void*)fake_SetFilePointerEx},
        {"GetFileSize", (void*)fake_GetFileSize}, {"GetFileSizeEx", (void*)fake_GetFileSizeEx},
        {"GetFileInformationByHandle", (void*)fake_GetFileInformationByHandle}, {"GetFileInformationByHandleEx", (void*)fake_GetFileInformationByHandleEx},
        {"GetFileTime", (void*)fake_GetFileTime}, {"CloseHandle", (void*)CloseHandle},
        {"CreateFileMappingW", (void*)fake_CreateFileMappingW}, {"CreateFileMappingA", (void*)fake_CreateFileMappingA} };
    return (FARPROC)m[n];
}

// The plain names, for DivaLinux's own file access (divalinux_file_*), which calls them
// directly rather than through the hooked import table.
HANDLE CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t) { return fake_CreateFileW(p, a, s, sa, d, f, t); }
BOOL ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD r, LPOVERLAPPED ov) { return fake_ReadFile(h, b, n, r, ov); }
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER d, PLARGE_INTEGER o, DWORD m) { return fake_SetFilePointerEx(h, d, o, m); }
BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER s) { return fake_GetFileSizeEx(h, s); }

#include "../vfs/vfs.cpp"
#include "../vfs/paths.cpp"
#include "../vfs/wildcard.cpp"
#include "../vfs/zip_index.cpp"
#include "../vfs/archives.cpp"
#include "../vfs/dir_cache.cpp"
#include "../vfs/vhandles.cpp"
#include "../vfs/hooks.cpp"
#include "../common/util.cpp"
#include "../common/iat_patch.cpp"

static int g_checks = 0, g_fail = 0;
static std::mutex g_check_m;
#define CHECK(c, ...) do { std::lock_guard<std::mutex> _l(g_check_m); g_checks++; if (!(c)) { if (g_fail++ < 40) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static std::string g_orig;
static std::vector<uint8_t> Slurp(const std::string& p) { std::vector<uint8_t> v; FILE* f = fopen(p.c_str(), "rb"); int c; while ((c = fgetc(f)) != EOF) v.push_back((uint8_t)c); fclose(f); return v; }
static void Walk(const std::string& root, const std::string& rel, std::vector<std::string>& files, std::vector<std::string>& dirs) {
    DIR* d = opendir((root + "/" + rel).c_str()); dirs.push_back(rel);
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name; if (n == "." || n == "..") continue;
        std::string r = rel.empty() ? n : rel + "/" + n; struct stat st; stat((root + "/" + r).c_str(), &st);
        if (S_ISDIR(st.st_mode)) Walk(root, r, files, dirs); else files.push_back(r);
    }
    closedir(d);
}
static std::wstring Win(const std::string& rel, bool upper) { std::wstring w; for (char c : rel) w += c == '/' ? L'\\' : (wchar_t)(upper ? toupper(c) : c); return w; }

// Reads one file through the hooks in odd-sized chunks, seeks around, maps it; compares with the original.
static void CheckFile(const std::string& rel, bool upper, unsigned seed) {
    std::vector<uint8_t> want = Slurp(g_orig + "/" + rel);
    std::wstring w = Win(rel, upper);
    HANDLE h = Hook_CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE, "%s: open failed (%u)", rel.c_str(), GetLastError());
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz; Hook_GetFileSizeEx(h, &sz); DWORD hi = 7, lo = Hook_GetFileSize(h, &hi);
    CHECK((size_t)sz.QuadPart == want.size() && lo == want.size() && hi == 0, "%s: size %lld/%u vs %zu", rel.c_str(), sz.QuadPart, lo, want.size());
    std::vector<uint8_t> got; uint8_t buf[70000]; DWORD n; srand(seed);
    for (;;) { DWORD ask = 1 + rand() % 70000; BOOL ok = Hook_ReadFile(h, buf, ask, &n, nullptr); if (!ok || !n) break; got.insert(got.end(), buf, buf + n); }
    CHECK(got == want, "%s: contents differ (%zu vs %zu bytes)", rel.c_str(), got.size(), want.size());
    CHECK(Hook_ReadFile(h, buf, 10, &n, nullptr) && n == 0, "%s: read at end", rel.c_str());
    CHECK(Hook_SetFilePointer(h, 0, nullptr, FILE_END) == want.size(), "%s: seek end", rel.c_str());
    if (want.size() > 20) {
        size_t mid = want.size() / 2;
        LARGE_INTEGER d; d.QuadPart = (LONGLONG)mid; LARGE_INTEGER np;
        CHECK(Hook_SetFilePointerEx(h, d, &np, FILE_BEGIN) && (size_t)np.QuadPart == mid, "%s: seek mid", rel.c_str());
        CHECK(Hook_ReadFile(h, buf, 10, &n, nullptr) && n == 10 && !memcmp(buf, &want[mid], 10), "%s: read at mid", rel.c_str());
        CHECK(Hook_SetFilePointer(h, -5, nullptr, FILE_CURRENT) == mid + 5, "%s: seek back", rel.c_str());
        CHECK(Hook_ReadFile(h, buf, 5, &n, nullptr) && n == 5 && !memcmp(buf, &want[mid + 5], 5), "%s: read after seek back", rel.c_str());
        OVERLAPPED ov = {}; ov.Offset = 3;
        CHECK(Hook_ReadFile(h, buf, 7, &n, &ov) && n == 7 && !memcmp(buf, &want[3], 7) && ov.InternalHigh == 7, "%s: overlapped read", rel.c_str());
        CHECK(Hook_SetFilePointer(h, -1, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER && GetLastError() == ERROR_NEGATIVE_SEEK, "%s: negative seek", rel.c_str());
        HANDLE m = Hook_CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
        uint8_t* view = m ? (uint8_t*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : nullptr;
        CHECK(view && !memcmp(view, want.data(), want.size()), "%s: mapping", rel.c_str());
        if (m) CloseHandle(m);
    }
    BY_HANDLE_FILE_INFORMATION bi; CHECK(Hook_GetFileInformationByHandle(h, &bi) && bi.nFileSizeLow == want.size(), "%s: file info", rel.c_str());
    CHECK(Hook_CloseHandle(h), "%s: close", rel.c_str());
}

// Single-name lookups: the hooks on the zipped game vs the fake Wine on the original.
static void CheckLookup(const std::wstring& w) {
    WIN32_FIND_DATAW a, b; std::string zipped = g_root;
    HANDLE ha = Hook_FindFirstFileExW(w.c_str(), FindExInfoBasic, &a, FindExSearchNameMatch, nullptr, 0); DWORD ea = GetLastError();
    g_root = g_orig; HANDLE hb = fake_FindFirstFileExW(w.c_str(), FindExInfoBasic, &b, FindExSearchNameMatch, nullptr, 0); DWORD eb = GetLastError();
    DWORD gb = fake_GetFileAttributesW(w.c_str()), geb = GetLastError(); g_root = zipped;
    DWORD ga = Hook_GetFileAttributesW(w.c_str()), gea = GetLastError();
    bool oka = ha != INVALID_HANDLE_VALUE, okb = hb != INVALID_HANDLE_VALUE;
    CHECK(oka == okb, "%ls: hooks %s, Wine on the folders %s", w.c_str(), oka ? "found" : "missing", okb ? "found" : "missing");
    if (!oka && !okb) CHECK(ea == eb && gea == geb, "%ls: errors %u/%u vs %u/%u", w.c_str(), ea, gea, eb, geb);
    if (oka && okb) {
        CHECK((a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == (b.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            && !wcscasecmp(a.cFileName, b.cFileName) && (a.nFileSizeLow == b.nFileSizeLow || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)),
            "%ls: %ls %x %u vs %ls %x %u", w.c_str(), a.cFileName, a.dwFileAttributes, a.nFileSizeLow, b.cFileName, b.dwFileAttributes, b.nFileSizeLow);
        CHECK((ga & FILE_ATTRIBUTE_DIRECTORY) == (gb & FILE_ATTRIBUTE_DIRECTORY), "%ls: attributes", w.c_str());
        Hook_FindClose(ha); fake_FindClose(hb);
    }
}

static std::set<std::string> ListHooks(const std::wstring& dir, bool ansi) {
    std::set<std::string> out; std::wstring pat = dir + L"\\*";
    if (ansi) { WIN32_FIND_DATAA fd; HANDLE h = Hook_FindFirstFileExA(W2N(pat).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return out; do { std::string n = fd.cFileName; for (auto& c : n) c = tolower(c); if (n != "." && n != "..") out.insert(n); } while (Hook_FindNextFileA(h, &fd)); Hook_FindClose(h); }
    else { WIN32_FIND_DATAW fd; HANDLE h = Hook_FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) return out; do { std::string n = W2N(fd.cFileName); for (auto& c : n) c = tolower(c); if (n != "." && n != "..") out.insert(n); } while (Hook_FindNextFileW(h, &fd)); Hook_FindClose(h); }
    return out;
}
static std::set<std::string> ListLinux(const std::string& dir) {
    std::set<std::string> out; DIR* d = opendir(dir.c_str());
    while (dirent* e = readdir(d)) { std::string n = e->d_name; for (auto& c : n) c = tolower(c); if (n != "." && n != "..") out.insert(n); }
    closedir(d); return out;
}

int main(int argc, char** argv) {
    g_orig = argv[1]; g_root = argv[2]; if (argc > 3) g_cache_setting = atoi(argv[3]);
    divalinux_vfs_install(nullptr, L"x.ini");
    g_quiet = true;
    std::vector<std::string> files, dirs;
    Walk(g_orig, "", files, dirs);
    std::vector<std::string> game;
    for (auto& f : files) if (!f.compare(0, 4, "rom/") || !f.compare(0, 6, "mdata/")) game.push_back(f);
    printf("%zu game files in the original\n", game.size());

    // Every file, through the hooks, in two spellings
    unsigned seed = 1;
    for (auto& f : game) { CheckFile(f, false, seed++); CheckFile(f, true, seed++); }
    // Every file and folder by name, plus names that don't exist
    for (auto& f : game) CheckLookup(Win(f, false));
    for (auto& d : dirs) if (!d.empty()) CheckLookup(Win(d, false));
    for (auto& d : dirs) if (!d.compare(0, 3, "rom") || !d.compare(0, 5, "mdata")) {
        CheckLookup(Win(d + "/nothere.bin", false)); CheckLookup(Win(d + "/missing_dir/x.bin", false));
    }
    CheckLookup(L"mdata\\NOPACK\\rom\\pv_db.txt"); CheckLookup(L"rom\\.\\objset\\..\\pv_db.txt"); CheckLookup(L"Z:\\GAME\\ROM\\PV_DB.TXT");

    // Folder listings, wide and ANSI: same names as the original (mdata also shows the .zip files)
    for (auto& d : dirs) {
        if (d.compare(0, 3, "rom") && d.compare(0, 5, "mdata")) continue;
        std::set<std::string> want = ListLinux(g_orig + "/" + d);
        if (d == "mdata") for (auto& n : ListLinux(g_root + "/mdata")) want.insert(n);
        for (bool ansi : { false, true }) {
            std::set<std::string> got = ListHooks(Win(d, false), ansi);
            CHECK(got == want, "listing %s (%s): %zu names vs %zu", d.c_str(), ansi ? "A" : "W", got.size(), want.size());
        }
    }
    // Pattern searches
    { WIN32_FIND_DATAW fd; int n = 0; HANDLE h = Hook_FindFirstFileExW(L"rom\\objset\\*.farc", FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
      if (h != INVALID_HANDLE_VALUE) { do n++; while (Hook_FindNextFileW(h, &fd)); Hook_FindClose(h); }
      int want = 0; for (auto& n2 : ListLinux(g_orig + "/rom/objset")) want += n2.size() > 5 && n2.substr(n2.size() - 5) == ".farc";
      CHECK(n == want, "*.farc: %d vs %d", n, want); }

    // Archives are read-only
    { std::string any; for (auto& f : game) if (!f.compare(0, 4, "rom/")) { any = f; break; }
      HANDLE h = Hook_CreateFileW(Win(any, false).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      CHECK(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, "write to an archive member");
      h = Hook_CreateFileW(L"rom\\objset\\new.bin", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
      CHECK(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, "create in an archive"); }
    // A loose pack can still be written to, and the new file shows up
    { HANDLE h = Hook_CreateFileW(L"mdata\\MLOOSE\\rom\\new_file.bin", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
      CHECK(h != INVALID_HANDLE_VALUE, "create in a loose pack"); if (h != INVALID_HANDLE_VALUE) Hook_CloseHandle(h);
      CHECK(Hook_GetFileAttributesW(L"mdata\\MLOOSE\\rom\\new_file.bin") != INVALID_FILE_ATTRIBUTES, "new file visible"); }

    // Eight threads reading random files at once
    std::atomic<int> bad{0}; std::vector<std::thread> t;
    for (int k = 0; k < 8; k++) t.emplace_back([&, k] {
        unsigned r = 1000 + k;
        for (int i = 0; i < 300; i++) {
            const std::string& f = game[rand_r(&r) % game.size()];
            std::vector<uint8_t> want = Slurp(g_orig + "/" + f), got(want.size() + 1);
            HANDLE h = Hook_CreateFileW(Win(f, (i & 1) != 0).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) { bad++; continue; }
            DWORD n = 0; Hook_ReadFile(h, got.data(), (DWORD)got.size(), &n, nullptr);
            if (n != want.size() || (n && memcmp(got.data(), want.data(), n))) bad++;
            Hook_CloseHandle(h);
        }
    });
    for (auto& x : t) x.join();
    CHECK(bad == 0, "%d wrong reads from threads", (int)bad);
    CHECK(g_vfiles.empty(), "%zu archive handles left open", g_vfiles.size());

    printf("%d/%d checks passed; %ld misses, %ld hits, %ld passed, %ld listed, %ld opened, %.1f MB read\n",
        g_checks - g_fail, g_checks, (long)g_misses, (long)g_hits, (long)g_passed, (long)g_listed, (long)g_opened, g_bytes / 1048576.0);
    return g_fail != 0;
}
