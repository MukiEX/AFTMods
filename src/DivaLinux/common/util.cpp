#include "util.hpp"
#include <cstdarg>
#include <cstdio>
#include <cwctype>

extern "C" void divavk_log_text(const char* text); // vk_bridge.cpp

namespace dl {
    double now_ms() {
        static LARGE_INTEGER freq = {};
        if (!freq.QuadPart)
            QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart * 1000.0 / freq.QuadPart;
    }

    void log(const char* format, ...) {
        char buf[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        divavk_log_text(buf);
    }

    std::string narrow(const std::wstring& w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    }

    std::wstring widen_acp(const char* s) {
        if (!s)
            return std::wstring();
        int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1)
            MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
        return w;
    }

    void lower(std::wstring& s) {
        for (wchar_t& c : s)
            c = (wchar_t)towlower(c);
    }
}
