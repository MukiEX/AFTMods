/* A real NFC card reader through PC/SC (winscard.dll): [aime] reader=...

   For an ACR122U and other PC/SC contactless readers. A thread asks the reader
   for its card every 250 ms; the card the game sees is the one on the reader:

   - FeliCa (a current Aime, Amusement IC, any FeliCa card): its IDm, read
     with the PC/SC "get data" command (FF CA 00 00 00). The server turns it
     into the same access code it gave the card before.
   - MIFARE Classic (an older Aime): the access code, 10 BCD bytes at offset
     6 of block 2, read with the reader's load key / authenticate / read
     binary commands and the keys Aime cards use. A MIFARE card whose block
     2 can't be read is ignored (its UID is not an access code).

   The reader's own "get data" and MIFARE commands (FF xx ...) are PC/SC part 3
   pseudo-APDUs the reader answers itself. One PN532 command goes through too:
   each look polls for the card over the air, since an ACR122U keeps reporting
   the last card it saw after it has gone (reader_card_answers). Under the DivaRecomp runtime (Android), winscard.dll is the
   runtime's: a CCID reader on USB.

   winscard.dll is loaded only when a reader is configured. */

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util/dprintf.h"

#include "divaonline.h"

typedef LONG (WINAPI *est_t)(DWORD, const void *, const void *, ULONG_PTR *);
typedef LONG (WINAPI *rel_t)(ULONG_PTR);
typedef LONG (WINAPI *list_t)(ULONG_PTR, const char *, char *, DWORD *);
typedef LONG (WINAPI *conn_t)(ULONG_PTR, const char *, DWORD, DWORD, ULONG_PTR *, DWORD *);
typedef LONG (WINAPI *disc_t)(ULONG_PTR, DWORD);
typedef struct { DWORD dwProtocol; DWORD cbPciLength; } reader_io_request;
typedef LONG (WINAPI *xmit_t)(ULONG_PTR, const reader_io_request *, const BYTE *, DWORD,
                              reader_io_request *, BYTE *, DWORD *);

#define R_SCOPE_USER 0
#define R_SHARE_SHARED 2
#define R_PROTOCOL_T0_T1 3
#define R_LEAVE_CARD 0
#define R_E_NO_SMARTCARD ((LONG) 0x8010000C)
#define R_W_REMOVED_CARD ((LONG) 0x80100069)
#define R_E_NO_READERS ((LONG) 0x8010002E)
#define R_E_TIMEOUT ((LONG) 0x8010000A)

static struct {
    est_t est;
    rel_t rel;
    list_t list;
    conn_t conn;
    disc_t disc;
    xmit_t xmit;
} sc;

static char reader_want[64];                /* name to look for ("" = any; ACR122 first) */
static CRITICAL_SECTION reader_lock;
static bool reader_card;                    /* a usable card is on the reader */
static bool reader_card_felica;
static uint64_t reader_card_idm;
static uint8_t reader_card_aime[10];
static bool reader_running;

/* The keys Aime MIFARE cards are read with (sector 0, block 2): tried as key A,
   then as key B. */
static const uint8_t reader_mifare_keys[][6] = {
    { 0x57, 0x43, 0x43, 0x46, 0x76, 0x32 },     /* "WCCFv2" */
    { 0x60, 0x90, 0xD0, 0x06, 0x32, 0xF5 },
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },     /* blank / transport key */
};

static LONG reader_apdu(ULONG_PTR card, DWORD proto, const BYTE *apdu, DWORD n,
                        BYTE *resp, DWORD *resp_len)
{
    reader_io_request pci = { proto, sizeof(reader_io_request) };

    return sc.xmit(card, &pci, apdu, n, NULL, resp, resp_len);
}

/* An APDU whose answer must end in 90 00; returns the data length, or -1 */
static int reader_ok(ULONG_PTR card, DWORD proto, const BYTE *apdu, DWORD n, BYTE *data, DWORD cap)
{
    BYTE resp[64];
    DWORD len = sizeof(resp);

    if (reader_apdu(card, proto, apdu, n, resp, &len) != 0 || len < 2) {
        return -1;
    }

    if (resp[len - 2] != 0x90 || resp[len - 1] != 0x00) {
        return -1;
    }

    len -= 2;

    if (data != NULL) {
        memcpy(data, resp, len < cap ? len : cap);
    }

    return (int) len;
}

