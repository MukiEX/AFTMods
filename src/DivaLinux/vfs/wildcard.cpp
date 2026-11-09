#include "wildcard.hpp"
#include <cwchar>
#include <cwctype>

namespace vfs {
    // Windows-style match: '*' any run, '?' one character, ignoring case ("*.*" is "*").
    bool Match(const wchar_t* p, const wchar_t* s) {
        if (!wcscmp(p, L"*.*"))
            p = L"*";
        const wchar_t *star = nullptr, *ss = nullptr;
        while (*s) {
            if (*p == L'?' || *p == (wchar_t)towlower(*s)) {
                p++;
                s++;
            }
            else if (*p == L'*') {
                star = p++;
                ss = s;
            }
            else if (star) {
                p = star + 1;
                s = ++ss;
            }
            else
                return false;
        }
        while (*p == L'*')
            p++;
        return !*p;
    }
}
