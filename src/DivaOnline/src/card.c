/* The card behind the emulated Aime reader. Replaces segatools' board/aime-dll.c
   and aimeio/aimeio.c in one file:

   - Built in (the default): the card number comes from DivaOnline.ini,
     [aime] accessCode=<20 digits>. The card is "on the reader" while the
     scan key is held, or R3 (or L2+R2) on a gamepad or a controller on DIVA
     Bridge, or all the time with autoInsert=1. With no accessCode,
     segatools' aime.txt / felica.txt files are read instead, as aimeio does.
   - [aime] reader=auto (or a reader's name): a real reader through PC/SC,
     such as an ACR122U (reader.c). The card on it wins; with none on it,
     the above still applies.
   - [aimeio] path=<dll>: a segatools-style aimeio DLL (for a real reader,
     e.g. a PC/SC one) is loaded and used instead, as segatools does. */

#include <windows.h>

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aimeio/aimeio.h"
#include "board/aime-dll.h"

#include "util/dprintf.h"

#include "divaonline.h"

struct aime_dll aime_dll;

static struct {
    uint8_t access_code[10];
    bool have_access_code;
    uint64_t felica_idm;
    bool have_felica_idm;
    wchar_t aime_path[MAX_PATH];
    wchar_t felica_path[MAX_PATH];
    bool felica_gen;
    int vk_scan;
    bool scan_pad;
    bool auto_insert;
} card_cfg;

static uint8_t card_aime_id[10];
static uint64_t card_felica_id;
static bool card_aime_present;
static bool card_felica_present;
static bool card_was_present;
static bool card_reader;            /* [aime] reader= is running */

/* ---------------------------------------------------------------- config */

static const struct { const wchar_t *name; int vk; } card_key_names[] = {
    { L"Enter", VK_RETURN }, { L"Return", VK_RETURN }, { L"Space", VK_SPACE },
    { L"Insert", VK_INSERT }, { L"Delete", VK_DELETE }, { L"Home", VK_HOME },
    { L"End", VK_END }, { L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT },
    { L"Backspace", VK_BACK }, { L"Tab", VK_TAB }, { L"Escape", VK_ESCAPE },
    { L"Pause", VK_PAUSE }, { L"ScrollLock", VK_SCROLL },
    { L"Numpad0", VK_NUMPAD0 }, { L"NumpadEnter", VK_RETURN },
    { L"NumpadPlus", VK_ADD }, { L"NumpadMinus", VK_SUBTRACT },
    { L"NumpadMultiply", VK_MULTIPLY }, { L"NumpadDivide", VK_DIVIDE },
};

/* "0x2D", "45", "Insert", "F3", "C" */
int divaonline_parse_key(const wchar_t *s, int fallback)
{
    wchar_t *end;
    long v;
    size_t i;

    while (*s == L' ') {
        s++;
    }

    if (*s == L'\0') {
        return fallback;
    }

    if (iswdigit(s[0]) && s[1] != L'\0') {
        v = wcstol(s, &end, 0);

        if (*end == L'\0' && v > 0 && v < 256) {
            return (int) v;
        }
    }

    if (s[1] == L'\0' && (iswalnum(s[0]))) {
        return (int) towupper(s[0]);    /* VK codes for A-Z / 0-9 are ASCII */
    }

    if ((s[0] == L'F' || s[0] == L'f') && iswdigit(s[1])) {
        v = wcstol(s + 1, &end, 10);

        if (*end == L'\0' && v >= 1 && v <= 24) {
            return VK_F1 + (int) v - 1;
        }
    }

    for (i = 0 ; i < _countof(card_key_names) ; i++) {
        if (_wcsicmp(s, card_key_names[i].name) == 0) {
            return card_key_names[i].vk;
        }
    }

    dprintf("Card: unknown key \"%S\", using 0x%02X\n", s, fallback);

    return fallback;
}

static bool card_parse_access_code(const wchar_t *s, uint8_t *out)
{
    wchar_t digits[21];
    size_t n;
    size_t i;

    /* Spaces and dashes are allowed, as access codes are usually written
       in groups of four. */

    for (n = 0 ; *s != L'\0' ; s++) {
        if (*s == L' ' || *s == L'-' || *s == L'\t') {
            continue;
        }

        if (!iswdigit(*s) || n == 20) {
            return false;
        }

        digits[n++] = *s;
    }

    if (n != 20) {
        return false;
    }

    /* The reader sends the access code as 10 BCD bytes, which is also how
       segatools' aime.txt spells it ("01 23 45 ..."). */

    for (i = 0 ; i < 10 ; i++) {
        out[i] = (uint8_t) (((digits[i * 2] - L'0') << 4) | (digits[i * 2 + 1] - L'0'));
    }

    return true;
}

