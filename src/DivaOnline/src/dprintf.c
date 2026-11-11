/* DivaOnline's dprintf: the vendored segatools code logs through these.
   Lines go to PD Loader's console (stdout), the debugger (OutputDebugString)
   and, when [divaonline] log_file is set, a log file. Each line gets a
   "[DivaOnline] " prefix, like the other PD Loader plugins' messages. */

#include <windows.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "util/dprintf.h"
#include "divaonline.h"

static CRITICAL_SECTION dprintf_lock;
static bool dprintf_lock_ready;
static bool dprintf_at_line_start = true;
static FILE *dprintf_file;

void divaonline_log_init(const wchar_t *log_path)
{
    if (!dprintf_lock_ready) {
        InitializeCriticalSection(&dprintf_lock);
        dprintf_lock_ready = true;
    }

    if (log_path != NULL && log_path[0] != L'\0' && dprintf_file == NULL) {
        dprintf_file = _wfopen(log_path, L"w");
    }
}

static void dprintf_emit(const char *text)
{
    const char *p;
    const char *nl;
    char line[1100];
    size_t n;

    for (p = text ; *p != '\0' ; p = nl) {
        nl = strchr(p, '\n');
        nl = nl != NULL ? nl + 1 : p + strlen(p);
        n = (size_t) (nl - p);

        if (dprintf_at_line_start) {
            fputs("[DivaOnline] ", stdout);
            OutputDebugStringA("[DivaOnline] ");

            if (dprintf_file != NULL) {
                fputs("[DivaOnline] ", dprintf_file);
            }
        }

        if (n >= sizeof(line)) {
            n = sizeof(line) - 1;
        }

        memcpy(line, p, n);
        line[n] = '\0';

        fputs(line, stdout);
        OutputDebugStringA(line);

        if (dprintf_file != NULL) {
            fputs(line, dprintf_file);
        }

        dprintf_at_line_start = n > 0 && line[n - 1] == '\n';
    }

    fflush(stdout);

    if (dprintf_file != NULL) {
        fflush(dprintf_file);
    }
}

void dprintfv(const char *fmt, va_list ap)
{
    char buf[1024];

    vsnprintf(buf, sizeof(buf), fmt, ap);
    buf[sizeof(buf) - 1] = '\0';

    if (dprintf_lock_ready) {
        EnterCriticalSection(&dprintf_lock);
    }

    dprintf_emit(buf);

    if (dprintf_lock_ready) {
        LeaveCriticalSection(&dprintf_lock);
    }
}

void dprintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    dprintfv(fmt, ap);
    va_end(ap);
}

void dwprintfv(const wchar_t *fmt, va_list ap)
{
    wchar_t wbuf[1024];
    char buf[1024 * 3];

    _vsnwprintf(wbuf, _countof(wbuf), fmt, ap);
    wbuf[_countof(wbuf) - 1] = L'\0';

    if (WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), NULL, NULL) == 0) {
        buf[0] = '\0';
    }

    if (dprintf_lock_ready) {
        EnterCriticalSection(&dprintf_lock);
    }

    dprintf_emit(buf);

    if (dprintf_lock_ready) {
        LeaveCriticalSection(&dprintf_lock);
    }
}

void dwprintf(const wchar_t *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    dwprintfv(fmt, ap);
    va_end(ap);
}
