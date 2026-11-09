// Reading a stored (uncompressed) zip's index: the central directory, zip64 included, and
// where a member's data starts. No Windows here, so the Linux tests can run it directly
// (tests/zip_index_test.cpp); the file layer (archives.cpp) reads through its ReadAt.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zipidx {
    struct Member {
        std::string name;        // UTF-8 as stored; '\' separators, no trailing one, no leading ".\"
        uint64_t header_offset;  // local file header
        uint64_t size;
        uint16_t dos_date, dos_time;
        bool dir;
    };

    // Reads `size` bytes at `offset` of the archive; false on error or a short read.
    using ReadAt = std::function<bool(uint64_t offset, void* buffer, uint32_t size)>;

    enum class Error {
        None,
        TooSmall,         // under 22 bytes
        ReadError,        // the end of the file couldn't be read
        NotZip,           // no end of central directory record
        BrokenZip64,      // zip64 locator or end record missing or wrong
        OutOfRange,       // the central directory lies outside the file, or is over 512 MB
        DirectoryRead,    // the central directory couldn't be read
        BrokenDirectory,  // a central directory record is cut short or has a bad signature
        NotStored,        // a member is compressed or encrypted (`member`, `encrypted`)
    };

    struct Result {
        Error error = Error::None;
        std::string member;      // NotStored: the member's name
        bool encrypted = false;  // NotStored: encrypted rather than compressed
    };

    // The members of a zip `file_size` bytes long. Names that could escape the folder
    // (absolute, a drive, "..") are left out.
    Result read_central_directory(uint64_t file_size, const ReadAt& read, std::vector<Member>& out);

    // Where a member's data starts, from its local header (the extra field there may differ
    // from the central directory's), or -1 if the header is bad or the data runs past the end.
    int64_t data_offset(const ReadAt& read, uint64_t header_offset, uint64_t size, uint64_t file_size);
}
