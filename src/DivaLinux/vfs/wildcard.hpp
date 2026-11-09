// Windows-style wildcard matching for searches (FindFirstFileEx patterns). No Windows here, so
// the Linux tests run it directly (tests/units_test.cpp).
#pragma once

namespace vfs {
    // '*' any run, '?' one character, ignoring case ("*.*" is "*"). `p` must be lower case.
    bool Match(const wchar_t* p, const wchar_t* s);
}