static bool card_parse_idm(const wchar_t *s, uint64_t *out)
{
    uint64_t v = 0;
    size_t n = 0;

    for ( ; *s != L'\0' ; s++) {
        if (*s == L' ' || *s == L'-' || *s == L':') {
            continue;
        }

        if (!iswxdigit(*s) || n == 16) {
            return false;
        }

        v = (v << 4) | (uint64_t) (iswdigit(*s) ? *s - L'0' : (towupper(*s) - L'A' + 10));
        n++;
    }

    if (n != 16) {
        return false;
    }

    *out = v;

    return true;
}

static void card_config_load(const wchar_t *ini)
{
    wchar_t buf[128];

    memset(&card_cfg, 0, sizeof(card_cfg));

    GetPrivateProfileStringW(L"aime", L"accessCode", L"", buf, _countof(buf), ini);

    if (buf[0] != L'\0') {
        card_cfg.have_access_code = card_parse_access_code(buf, card_cfg.access_code);

        if (!card_cfg.have_access_code) {
            dprintf("Card: [aime] accessCode must be 20 digits, ignoring \"%S\"\n", buf);
        }
    }

    GetPrivateProfileStringW(L"aime", L"felicaIdm", L"", buf, _countof(buf), ini);

    if (buf[0] != L'\0') {
        card_cfg.have_felica_idm = card_parse_idm(buf, &card_cfg.felica_idm);

        if (!card_cfg.have_felica_idm) {
            dprintf("Card: [aime] felicaIdm must be 16 hex digits, ignoring \"%S\"\n", buf);
        }
    }

    /* segatools' aimeio settings, used when no number is set above */

    GetPrivateProfileStringW(L"aime", L"aimePath", L"DEVICE\\aime.txt",
            card_cfg.aime_path, _countof(card_cfg.aime_path), ini);
    GetPrivateProfileStringW(L"aime", L"felicaPath", L"DEVICE\\felica.txt",
            card_cfg.felica_path, _countof(card_cfg.felica_path), ini);
    card_cfg.felica_gen = GetPrivateProfileIntW(L"aime", L"felicaGen", 0, ini) != 0;

    /* Enter is segatools' default, but TLAC uses Enter for START, so tapping
       the card would also press START. Insert is free in TLAC's defaults. */

    GetPrivateProfileStringW(L"aime", L"scan", L"Insert", buf, _countof(buf), ini);
    card_cfg.vk_scan = divaonline_parse_key(buf, VK_INSERT);

    card_cfg.scan_pad = GetPrivateProfileIntW(L"aime", L"scanPad", 1, ini) != 0;
    card_cfg.auto_insert = GetPrivateProfileIntW(L"aime", L"autoInsert", 0, ini) != 0;
}

/* ---------------------------------------------------- aime.txt / felica.txt */

static HRESULT card_read_id_file(const wchar_t *path, uint8_t *bytes, size_t nbytes)
{
    FILE *f;
    size_t i;
    int byte;

    f = _wfopen(path, L"r");

    if (f == NULL) {
        return S_FALSE;
    }

    memset(bytes, 0, nbytes);

    for (i = 0 ; i < nbytes ; i++) {
        if (fscanf(f, "%02x ", &byte) != 1) {
            dprintf("Card: %S: could not read byte %u\n", path, (unsigned int) i);
            fclose(f);

            return E_FAIL;
        }

        bytes[i] = (uint8_t) byte;
    }

    fclose(f);

    return S_OK;
}

static HRESULT card_generate_felica(const wchar_t *path, uint8_t *bytes, size_t nbytes)
{
    FILE *f;
    size_t i;

    srand((unsigned int) time(NULL));

    for (i = 0 ; i < nbytes ; i++) {
        bytes[i] = (uint8_t) rand();
    }

    bytes[0] &= 0x0F;   /* as segatools does */

    f = _wfopen(path, L"w");

    if (f == NULL) {
        dprintf("Card: %S: could not create\n", path);

        return E_FAIL;
    }

    for (i = 0 ; i < nbytes ; i++) {
        fprintf(f, "%02X", bytes[i]);
    }

    fprintf(f, "\n");
    fclose(f);
    dprintf("Card: generated a random FeliCa ID in %S\n", path);

    return S_OK;
}

