// The file layer's parts (vfs/*.cpp) share these. See vfs.cpp for what it does.
//
//   vfs.cpp        settings, install, hooking modules, statistics, DivaLinux's own file access
//   paths.cpp      which paths are ours (rom\ and mdata\ in the game folder)
//   wildcard.*     Windows-style wildcard matching (no Windows: tested on Linux)
//   zip_index.*    reading a stored zip's central directory (no Windows: tested on Linux)
//   archives.cpp   finding rom.zip / mdata\*.zip and turning their index into folders
//   dir_cache.cpp  folder listings, made once and answered from memory
//   vhandles.cpp   our find handles and archive member handles
//   hooks.cpp      the kernel32 replacements
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/util.hpp"
#include "wildcard.hpp"

namespace vfs {
    // ------------------------------------------------------------ kernel32's own functions (vfs.cpp)

    typedef HANDLE (WINAPI* P_FindFirstFileExW)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    typedef HANDLE (WINAPI* P_FindFirstFileExA)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
    typedef BOOL (WINAPI* P_FindNextFileW)(HANDLE, LPWIN32_FIND_DATAW);
    typedef BOOL (WINAPI* P_FindNextFileA)(HANDLE, LPWIN32_FIND_DATAA);
    typedef BOOL (WINAPI* P_FindClose)(HANDLE);
    typedef DWORD (WINAPI* P_GetFileAttributesW)(LPCWSTR);
    typedef DWORD (WINAPI* P_GetFileAttributesA)(LPCSTR);
    typedef BOOL (WINAPI* P_GetFileAttributesExW)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    typedef BOOL (WINAPI* P_GetFileAttributesExA)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
    typedef HANDLE (WINAPI* P_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    typedef HANDLE (WINAPI* P_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    typedef BOOL (WINAPI* P_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
    typedef DWORD (WINAPI* P_SetFilePointer)(HANDLE, LONG, PLONG, DWORD);
    typedef BOOL (WINAPI* P_SetFilePointerEx)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
    typedef DWORD (WINAPI* P_GetFileSize)(HANDLE, LPDWORD);
    typedef BOOL (WINAPI* P_GetFileSizeEx)(HANDLE, PLARGE_INTEGER);
    typedef BOOL (WINAPI* P_GetFileInformationByHandle)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
    typedef BOOL (WINAPI* P_GetFileInformationByHandleEx)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
    typedef BOOL (WINAPI* P_GetFileTime)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME);
    typedef BOOL (WINAPI* P_CloseHandle)(HANDLE);
    typedef HANDLE (WINAPI* P_CreateFileMappingW)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
    typedef HANDLE (WINAPI* P_CreateFileMappingA)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);

    extern P_FindFirstFileExW real_FindFirstFileExW;
    extern P_FindFirstFileExA real_FindFirstFileExA;
    extern P_FindNextFileW real_FindNextFileW;
    extern P_FindNextFileA real_FindNextFileA;
    extern P_FindClose real_FindClose;
    extern P_GetFileAttributesW real_GetFileAttributesW;
    extern P_GetFileAttributesA real_GetFileAttributesA;
    extern P_GetFileAttributesExW real_GetFileAttributesExW;
    extern P_GetFileAttributesExA real_GetFileAttributesExA;
    extern P_CreateFileW real_CreateFileW;
    extern P_CreateFileA real_CreateFileA;
    extern P_ReadFile real_ReadFile;
    extern P_SetFilePointer real_SetFilePointer;
    extern P_SetFilePointerEx real_SetFilePointerEx;
    extern P_GetFileSize real_GetFileSize;
    extern P_GetFileSizeEx real_GetFileSizeEx;
    extern P_GetFileInformationByHandle real_GetFileInformationByHandle;
    extern P_GetFileInformationByHandleEx real_GetFileInformationByHandleEx;
    extern P_GetFileTime real_GetFileTime;
    extern P_CloseHandle real_CloseHandle;
    extern P_CreateFileMappingW real_CreateFileMappingW;
    extern P_CreateFileMappingA real_CreateFileMappingA;

    // ------------------------------------------------------------ data (vfs.cpp)

    struct Archive;

    struct ZipItem {               // one stored member of an archive
        Archive* archive;
        uint64_t header_offset;    // local file header
        uint64_t size;
        std::atomic<int64_t> data_offset{ -1 }; // found from the local header on first open
        uint32_t index;            // for BY_HANDLE_FILE_INFORMATION's file index
        ZipItem(Archive* a, uint64_t h, uint64_t s, uint32_t i) : archive(a), header_offset(h), size(s), index(i) {}
    };

    struct Entry {
        std::wstring name;         // as on disk / in the archive
        DWORD attributes = 0;
        FILETIME created = {}, accessed = {}, written = {};
        uint64_t size = 0;
        ZipItem* item = nullptr;   // set for archive members
    };

    struct Dir {
        bool exists = false;
        bool unknown = false;      // couldn't be listed: lookups in it go to Wine
        bool archived = false;     // inside an archive
        std::wstring real_path;    // as on disk, no trailing backslash (real folders)
        std::unordered_map<std::wstring, Entry> entries; // lower-case name -> entry
    };

    struct Archive {
        std::wstring path;         // the .zip, as on disk
        std::wstring root;         // the folder it stands for, lower case absolute
        std::wstring root_name;    // that folder's name as shown in listings ("MZKO", "rom")
        HANDLE file = INVALID_HANDLE_VALUE;
        FILETIME written = {};
        uint64_t file_size = 0;
        std::deque<ZipItem> items;
        bool ok = false;
    };

    extern bool g_cache, g_archives, g_enabled;
    extern HMODULE g_self;
    extern std::wstring g_base;        // game folder, lower case, trailing backslash
    extern std::wstring g_base_dir;    // the same without the trailing backslash
    extern std::wstring g_base_real;   // the game folder as on disk, no trailing backslash
    extern std::vector<Archive*> g_archive_list;

    extern SRWLOCK g_lock;
    extern std::unordered_map<std::wstring, Dir> g_dirs; // lower-case absolute path -> listing

    // Statistics (approximate)
    extern volatile LONG g_misses, g_hits, g_passed, g_listed, g_dropped;
    extern volatile LONG g_opened, g_enumerated;
    extern volatile LONG64 g_bytes;
    extern double g_list_ms;

    enum Answer { PASS, FOUND, MISSING_FILE, MISSING_PATH };

    struct FindState {
        std::vector<Entry> entries;
        size_t next = 0;
    };

    struct VFile {
        ZipItem* item;
        uint64_t data;     // offset of the member's data in the archive
        uint64_t size;
        uint64_t pos = 0;
        FILETIME written;
        DWORD attributes;
    };

    // ------------------------------------------------------------ paths.cpp

    bool Absolute(const wchar_t* path, std::wstring& full);
    bool Resolve(const wchar_t* path, std::wstring& dir, std::wstring& name);
    bool ResolveSearch(const wchar_t* path, std::wstring& dir, std::wstring& pattern);

    // ------------------------------------------------------------ archives.cpp

    Archive* ArchiveFor(const std::wstring& dir);
    bool IsArchiveParent(const std::wstring& dir);
    bool ReadAt(HANDLE file, uint64_t offset, void* buffer, DWORD size);
    void FindArchives();

    // ------------------------------------------------------------ dir_cache.cpp

    Dir& GetDirLocked(const std::wstring& dir);
    Answer Classify(const Dir& d, const std::wstring& name, Entry* out);
    bool Answerable(const std::wstring& key, const Dir& d);
    Answer Lookup(const wchar_t* path, Entry& out);
    void Drop(const wchar_t* path);

    // ------------------------------------------------------------ vhandles.cpp

    extern uint8_t* g_find_base;
    const size_t FIND_SLOTS = 4096;
    HANDLE NewFind(FindState* s);
    FindState* GetFind(HANDLE h, size_t* index);
    bool CloseFind(HANDLE h);   // false if it isn't ours
    void FillW(WIN32_FIND_DATAW* fd, const Entry& e);
    void FillA(WIN32_FIND_DATAA* fd, const Entry& e);
    Answer FindFirst(const wchar_t* path, FINDEX_INFO_LEVELS level, FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags,
        bool answer_found, FindState** state);
    HANDLE FinishFind(Answer a, FindState* s, HANDLE (*fill_and_pass)(void*), void* ctx);
    bool GetVFile(HANDLE h, VFile* out);
    void SetPos(HANDLE h, uint64_t pos);
    void ForgetVFile(HANDLE h); // before the handle itself is closed
    HANDLE CreateFileCommon(const wchar_t* path, DWORD access, DWORD disposition, DWORD flags, bool serve_members, bool* pass);

    // ------------------------------------------------------------ hooks.cpp

    HANDLE WINAPI Hook_FindFirstFileExW(LPCWSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl);
    HANDLE WINAPI Hook_FindFirstFileExW_Lookups(LPCWSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl);
    HANDLE WINAPI Hook_FindFirstFileExA(LPCSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl);
    HANDLE WINAPI Hook_FindFirstFileExA_Lookups(LPCSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl);
    BOOL WINAPI Hook_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data);
    BOOL WINAPI Hook_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data);
    BOOL WINAPI Hook_FindClose(HANDLE h);
    DWORD WINAPI Hook_GetFileAttributesW(LPCWSTR path);
    DWORD WINAPI Hook_GetFileAttributesA(LPCSTR path);
    BOOL WINAPI Hook_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info);
    BOOL WINAPI Hook_GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info);
    HANDLE WINAPI Hook_CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t);
    HANDLE WINAPI Hook_CreateFileW_Lookups(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t);
    HANDLE WINAPI Hook_CreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t);
    HANDLE WINAPI Hook_CreateFileA_Lookups(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t);
    BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buffer, DWORD n, LPDWORD read, LPOVERLAPPED ov);
    DWORD WINAPI Hook_SetFilePointer(HANDLE h, LONG low, PLONG high, DWORD method);
    BOOL WINAPI Hook_SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, PLARGE_INTEGER out, DWORD method);
    DWORD WINAPI Hook_GetFileSize(HANDLE h, LPDWORD high);
    BOOL WINAPI Hook_GetFileSizeEx(HANDLE h, PLARGE_INTEGER size);
    BOOL WINAPI Hook_GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info);
    BOOL WINAPI Hook_GetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID buffer, DWORD size);
    BOOL WINAPI Hook_GetFileTime(HANDLE h, LPFILETIME created, LPFILETIME accessed, LPFILETIME written);
    BOOL WINAPI Hook_CloseHandle(HANDLE h);
    HANDLE WINAPI Hook_CreateFileMappingW(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name);
    HANDLE WINAPI Hook_CreateFileMappingA(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCSTR name);
}
