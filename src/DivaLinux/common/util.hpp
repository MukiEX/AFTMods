// DivaLinux: small helpers shared by its features (logging, timing, text).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace dl {
    // Milliseconds from the performance counter.
    double now_ms();
    // printf-style line to DivaLinux.log (vk_bridge.cpp's divavk_log_text).
    void log(const char* format, ...);
    // UTF-16 to UTF-8.
    std::string narrow(const std::wstring& w);
    // ANSI code page (what the A functions take) to UTF-16; empty for null.
    std::wstring widen_acp(const char* s);
    // Lower case, in place.
    void lower(std::wstring& s);
}