static uint64_t card_bytes_to_idm(const uint8_t *b)
{
    uint64_t v = 0;
    size_t i;

    for (i = 0 ; i < 8 ; i++) {
        v = (v << 8) | b[i];
    }

    return v;
}

/* ---------------------------------------------------------- built-in aimeio */

static HRESULT card_init(void)
{
    char code[32];
    size_t i;

    if (card_cfg.have_access_code) {
        for (i = 0 ; i < 10 ; i++) {
            sprintf(code + i * 2, "%02X", card_cfg.access_code[i]);
        }

        dprintf("Card: access code %.4s-%.4s-%.4s-%.4s-%.4s, %s\n",
                code, code + 4, code + 8, code + 12, code + 16,
                card_cfg.auto_insert ? "always on the reader (autoInsert=1)" : "tap with the scan key");
    } else if (card_cfg.have_felica_idm) {
        dprintf("Card: FeliCa IDm %016llX, %s\n", (unsigned long long) card_cfg.felica_idm,
                card_cfg.auto_insert ? "always on the reader (autoInsert=1)" : "tap with the scan key");
    } else {
        dprintf("Card: no [aime] accessCode set, using %S / %S\n",
                card_cfg.aime_path, card_cfg.felica_path);
    }

    if (!card_cfg.auto_insert) {
        dprintf("Card: scan key is VK 0x%02X%s\n", card_cfg.vk_scan,
                card_cfg.scan_pad ? ", or R3 / L2+R2 on a gamepad or a controller on DIVA Bridge" : "");
    }

    return S_OK;
}

/* R3, or L2 and R2 together: on an XInput gamepad, or on a controller that
   DivallerBridge.dva reads from DIVA Bridge (its extended buttons: LT bit 6,
   RT bit 7, RS bit 9). Under the DivaRecomp runtime (Android), both come
   from the runtime: the headset's gamepad, the Quest's right thumbstick
   click, and slider controllers on USB, Bluetooth or the network. */

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} card_xinput_gamepad;

typedef struct {
    DWORD dwPacketNumber;
    card_xinput_gamepad Gamepad;
} card_xinput_state;

typedef DWORD (WINAPI *card_xinput_get_state_t)(DWORD, card_xinput_state *);
typedef DWORD (__cdecl *card_bridge_ext_t)(void);

#define CARD_XI_RIGHT_THUMB 0x0080
#define CARD_EXT_LT (1u << 6)
#define CARD_EXT_RT (1u << 7)
#define CARD_EXT_RS (1u << 9)

static const char *card_pad_held(void)
{
    static card_xinput_get_state_t get_state;
    static bool xinput_tried;
    static DWORD retry_at[4];           /* disconnected pads are slow to ask: every 2 s */
    static card_bridge_ext_t bridge_ext;
    static DWORD bridge_tried_at;
    static const wchar_t *const dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    DWORD now = GetTickCount();
    DWORD i;

    if (!xinput_tried) {
        xinput_tried = true;

        for (i = 0 ; i < _countof(dlls) && get_state == NULL ; i++) {
            HMODULE mod = GetModuleHandleW(dlls[i]);

            if (mod == NULL) {
                mod = LoadLibraryW(dlls[i]);
            }

            if (mod != NULL) {
                get_state = (card_xinput_get_state_t) GetProcAddress(mod, "XInputGetState");
            }
        }
    }

    for (i = 0 ; get_state != NULL && i < 4 ; i++) {
        card_xinput_state st;

        if (retry_at[i] != 0 && (LONG) (now - retry_at[i]) < 0) {
            continue;
        }

        memset(&st, 0, sizeof(st));

        if (get_state(i, &st) != ERROR_SUCCESS) {
            retry_at[i] = now + 2000;
            if (retry_at[i] == 0) retry_at[i] = 1;
            continue;
        }

        retry_at[i] = 0;

        if (st.Gamepad.wButtons & CARD_XI_RIGHT_THUMB) {
            return "R3";
        }

        if (st.Gamepad.bLeftTrigger > 128 && st.Gamepad.bRightTrigger > 128) {
            return "L2+R2";
        }
    }

    /* DivallerBridge.dva loads after us (directory order): look again now and then */

    if (bridge_ext == NULL && (bridge_tried_at == 0 || now - bridge_tried_at > 2000)) {
        HMODULE mod = GetModuleHandleW(L"DivallerBridge.dva");

        bridge_tried_at = now ? now : 1;

        if (mod != NULL) {
            bridge_ext = (card_bridge_ext_t) GetProcAddress(mod, "DivallerBridge_ExtButtons");
        }
    }

    if (bridge_ext != NULL) {
        DWORD ext = bridge_ext();

        if (ext & CARD_EXT_RS) {
            return "R3 (DIVA Bridge)";
        }

        if ((ext & (CARD_EXT_LT | CARD_EXT_RT)) == (CARD_EXT_LT | CARD_EXT_RT)) {
            return "L2+R2 (DIVA Bridge)";
        }
    }

    return NULL;
}