static bool reader_bcd(const uint8_t *b, size_t n)
{
    size_t i;

    for (i = 0 ; i < n ; i++) {
        if ((b[i] >> 4) > 9 || (b[i] & 0xF) > 9) {
            return false;
        }
    }

    return true;
}

/* An older Aime: block 2 holds the access code */
static bool reader_read_mifare(ULONG_PTR card, DWORD proto, uint8_t code[10])
{
    static const BYTE read2[] = { 0xFF, 0xB0, 0x00, 0x02, 0x10 };
    BYTE load[11] = { 0xFF, 0x82, 0x00, 0x00, 0x06 };
    BYTE auth[10] = { 0xFF, 0x86, 0x00, 0x00, 0x05, 0x01, 0x00, 0x02, 0x60, 0x00 };
    BYTE block[16];
    size_t k;
    int type;

    for (k = 0 ; k < _countof(reader_mifare_keys) ; k++) {
        memcpy(load + 5, reader_mifare_keys[k], 6);

        if (reader_ok(card, proto, load, sizeof(load), NULL, 0) < 0) {
            continue;
        }

        for (type = 0x60 ; type <= 0x61 ; type++) {
            auth[8] = (BYTE) type;

            if (reader_ok(card, proto, auth, sizeof(auth), NULL, 0) < 0) {
                continue;
            }

            if (reader_ok(card, proto, read2, sizeof(read2), block, sizeof(block)) != 16) {
                continue;
            }

            if (!reader_bcd(block + 6, 10)) {
                dprintf("Reader: MIFARE block 2 isn't an access code: "
                        "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                        block[0], block[1], block[2], block[3], block[4], block[5], block[6], block[7],
                        block[8], block[9], block[10], block[11], block[12], block[13], block[14], block[15]);

                return false;
            }

            memcpy(code, block + 6, 10);

            return true;
        }
    }

    return false;
}

/* Is the card really there? The ACR122U keeps reporting the last card it saw (FeliCa
   especially) after it has left, even to a program started later, until the reader sees
   another card or is replugged: "get data" answers from that memory. So the card is
   polled for over the air, through the reader's direct-transmit pseudo-APDU
   (FF 00 00 00 Lc + a PN532 command): InListPassiveTarget, FeliCa 212 kbps or ISO 14443A,
   then InRelease so the reader carries on as before.
   1: the card with this ID answered, 0: nothing (or another card) answered, -1: the
   reader doesn't take direct commands (then "get data" is all there is). */
static int reader_card_answers(ULONG_PTR card, DWORD proto, const BYTE *uid, int n)
{
    static const BYTE poll_felica[] = { 0xFF, 0x00, 0x00, 0x00, 0x09,
                                        0xD4, 0x4A, 0x01, 0x01, 0x00, 0xFF, 0xFF, 0x01, 0x00 };
    static const BYTE poll_a[] = { 0xFF, 0x00, 0x00, 0x00, 0x04, 0xD4, 0x4A, 0x01, 0x00 };
    static const BYTE release[] = { 0xFF, 0x00, 0x00, 0x00, 0x03, 0xD4, 0x52, 0x00 };
    BYTE r[64];
    int len;
    int found = 0;

    len = reader_ok(card, proto, n == 8 ? poll_felica : poll_a,
                    n == 8 ? sizeof(poll_felica) : sizeof(poll_a), r, sizeof(r));

    if (len < 3 || r[0] != 0xD5 || r[1] != 0x4B) {
        return -1;
    }

    if (r[2] >= 1) {
        if (n == 8) {
            /* D5 4B 01 Tg len 01 IDm[8] PMm[8] ... */
            found = len >= 14 && r[5] == 0x01 && memcmp(r + 6, uid, 8) == 0;
        } else {
            /* D5 4B 01 Tg SENS_RES[2] SEL_RES NFCIDLength NFCID... */
            found = len >= 8 && r[7] == n && len >= 8 + n && memcmp(r + 8, uid, (size_t) n) == 0;
        }
    }

    reader_ok(card, proto, release, sizeof(release), NULL, 0);

    return found;
}

