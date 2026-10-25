// A GameCube disc image read as the plain disc it holds, whatever its file format:
//   .iso / .gcm   the disc as is
//   .ciso         a block map (0x8000-byte header: "CISO", block size, one used/unused byte per
//                 block); unused blocks read as zeros
//   .rvz / .wia   Dolphin's compressed formats: the disc in chunk groups, each compressed on its
//                 own (Zstandard, or stored), RVZ's pseudorandom padding stored as its seeds and
//                 regenerated (lagged Fibonacci, as GC/Wii mastering made it). Format details from
//                 Dolphin's docs/WiaAndRvz.md. GameCube discs only (no Wii partitions); WIA's
//                 bzip2/LZMA chunks are not supported.
// Thread-safe: reads from several threads take turns.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace disc {

class Image {
public:
    virtual ~Image() = default;
    virtual bool read(uint64_t offset, void* dst, size_t size) = 0;  // false past the end or on a bad file
    virtual uint64_t size() const = 0;
    virtual const char* format() const = 0;  // "ISO", "CISO", "RVZ", "WIA"
};

// The image in that file, or null with a reason in `error`.
std::unique_ptr<Image> open(const std::filesystem::path& path, std::string& error);

}  // namespace disc