static bool card_scan_held(void)
{
    if (card_cfg.auto_insert) {
        return true;
    }

    /* Only while the game window (or its console) has focus: a key held in
       another program shouldn't tap a card. */

    {
        static bool warned;
        static const char *last_pad;
        DWORD pid = 0;
        const char *pad = card_cfg.scan_pad ? card_pad_held() : NULL;
        bool held = (GetAsyncKeyState(card_cfg.vk_scan) & 0x8000) != 0 || pad != NULL;

        if (pad != last_pad) {
            if (pad != NULL) {
                dprintf("Card: tapped with %s\n", pad);
            }

            last_pad = pad;
        }

        if (!held) {
            return false;
        }

        if (GetForegroundWindow() != NULL) {
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        }

        if (pid != GetCurrentProcessId()) {
            if (!warned) {
                dprintf("Card: scan key or button held, but the game window isn't in front; ignored\n");
                warned = true;
            }

            return false;
        }

        return true;
    }
}

static HRESULT card_nfc_poll(uint8_t unit_no)
{
    uint8_t felica[8];
    HRESULT hr;
    bool present;

    if (unit_no != 0) {
        return S_OK;
    }

    {
        static bool first_poll = true;

        if (first_poll) {
            dprintf("Card: the game is looking for a card now (hold the scan key, or R3 / L2+R2)\n");
            first_poll = false;
        }
    }

    card_aime_present = false;
    card_felica_present = false;

    /* a card on a real reader comes first */
    if (card_reader) {
        uint64_t idm;
        uint8_t code[10];
        int kind = divaonline_reader_card(&idm, code);

        if (kind == 1) {
            card_felica_id = idm;
            card_felica_present = true;
        } else if (kind == 2) {
            memcpy(card_aime_id, code, sizeof(card_aime_id));
            card_aime_present = true;
        }

        if (kind != 0) {
            if (!card_was_present) {
                dprintf("Card: on the reader (the real one)\n");
            }

            card_was_present = true;

            return S_OK;
        }
    }

    if (!card_scan_held()) {
        card_was_present = false;

        return S_OK;
    }

    if (card_cfg.have_access_code) {
        memcpy(card_aime_id, card_cfg.access_code, sizeof(card_aime_id));
        card_aime_present = true;
    } else if (card_cfg.have_felica_idm) {
        card_felica_id = card_cfg.felica_idm;
        card_felica_present = true;
    } else {
        hr = card_read_id_file(card_cfg.aime_path, card_aime_id, sizeof(card_aime_id));

        if (hr == S_OK) {
            card_aime_present = true;
        } else {
            hr = card_read_id_file(card_cfg.felica_path, felica, sizeof(felica));

            if (hr == S_FALSE && card_cfg.felica_gen) {
                hr = card_generate_felica(card_cfg.felica_path, felica, sizeof(felica));
            }

            if (hr == S_OK) {
                card_felica_id = card_bytes_to_idm(felica);
                card_felica_present = true;
            }
        }
    }

    present = card_aime_present || card_felica_present;

    if (present && !card_was_present) {
        dprintf("Card: on the reader\n");
    } else if (!present && !card_was_present) {
        dprintf("Card: scan key held but no card number is set\n");
    }

    card_was_present = true;

    return S_OK;
}

