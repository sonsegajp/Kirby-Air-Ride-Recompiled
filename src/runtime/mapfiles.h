#pragma once
// City Trial map mods as files (src/runtime/mapfiles.cpp): each in a folder of its own under mods/maps beside the
// program (src/runtime/maps.h serves the one chosen to the game). What an online session names a map by - the hash of
// its content - and a map packed for sending to a console without it, then unpacked and checked there (airnet's map
// downloads, docs/ONLINE.md "Maps").
//
// Files only, nothing of the game's: every function may be called from any thread. Hashing and packing read every
// file of a map (tens of megabytes): never on the game's thread or the UI's while a hash is not cached.
#include <cstdint>
#include <string>
#include <vector>

namespace rt::mapfiles {

inline constexpr uint64_t kBytesMax = 512ull << 20;  // a map's files, at most (what a console sends or takes)
inline constexpr int kFilesMax = 1024;

struct Map {
    std::string folder;  // its folder's name (in the maps folder)
    std::string title;   // its mod.json's "name" (else the folder's name)
    std::string hash;    // its content's hash (hex), when asked for
    uint64_t size = 0;   // its files' bytes, likewise
};

std::string defaultRoot();  // "mods/maps": the maps folder (of the working folder, the program's)
std::string dirOf(const std::string& root, const std::string& folder);

// The map mods in `root` (folders with a GrCity1.dat), by title; `hashes`: each one's hash and size too.
std::vector<Map> scan(const std::string& root, bool hashes);

// A map's content hash: SHA-256 over its files (every one, in its folders too) in the order of their paths (relative,
// '/' between folders, compared byte by byte) - each its path (UTF-8), a 0 byte, its size (u64, big-endian), its bytes.
// Remembered per folder while its files' sizes and times stay the same. False: the folder cannot be read.
bool folderHash(const std::string& dir, std::string& hash, uint64_t& size);

// The installed map with this content: the folder `folder` (or the name it would be downloaded as) if its hash is
// `hash`, else any folder whose hash is. Its folder's name; empty: none.
std::string find(const std::string& root, const std::string& folder, const std::string& hash);

// A folder's name as a download keeps it: its letters and digits (none: "Map" and the start of the hash).
std::string cleanFolder(const std::string& folder, const std::string& hash);

// A map packed for sending: a manifest - "karmap 1", then a line per file: its size, its SHA-256 (hex), its path -
// then every file's bytes in the manifest's order. `hash`: the content's hash, as folderHash() has it.
bool pack(const std::string& dir, std::vector<uint8_t>& out, uint32_t& manifestBytes, std::string& hash);

// A packed map unpacked into root/.dl-<cleanFolder>, every file and then the whole content checked against `hash`,
// then put in place as root/<cleanFolder> (a folder already there of that name with other content replaced). False:
// `error` says why, for the player. `folderOut`: the folder's name.
bool install(const std::string& root, const std::string& folder, const std::string& hash, const std::vector<uint8_t>& packed,
             uint32_t manifestBytes, std::string& error, std::string* folderOut = nullptr);

}  // namespace rt::mapfiles