/* One look at the reader. 1: a card was read, 0: no card, -1: the reader is gone */
static int reader_poll_once(ULONG_PTR ctx, const char *name)
{
    static const BYTE get_uid[] = { 0xFF, 0xCA, 0x00, 0x00, 0x00 };
    static uint8_t last_uid[10];
    static int last_uid_len;
    static bool last_ok;
    static bool last_felica;
    static uint64_t last_idm;
    static uint8_t last_code[10];
    static uint64_t logged_idm;     /* said once per tap, however often the reader re-reads it */
    ULONG_PTR card;
    DWORD proto = 0;
    BYTE uid[16];
    LONG r;
    int n;

    r = sc.conn(ctx, name, R_SHARE_SHARED, R_PROTOCOL_T0_T1, &card, &proto);

    if (r == R_E_NO_SMARTCARD || r == R_W_REMOVED_CARD || r == R_E_TIMEOUT) {
        last_uid_len = 0;

        return 0;
    }

    if (r != 0) {
        last_uid_len = 0;

        return -1;
    }

    n = reader_ok(card, proto, get_uid, sizeof(get_uid), uid, sizeof(uid));

    if (n <= 0 || n > 10) {
        sc.disc(card, R_LEAVE_CARD);
        last_uid_len = 0;

        return 0;
    }

    {
        static bool warned_direct;
        int there = reader_card_answers(card, proto, uid, n);

        if (there == 0) {
            /* the reader remembers a card that isn't on it */
            sc.disc(card, R_LEAVE_CARD);

            if (last_uid_len != 0 || logged_idm != 0) {
                dprintf("Reader: card removed\n");
            }

            logged_idm = 0;

            last_uid_len = 0;

            return 0;
        }

        if (there < 0 && !warned_direct) {
            dprintf("Reader: this reader doesn't take direct commands; trusting it to say when a card has left\n");
            warned_direct = true;
        }
    }

    if (n == last_uid_len && memcmp(uid, last_uid, (size_t) n) == 0) {
        sc.disc(card, R_LEAVE_CARD);    /* the same card: as read before */
    } else {
        memcpy(last_uid, uid, (size_t) n);
        last_uid_len = n;
        last_ok = false;

        if (n == 8) {
            last_idm = 0;

            for (int i = 0 ; i < 8 ; i++) {
                last_idm = (last_idm << 8) | uid[i];
            }

            last_felica = true;
            last_ok = true;

            if (last_idm != logged_idm) {
                dprintf("Reader: FeliCa card %016llX\n", (unsigned long long) last_idm);
                logged_idm = last_idm;
            }
        } else {
            last_felica = false;
            last_ok = reader_read_mifare(card, proto, last_code);

            if (last_ok) {
                dprintf("Reader: MIFARE Aime, access code "
                        "%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X\n",
                        last_code[0], last_code[1], last_code[2], last_code[3], last_code[4],
                        last_code[5], last_code[6], last_code[7], last_code[8], last_code[9]);
            } else {
                dprintf("Reader: a %d-byte card that isn't FeliCa and whose access code "
                        "couldn't be read; ignored\n", n);
            }
        }

        sc.disc(card, R_LEAVE_CARD);
    }

    EnterCriticalSection(&reader_lock);
    reader_card = last_ok;
    reader_card_felica = last_felica;
    reader_card_idm = last_idm;
    memcpy(reader_card_aime, last_code, sizeof(reader_card_aime));
    LeaveCriticalSection(&reader_lock);

    return 1;
}

/* the reader to use: the configured name, else an ACR122, else the first one */
static bool reader_find(ULONG_PTR ctx, char *out, size_t cap)
{
    char names[2048];
    DWORD len = sizeof(names);
    const char *p;
    const char *pick = NULL;

    if (sc.list(ctx, NULL, names, &len) != 0) {
        return false;
    }

    for (p = names ; *p != '\0' ; p += strlen(p) + 1) {
        if (reader_want[0] != '\0') {
            if (strstr(p, reader_want) != NULL) {
                pick = p;
                break;
            }
        } else if (strstr(p, "ACR122") != NULL) {
            pick = p;
            break;
        } else if (pick == NULL) {
            pick = p;
        }
    }

    if (pick == NULL) {
        return false;
    }

    snprintf(out, cap, "%s", pick);

    return true;
}