static HRESULT card_nfc_get_aime_id(uint8_t unit_no, uint8_t *luid, size_t luid_size)
{
    assert(luid != NULL);

    if (unit_no != 0 || !card_aime_present || luid_size != sizeof(card_aime_id)) {
        return S_FALSE;
    }

    memcpy(luid, card_aime_id, luid_size);

    return S_OK;
}

static HRESULT card_nfc_get_felica_id(uint8_t unit_no, uint64_t *IDm)
{
    assert(IDm != NULL);

    if (unit_no != 0 || !card_felica_present) {
        return S_FALSE;
    }

    *IDm = card_felica_id;

    return S_OK;
}

static void card_led_set_color(uint8_t unit_no, uint8_t r, uint8_t g, uint8_t b)
{
    (void) unit_no;
    (void) r;
    (void) g;
    (void) b;
}

/* ---------------------------------------------------------------- aime_dll */

static HRESULT card_bind_external(const wchar_t *path)
{
    uint16_t (*get_api_version)(void);
    HMODULE dll;

    dll = LoadLibraryW(path);

    if (dll == NULL) {
        dprintf("Card: could not load aimeio DLL %S (error %lu)\n", path, GetLastError());

        return E_FAIL;
    }

    get_api_version = (void *) GetProcAddress(dll, "aime_io_get_api_version");
    aime_dll.api_version = get_api_version != NULL ? get_api_version() : 0x0100;

    if (aime_dll.api_version >= 0x0200) {
        dprintf("Card: %S has unsupported aimeio API version %04x\n", path, aime_dll.api_version);

        return E_NOTIMPL;
    }

    aime_dll.init = (void *) GetProcAddress(dll, "aime_io_init");
    aime_dll.nfc_poll = (void *) GetProcAddress(dll, "aime_io_nfc_poll");
    aime_dll.nfc_get_aime_id = (void *) GetProcAddress(dll, "aime_io_nfc_get_aime_id");
    aime_dll.nfc_get_felica_id = (void *) GetProcAddress(dll, "aime_io_nfc_get_felica_id");
    aime_dll.led_set_color = (void *) GetProcAddress(dll, "aime_io_led_set_color");

    if (aime_dll.init == NULL || aime_dll.nfc_poll == NULL || aime_dll.nfc_get_aime_id == NULL
            || aime_dll.nfc_get_felica_id == NULL || aime_dll.led_set_color == NULL) {
        dprintf("Card: %S is missing aime_io_* exports\n", path);

        return E_FAIL;
    }

    dprintf("Card: using aimeio DLL %S (API %04x)\n", path, aime_dll.api_version);

    return S_OK;
}

static void card_use_builtin(void)
{
    card_config_load(divaonline_ini_path());

    /* the reader is watched from the start, so it's ready when the card screen comes */
    if (!card_reader) {
        card_reader = divaonline_reader_start(divaonline_ini_path());
    }

    aime_dll.api_version = 0x0100;
    aime_dll.init = card_init;
    aime_dll.nfc_poll = card_nfc_poll;
    aime_dll.nfc_get_aime_id = card_nfc_get_aime_id;
    aime_dll.nfc_get_felica_id = card_nfc_get_felica_id;
    aime_dll.led_set_color = card_led_set_color;
}

static HRESULT (*card_external_init)(void);

/* An aimeio DLL that fails to start (reader unplugged, its own .ini missing)
   would leave the game's card system dead. Fall back to the built-in card
   (the scan key) instead, and say so. */
static HRESULT card_external_init_or_fallback(void)
{
    HRESULT hr = card_external_init();

    if (SUCCEEDED(hr)) {
        return hr;
    }

    dprintf("Card: aimeio DLL failed to start (%08lx); using the built-in card instead"
            " ([aime] accessCode, scan key)\n", (unsigned long) hr);
    card_use_builtin();

    return aime_dll.init();
}

HRESULT aime_dll_init(const struct aime_dll_config *cfg, HINSTANCE self)
{
    HRESULT hr;

    (void) self;

    assert(cfg != NULL);

    if (cfg->path[0] != L'\0') {
        hr = card_bind_external(cfg->path);

        if (SUCCEEDED(hr)) {
            card_external_init = aime_dll.init;
            aime_dll.init = card_external_init_or_fallback;

            return hr;
        }

        dprintf("Card: falling back to the built-in card\n");
    }

    card_use_builtin();

    return S_OK;
}
