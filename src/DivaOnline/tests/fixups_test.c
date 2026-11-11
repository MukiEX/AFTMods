/* Checks DivaOnline's undo of PD Loader patches against the real diva.exe 7.10 image.
   Maps diva.exe (without running it) at its base, writes PD Loader's quick_start=2 and
   network check patches, loads DivaOnline.dva and checks the original bytes are back.
   Build with a different image base so 0x140000000 is free for diva.exe:
     x86_64-w64-mingw32-clang -O1 -o fixups_test.exe fixups_test.c -Wl,--image-base,0x10000000
   Run next to diva.exe and plugins/DivaOnline.dva (with undo_network_patches=1). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void put(uintptr_t a, const char *b, size_t n)
{
    DWORD old;
    VirtualProtect((void *) a, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *) a, b, n);
    VirtualProtect((void *) a, n, old, &old);
}

static void expect(uintptr_t a, const char *b, size_t n, const char *what)
{
    int ok = memcmp((void *) a, b, n) == 0;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

int main(void)
{
    HMODULE diva = LoadLibraryExW(L"diva.exe", NULL, DONT_RESOLVE_DLL_REFERENCES);
    printf("%s diva.exe mapped at %p\n", diva == (HMODULE) 0x140000000 ? "ok  " : "FAIL", (void *) diva);
    if (diva != (HMODULE) 0x140000000) return 1;

    put(0x140578EA9, "\xE9\xF1\x00\x00\x00", 5);
    put(0x140578E9D, "\xC7\x47\x68\x28\x00\x00\x00", 7);
    put(0x1406717B1, "\xE9\x22\x03\x00", 4);
    put(0x1406724E7, "\x00\x00", 2);
    put(0x1406732A2, "\x90\x90", 2);

    HMODULE m = LoadLibraryW(L"plugins\\DivaOnline.dva");
    ((void (*)(void)) GetProcAddress(m, "InitializeDVA"))();

    expect(0x140578EA9, "\xE8\x92\xC5\xFE\xFF", 5, "quick start jump restored");
    expect(0x140578E9D, "\xC7\x47\x68\x0A\x00\x00\x00", 7, "back button state restored");
    expect(0x1406717B1, "\x0F\x85\x21\x03", 4, "network check jump restored");
    expect(0x1406724E7, "\xA0\x8C", 2, "DHCP wait restored");
    expect(0x1406732A2, "\x75\x1E", 2, "location server check restored");
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures != 0;
}
