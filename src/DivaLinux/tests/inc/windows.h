// Minimal Win32 stand-in for testing the file layer (../vfs) on Linux (see test.cpp for the fake kernel).
#pragma once
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#define WINAPI
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
typedef int BOOL; typedef unsigned char BOOLEAN; typedef uint32_t DWORD; typedef int32_t LONG; typedef int64_t LONG64; typedef int64_t LONGLONG;
typedef uint32_t UINT; typedef uintptr_t ULONG_PTR; typedef size_t SIZE_T; typedef void* PVOID;
typedef void* HANDLE; typedef void* HMODULE; typedef void* LPVOID; typedef const wchar_t* LPCWSTR; typedef const char* LPCSTR;
typedef void* LPSECURITY_ATTRIBUTES; typedef DWORD* LPDWORD; typedef LONG* PLONG;
typedef void* (*FARPROC)();
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *LPFILETIME;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; wchar_t cFileName[MAX_PATH]; wchar_t cAlternateFileName[14]; } WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; char cFileName[MAX_PATH]; char cAlternateFileName[14]; } WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks, nFileIndexHigh, nFileIndexLow; } BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;
typedef enum { FindExInfoStandard, FindExInfoBasic } FINDEX_INFO_LEVELS;
typedef enum { FindExSearchNameMatch, FindExSearchLimitToDirectories } FINDEX_SEARCH_OPS;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef enum { FileBasicInfo, FileStandardInfo, FileNameInfo } FILE_INFO_BY_HANDLE_CLASS;
typedef union { struct { DWORD LowPart; LONG HighPart; }; long long QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;
typedef struct { LARGE_INTEGER AllocationSize, EndOfFile; DWORD NumberOfLinks; BOOLEAN DeletePending, Directory; } FILE_STANDARD_INFO;
typedef struct { LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime; DWORD FileAttributes; } FILE_BASIC_INFO;
typedef struct { ULONG_PTR Internal, InternalHigh; DWORD Offset, OffsetHigh; HANDLE hEvent; } OVERLAPPED, *LPOVERLAPPED;
typedef struct { pthread_rwlock_t l; } SRWLOCK;
#define SRWLOCK_INIT { PTHREAD_RWLOCK_INITIALIZER }
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FIND_FIRST_EX_CASE_SENSITIVE 1
#define FIND_FIRST_EX_LARGE_FETCH 2
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_READ_FAULT 30
#define ERROR_INVALID_PARAMETER 87
#define ERROR_NEGATIVE_SEEK 131
#define ERROR_FILE_INVALID 1006
#define ERROR_FILE_CORRUPT 1392
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define FILE_READ_DATA 1
#define FILE_READ_EA 8
#define FILE_EXECUTE 0x20
#define FILE_READ_ATTRIBUTES 0x80
#define READ_CONTROL 0x20000
#define SYNCHRONIZE 0x100000
#define FILE_SHARE_READ 1
#define FILE_SHARE_DELETE 4
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MEM_RESERVE 0x2000
#define PAGE_NOACCESS 1
#define PAGE_READONLY 2
#define PAGE_READWRITE 4
#define PAGE_EXECUTE_READWRITE 0x40
#define FILE_MAP_WRITE 2
#define FILE_MAP_READ 4
#define DUPLICATE_SAME_ACCESS 2
#define CP_ACP 0
#define CP_UTF8 65001
inline void AcquireSRWLockShared(SRWLOCK* l) { pthread_rwlock_rdlock(&l->l); }
inline void ReleaseSRWLockShared(SRWLOCK* l) { pthread_rwlock_unlock(&l->l); }
inline void AcquireSRWLockExclusive(SRWLOCK* l) { pthread_rwlock_wrlock(&l->l); }
inline void ReleaseSRWLockExclusive(SRWLOCK* l) { pthread_rwlock_unlock(&l->l); }
inline LONG InterlockedIncrement(volatile LONG* p) { return __sync_add_and_fetch(p, 1); }
inline LONG InterlockedDecrement(volatile LONG* p) { return __sync_sub_and_fetch(p, 1); }
inline LONG64 InterlockedExchangeAdd64(volatile LONG64* p, LONG64 v) { return __sync_fetch_and_add(p, v); }
inline PVOID InterlockedCompareExchangePointer(PVOID volatile* p, PVOID x, PVOID c) { return __sync_val_compare_and_swap(p, c, x); }
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000LL; return 1; }
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c) { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); c->QuadPart = t.tv_sec * 1000000000LL + t.tv_nsec; return 1; }
inline void* VirtualAlloc(void*, size_t n, DWORD, DWORD) { return aligned_alloc(4096, (n + 4095) & ~(size_t)4095); }
inline BOOL VirtualProtect(void*, size_t, DWORD, DWORD*) { return 1; }
inline HANDLE CreateThread(void*, size_t, DWORD (*)(void*), void*, DWORD, DWORD*) { return nullptr; }
inline UINT GetWindowsDirectoryW(wchar_t* b, UINT) { wcscpy(b, L"C:\\windows"); return 10; }
inline BOOL K32EnumProcessModules(HANDLE, HMODULE*, DWORD, DWORD*) { return 0; }
inline HANDLE GetCurrentProcess() { return (HANDLE)(intptr_t)-2; }
inline void Sleep(DWORD ms) { usleep(ms * 1000); }
inline BOOL SetEvent(HANDLE) { return 1; }
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) { return wcsncasecmp(a, b, n); }
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
int WideCharToMultiByte(UINT, DWORD, const wchar_t*, int, char*, int, void*, void*);
int MultiByteToWideChar(UINT, DWORD, const char*, int, wchar_t*, int);
inline BOOL DosDateTimeToFileTime(uint16_t d, uint16_t t, FILETIME* f) { f->dwLowDateTime = t; f->dwHighDateTime = d; return 1; }
inline BOOL LocalFileTimeToFileTime(const FILETIME* a, FILETIME* b) { *b = *a; return 1; }
DWORD GetLastError(); void SetLastError(DWORD);
DWORD GetModuleFileNameW(HMODULE, wchar_t*, DWORD);
DWORD GetFullPathNameW(const wchar_t*, DWORD, wchar_t*, wchar_t**);
HMODULE GetModuleHandleW(const wchar_t*);
FARPROC GetProcAddress(HMODULE, const char*);
UINT GetPrivateProfileIntW(const wchar_t*, const wchar_t*, int, const wchar_t*);
BOOL CloseHandle(HANDLE);
BOOL DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE*, DWORD, BOOL, DWORD);
void* MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
BOOL UnmapViewOfFile(const void*);
typedef struct { uint16_t e_magic; uint8_t pad[58]; LONG e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct { IMAGE_DATA_DIRECTORY DataDirectory[16]; } IMAGE_OPTIONAL_HEADER;
typedef struct { DWORD Signature; IMAGE_OPTIONAL_HEADER OptionalHeader; } IMAGE_NT_HEADERS;
typedef struct { DWORD OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk; } IMAGE_IMPORT_DESCRIPTOR;
typedef struct { union { ULONG_PTR AddressOfData, Ordinal, Function; } u1; } IMAGE_THUNK_DATA;
typedef struct { uint16_t Hint; char Name[1]; } IMAGE_IMPORT_BY_NAME;
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1
#define IMAGE_SNAP_BY_ORDINAL(o) (((o) >> 63) != 0)
