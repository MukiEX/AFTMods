// Linux tests for the file layer's parts that don't need Windows: zip_index (a zip's central
// directory and members' data offsets) and wildcard (Windows-style pattern matching).
//
//   python3 make_zips.py zips
//   g++ -std=c++17 -O1 -fsanitize=address,undefined units_test.cpp ../vfs/zip_index.cpp ../vfs/wildcard.cpp -o units_test
//   ./units_test zips
#include "../vfs/zip_index.hpp"
#include "../vfs/wildcard.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_fail = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct File {
    FILE* f = nullptr;
    uint64_t size = 0;
    explicit File(const std::string& path) {
        f = fopen(path.c_str(), "rb");
        if (f) { fseek(f, 0, SEEK_END); size = (uint64_t)ftell(f); }
    }
    ~File() { if (f) fclose(f); }
    zipidx::ReadAt reader() {
        return [this](uint64_t offset, void* buffer, uint32_t n) {
            return fseek(f, (long)offset, SEEK_SET) == 0 && fread(buffer, 1, n, f) == n;
        };
    }
    std::string read(uint64_t offset, uint64_t n) {
        std::string s((size_t)n, '\0');
        fseek(f, (long)offset, SEEK_SET);
        if (n && fread(&s[0], 1, (size_t)n, f) != n) s.clear();
        return s;
    }
};

static const zipidx::Member* find(const std::vector<zipidx::Member>& m, const std::string& name) {
    for (const zipidx::Member& x : m)
        if (x.name == name) return &x;
    return nullptr;
}

static void test_stored(const std::string& dir) {
    File f(dir + "/stored.zip");
    std::vector<zipidx::Member> m;
    zipidx::Result r = zipidx::read_central_directory(f.size, f.reader(), m);
    CHECK(r.error == zipidx::Error::None, "stored.zip: error %d", (int)r.error);
    CHECK(m.size() == 5, "stored.zip: %zu members, expected 5", m.size());
    const char* names[] = { "rom\\a.bin", "rom\\sub", "rom\\sub\\b.txt", "c.txt", "rom\\\xE3\x83\x9F\xE3\x82\xAF.txt" };
    const char* data[] = { "0123456789", nullptr, "abc", "see", "utf8" };
    for (int i = 0; i < 5; i++) {
        const zipidx::Member* x = find(m, names[i]);
        CHECK(x, "stored.zip: no %s", names[i]);
        if (!x) continue;
        CHECK(x->dir == !data[i], "%s: dir %d", names[i], x->dir);
        if (!data[i]) continue;
        CHECK(x->size == strlen(data[i]), "%s: size %llu", names[i], (unsigned long long)x->size);
        int64_t off = zipidx::data_offset(f.reader(), x->header_offset, x->size, f.size);
        CHECK(off > 0 && f.read((uint64_t)off, x->size) == data[i], "%s: data at %lld", names[i], (long long)off);
    }
    for (const char* bad : { "..\\evil.txt", "\\abs.txt", "C:\\drive.txt", "evil.txt", "abs.txt", "drive.txt" })
        CHECK(!find(m, bad), "stored.zip: %s should be left out", bad);
    // a data offset that isn't a local header, and one whose data runs past the end
    CHECK(zipidx::data_offset(f.reader(), 1, 1, f.size) < 0, "bad local header accepted");
    if (const zipidx::Member* x = find(m, "rom\\a.bin"))
        CHECK(zipidx::data_offset(f.reader(), x->header_offset, f.size, f.size) < 0, "data past the end accepted");
}

static void test_zip64(const std::string& dir) {
    File f(dir + "/zip64.zip");
    std::vector<zipidx::Member> m;
    zipidx::Result r = zipidx::read_central_directory(f.size, f.reader(), m);
    CHECK(r.error == zipidx::Error::None, "zip64.zip: error %d", (int)r.error);
    CHECK(m.size() == 65540, "zip64.zip: %zu members, expected 65540", m.size());
    if (m.size() == 65540) {
        const zipidx::Member& x = m[65539];
        CHECK(x.name == "f\\65539", "zip64.zip: last is %s", x.name.c_str());
        int64_t off = zipidx::data_offset(f.reader(), x.header_offset, x.size, f.size);
        CHECK(off > 0 && f.read((uint64_t)off, x.size) == "65539", "zip64.zip: last member's data");
    }
}

static void test_errors(const std::string& dir) {
    struct { const char* file; zipidx::Error error; } cases[] = {
        { "deflated.zip", zipidx::Error::NotStored },
        { "encrypted.zip", zipidx::Error::NotStored },
        { "broken.zip", zipidx::Error::BrokenDirectory },
        { "notzip.bin", zipidx::Error::NotZip },
        { "tiny.bin", zipidx::Error::TooSmall },
    };
    for (auto& c : cases) {
        File f(dir + "/" + c.file);
        std::vector<zipidx::Member> m;
        zipidx::Result r = zipidx::read_central_directory(f.size, f.reader(), m);
        CHECK(r.error == c.error, "%s: error %d, expected %d", c.file, (int)r.error, (int)c.error);
        if (!strcmp(c.file, "deflated.zip"))
            CHECK(r.member == "rom/packed.bin" && !r.encrypted, "deflated.zip: %s encrypted=%d", r.member.c_str(), r.encrypted);
        if (!strcmp(c.file, "encrypted.zip"))
            CHECK(r.encrypted, "encrypted.zip: not reported as encrypted");
    }
    // a reader that fails
    std::vector<zipidx::Member> m;
    zipidx::Result r = zipidx::read_central_directory(1000, [](uint64_t, void*, uint32_t) { return false; }, m);
    CHECK(r.error == zipidx::Error::ReadError, "failing reader: error %d", (int)r.error);
}

static void test_wildcard() {
    struct { const wchar_t* p; const wchar_t* s; bool match; } cases[] = {
        { L"*", L"anything.bin", true }, { L"*.*", L"noext", true }, { L"*.*", L"a.b", true },
        { L"*.farc", L"OBJ_001.FARC", true }, { L"*.farc", L"obj.farc.bak", false },
        { L"pv_???.dsc", L"PV_001.DSC", true }, { L"pv_???.dsc", L"pv_01.dsc", false },
        { L"a*b*c", L"aXXbYYc", true }, { L"a*b*c", L"aXXcYYb", false },
        { L"*_param*", L"Skin_Param.txt", true }, { L"?", L"", false }, { L"*", L"", true },
        { L"mzko", L"MZKO", true }, { L"mzko", L"MZK", false },
    };
    for (auto& c : cases)
        CHECK(vfs::Match(c.p, c.s) == c.match, "Match(%ls, %ls) should be %d", c.p, c.s, c.match);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: units_test <zips folder from make_zips.py>\n"); return 2; }
    test_stored(argv[1]);
    test_zip64(argv[1]);
    test_errors(argv[1]);
    test_wildcard();
    printf("%d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