static DWORD WINAPI reader_thread(void *arg)
{
    ULONG_PTR ctx = 0;
    char name[256] = "";
    bool have = false;
    bool warned = false;
    int r;

    (void) arg;

    for (;;) {
        if (ctx == 0 && sc.est(R_SCOPE_USER, NULL, NULL, &ctx) != 0) {
            ctx = 0;
            Sleep(2000);
            continue;
        }

        if (!have) {
            have = reader_find(ctx, name, sizeof(name));

            if (have) {
                dprintf("Reader: using \"%s\": tap a card\n", name);
                warned = false;
            } else {
                if (!warned) {
                    dprintf("Reader: no card reader%s%s%s found yet (still looking)\n",
                            reader_want[0] ? " named \"" : "", reader_want, reader_want[0] ? "\"" : "");
                    warned = true;
                }

                sc.rel(ctx);
                ctx = 0;
                Sleep(2000);
                continue;
            }
        }

        r = reader_poll_once(ctx, name);

        if (r < 0) {
            dprintf("Reader: \"%s\" went away\n", name);
            have = false;
            sc.rel(ctx);
            ctx = 0;
        }

        if (r <= 0) {
            EnterCriticalSection(&reader_lock);
            reader_card = false;
            LeaveCriticalSection(&reader_lock);
        }

        Sleep(250);
    }

    return 0;
}

bool divaonline_reader_start(const wchar_t *ini)
{
    wchar_t want[64];
    HMODULE mod;

    GetPrivateProfileStringW(L"aime", L"reader", L"", want, _countof(want), ini);

    if (want[0] == L'\0' || _wcsicmp(want, L"none") == 0 || _wcsicmp(want, L"0") == 0) {
        return false;
    }

    /* "auto", "pcsc" and "acr122u" mean "find one"; anything else is part of a reader's name */
    if (_wcsicmp(want, L"auto") == 0 || _wcsicmp(want, L"pcsc") == 0 || _wcsicmp(want, L"acr122u") == 0
            || _wcsicmp(want, L"1") == 0) {
        reader_want[0] = '\0';
    } else {
        WideCharToMultiByte(CP_ACP, 0, want, -1, reader_want, sizeof(reader_want), NULL, NULL);
    }

    mod = LoadLibraryW(L"winscard.dll");

    if (mod == NULL) {
        dprintf("Reader: winscard.dll isn't there; no card reader\n");

        return false;
    }

    sc.est = (est_t) GetProcAddress(mod, "SCardEstablishContext");
    sc.rel = (rel_t) GetProcAddress(mod, "SCardReleaseContext");
    sc.list = (list_t) GetProcAddress(mod, "SCardListReadersA");
    sc.conn = (conn_t) GetProcAddress(mod, "SCardConnectA");
    sc.disc = (disc_t) GetProcAddress(mod, "SCardDisconnect");
    sc.xmit = (xmit_t) GetProcAddress(mod, "SCardTransmit");

    if (!sc.est || !sc.rel || !sc.list || !sc.conn || !sc.disc || !sc.xmit) {
        dprintf("Reader: winscard.dll is missing functions; no card reader\n");

        return false;
    }

    InitializeCriticalSection(&reader_lock);
    reader_running = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL) != NULL;

    return reader_running;
}

/* The card on the reader, if any: 1 FeliCa (idm), 2 Aime access code (code), 0 none */
int divaonline_reader_card(uint64_t *idm, uint8_t code[10])
{
    int kind = 0;

    if (!reader_running) {
        return 0;
    }

    EnterCriticalSection(&reader_lock);

    if (reader_card) {
        kind = reader_card_felica ? 1 : 2;
        *idm = reader_card_idm;
        memcpy(code, reader_card_aime, 10);
    }

    LeaveCriticalSection(&reader_lock);

    return kind;
}
