#include "zip_index.hpp"
#include <algorithm>

namespace zipidx {
    static uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
    static uint32_t U32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
    static uint64_t U64(const uint8_t* p) { return (uint64_t)U32(p) | (uint64_t)U32(p + 4) << 32; }

    Result read_central_directory(uint64_t file_size, const ReadAt& read, std::vector<Member>& out) {
        Result r;
        if (file_size < 22) {
            r.error = Error::TooSmall;
            return r;
        }
        // End of central directory: in the last 64 KB + 22 bytes
        uint64_t tail_size = std::min<uint64_t>(file_size, 65557 + 20);
        std::vector<uint8_t> tail((size_t)tail_size);
        if (!read(file_size - tail_size, tail.data(), (uint32_t)tail_size)) {
            r.error = Error::ReadError;
            return r;
        }
        int64_t eocd = -1;
        for (int64_t i = (int64_t)tail_size - 22; i >= 0; i--)
            if (U32(&tail[(size_t)i]) == 0x06054b50) {
                eocd = i;
                break;
            }
        if (eocd < 0) {
            r.error = Error::NotZip;
            return r;
        }
        const uint8_t* e = &tail[(size_t)eocd];
        uint64_t count = U16(e + 10), cd_size = U32(e + 12), cd_offset = U32(e + 16);
        if (count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_offset == 0xFFFFFFFF) {
            // Zip64: the locator sits just before the end record
            uint8_t z[56];
            if (eocd < 20 || U32(&tail[(size_t)eocd - 20]) != 0x07064b50
                || !read(U64(&tail[(size_t)eocd - 20 + 8]), z, sizeof(z)) || U32(z) != 0x06064b50) {
                r.error = Error::BrokenZip64;
                return r;
            }
            count = U64(z + 32);
            cd_size = U64(z + 40);
            cd_offset = U64(z + 48);
        }
        if (cd_offset + cd_size > file_size || cd_size > 512ull << 20) {
            r.error = Error::OutOfRange;
            return r;
        }
        std::vector<uint8_t> cd((size_t)cd_size);
        if (cd_size && !read(cd_offset, cd.data(), (uint32_t)cd_size)) {
            r.error = Error::DirectoryRead;
            return r;
        }

        out.clear();
        out.reserve((size_t)std::min<uint64_t>(count, cd_size / 46 + 1));
        size_t p = 0;
        for (uint64_t i = 0; i < count; i++) {
            if (p + 46 > cd.size() || U32(&cd[p]) != 0x02014b50) {
                r.error = Error::BrokenDirectory;
                return r;
            }
            const uint8_t* c = &cd[p];
            uint16_t flags = U16(c + 8), method = U16(c + 10);
            uint64_t csize = U32(c + 20), usize = U32(c + 24), header = U32(c + 42);
            size_t nlen = U16(c + 28), xlen = U16(c + 30), clen = U16(c + 32);
            if (p + 46 + nlen + xlen + clen > cd.size()) {
                r.error = Error::BrokenDirectory;
                return r;
            }
            std::string name((const char*)c + 46, nlen);
            // Zip64 sizes and offset
            for (size_t x = 0; x + 4 <= xlen;) {
                const uint8_t* f = c + 46 + nlen + x;
                uint16_t id = U16(f), len = U16(f + 2);
                if (id == 0x0001) {
                    const uint8_t* v = f + 4;
                    const uint8_t* end = v + len;
                    if (usize == 0xFFFFFFFF && v + 8 <= end) { usize = U64(v); v += 8; }
                    if (csize == 0xFFFFFFFF && v + 8 <= end) { csize = U64(v); v += 8; }
                    if (header == 0xFFFFFFFF && v + 8 <= end) { header = U64(v); v += 8; }
                }
                x += 4 + (size_t)len;
            }
            p += 46 + nlen + xlen + clen;

            bool dir = !name.empty() && name.back() == '/';
            if (!dir && (method != 0 || (flags & 1) || csize != usize)) {
                r.error = Error::NotStored;
                r.member = name;
                r.encrypted = (flags & 1) != 0;
                return r;
            }
            // '/', '\', '.' and ':' never occur inside a UTF-8 multibyte sequence, so these
            // byte-wise edits are the same as on the decoded name.
            std::replace(name.begin(), name.end(), '/', '\\');
            while (!name.empty() && name.back() == '\\')
                name.pop_back();
            while (!name.compare(0, 2, ".\\"))
                name.erase(0, 2);
            if (name.empty() || name[0] == '\\' || name.find(':') != std::string::npos
                || name.find("..\\") != std::string::npos || name == "..")
                continue;
            out.push_back({ name, header, usize, U16(c + 14), U16(c + 12), dir });
        }
        return r;
    }

    int64_t data_offset(const ReadAt& read, uint64_t header_offset, uint64_t size, uint64_t file_size) {
        uint8_t h[30];
        if (!read(header_offset, h, sizeof(h)) || U32(h) != 0x04034b50)
            return -1;
        uint64_t data = header_offset + 30 + U16(h + 26) + U16(h + 28);
        if (data + size > file_size)
            return -1;
        return (int64_t)data;
    }
}
