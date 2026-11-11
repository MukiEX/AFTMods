/* Smoke test for DivaOnline.dva, run under Wine or Windows (not part of the plugin).
   Acts as the game: loads plugins\DivaOnline.dva the way PD Loader does, then
   uses the APIs diva.exe uses for the keychip, ALL.Net DNS and the Aime reader
   on COM10, and checks that DivaOnline answers them.
   Build: x86_64-w64-mingw32-clang -O1 -o smoke.exe smoke.c -lws2_32 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (c) printf("ok   "); else { printf("FAIL "); failures++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static HANDLE com;
static uint8_t seq;

static void send_frame(uint8_t cmd, const uint8_t *payload, uint8_t n)
{
    uint8_t raw[300], out[600];
    size_t len = 0, o = 0;
    uint8_t sum = 0;
    DWORD w;
    raw[len++] = (uint8_t) (5 + n);
    raw[len++] = 0x00;          /* NFC address */
    raw[len++] = seq++;
    raw[len++] = cmd;
    raw[len++] = n;
    memcpy(raw + len, payload, n);
    len += n;
    out[o++] = 0xE0;
    for (size_t i = 0; i <= len; i++) {
        uint8_t b = i < len ? raw[i] : sum;
        if (i < len) sum += b;
        if (b == 0xE0 || b == 0xD0) { out[o++] = 0xD0; out[o++] = b - 1; } else out[o++] = b;
    }
    WriteFile(com, out, (DWORD) o, &w, NULL);
}

/* returns decoded frame length (without sync and checksum) */
static size_t recv_frame(uint8_t *f)
{
    uint8_t in[600];
    DWORD r = 0;
    size_t n = 0;
    ReadFile(com, in, sizeof(in), &r, NULL);
    for (DWORD i = 0; i < r; i++) {
        if (in[i] == 0xE0) { n = 0; continue; }
        if (in[i] == 0xD0 && i + 1 < r) { f[n++] = in[++i] + 1; continue; }
        f[n++] = in[i];
    }
    return n ? n - 1 : 0;
}

int main(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    *wcsrchr(dir, L'\\') = 0;
    swprintf(path, MAX_PATH, L"%ls\\plugins\\DivaOnline.dva", dir);

    /* PD Loader: LoadLibrary, then InitializeDVA */
    HMODULE m = LoadLibraryW(path);
    CHECK(m != NULL, "LoadLibrary DivaOnline.dva");
    if (!m) return 1;
    void (*init)(void) = (void (*)(void)) GetProcAddress(m, "InitializeDVA");
    CHECK(init != NULL, "InitializeDVA exported");
    init();

    /* keychip, as amLib reads it */
    HKEY k;
    char id[32] = {0};
    DWORD sz = sizeof(id), type = 0;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\SEGA\\SystemProperty\\keychip", 0, KEY_READ, &k);
    CHECK(rc == ERROR_SUCCESS, "open SystemProperty\\keychip (%ld)", rc);
    if (rc == ERROR_SUCCESS) {
        rc = RegQueryValueExW(k, L"keychipId", NULL, &type, (BYTE *) id, &sz);
        CHECK(rc == ERROR_SUCCESS && memcmp(id, "A69E-01A12345678", 16) == 0, "keychipId = %.16s", id);
        sz = sizeof(id); memset(id, 0, sizeof(id));
        rc = RegQueryValueExW(k, L"gameId", NULL, &type, (BYTE *) id, &sz);
        CHECK(rc == ERROR_SUCCESS && memcmp(id, "SBZV", 4) == 0, "gameId = %.4s", id);
        RegCloseKey(k);
    }

    /* ALL.Net host names */
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const char *hosts[] = { "naominet.jp", "ib.naominet.jp", "aime.naominet.jp", "tenporouter.loc" };
    for (int i = 0; i < 4; i++) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        int e = getaddrinfo(hosts[i], "80", &hints, &res);
        char ip[64] = "?";
        if (e == 0) inet_ntop(AF_INET, &((struct sockaddr_in *) res->ai_addr)->sin_addr, ip, sizeof(ip));
        CHECK(e == 0 && strcmp(ip, "10.20.30.40") == 0, "getaddrinfo %s -> %s", hosts[i], ip);
        if (res) freeaddrinfo(res);
    }

    /* Aime reader */
    com = CreateFileW(L"\\\\.\\COM10", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(com != INVALID_HANDLE_VALUE, "open COM10");
    if (com == INVALID_HANDLE_VALUE) return 1;
    uint8_t f[300];
    size_t n;

    send_frame(0x30, NULL, 0);  /* get firmware version */
    n = recv_frame(f);
    CHECK(n > 6 && f[3] == 0x30 && f[4] == 0, "firmware version: %.*s", (int) (n > 6 ? n - 6 : 0), f + 6);

    send_frame(0x40, (const uint8_t *) "\x01\x03", 2);  /* radio on */
    recv_frame(f);
    send_frame(0x42, NULL, 0);  /* poll */
    n = recv_frame(f);
    CHECK(n >= 7 && f[3] == 0x42 && f[6] == 1 && f[7] == 0x10, "poll: %u card(s), type %02x (MIFARE)", n >= 7 ? f[6] : 0, n >= 8 ? f[7] : 0);
    uint8_t uid[4];
    memcpy(uid, f + 9, 4);

    uint8_t rq[5];
    memcpy(rq, uid, 4);
    rq[4] = 2;                  /* block 2 holds the access code */
    send_frame(0x52, rq, 5);
    n = recv_frame(f);
    const uint8_t want[10] = { 0x50, 0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56, 0x78 };
    CHECK(n >= 22 && memcmp(f + 6 + 6, want, 10) == 0, "block 2 access code %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
          f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19], f[20], f[21]);
    CloseHandle(com);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
