// City Trial map mods as files: their content hash, and a map packed for a console without it and unpacked there
// (src/runtime/mapfiles.h). Files only - no game, no window: the online client's tests build it too.
#include "runtime/mapfiles.h"
#include "runtime/airnet.h"  // (SHA-256)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>
#include <tuple>

namespace rt {
void log(const char* fmt, ...);  // runtime/runtime.h (not included: it brings the guest CPU headers)
}

namespace rt::mapfiles {

namespace fs = std::filesystem;

namespace {

fs::path pathOf(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::string genericOf(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::string hexOf(const uint8_t* p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; i++) s[2 * i] = digits[p[i] >> 4], s[2 * i + 1] = digits[p[i] & 15];
    return s;
}

bool isHash(const std::string& h) {
    if (h.size() != 64) return false;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// A folder's name that stays in the maps folder (none of '/', '\', ':'; not "." or "..").
bool plainName(const std::string& s) {
    if (s.empty() || s == "." || s == ".." || s.size() > 255) return false;
    for (char c : s)
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
    return true;
}

// A path in a packed map: folders and a file below the map's folder, each a plain name not starting with '.'.
bool safePath(const std::string& rel) {
    if (rel.empty() || rel.size() > 240) return false;
    size_t start = 0;
    int depth = 0;
    for (;;) {
        const size_t slash = rel.find('/', start);
        const std::string part = rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!plainName(part) || part[0] == '.' || part.back() == ' ' || ++depth > 8) return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

struct File {
    std::string rel;  // its path in the map's folder ('/' between folders)
    fs::path full;
    uint64_t size = 0;
    int64_t time = 0;
};

// A folder's files (in its folders too), in the order of their paths. False: it cannot be read.
bool listFiles(const fs::path& dir, std::vector<File>& out) {
    out.clear();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return false;
    for (; it != end; it.increment(ec)) {
        if (ec) return false;
        const fs::directory_entry& e = *it;
        if (!e.is_regular_file(ec)) continue;
        File f;
        f.full = e.path();
        f.rel = genericOf(e.path().lexically_relative(dir));
        f.size = e.file_size(ec);
        if (ec) return false;
        f.time = int64_t(e.last_write_time(ec).time_since_epoch().count());
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const File& a, const File& b) { return a.rel < b.rel; });
    return true;
}

bool readFile(const File& f, std::vector<uint8_t>& out, size_t at) {
    std::ifstream in(f.full, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(out.data() + at), std::streamsize(f.size));
    return uint64_t(in.gcount()) == f.size;
}

// A file's part of the content hash before its bytes: its path, a 0 byte, its size.
void hashHead(airnet::detail::Sha256& s, const std::string& rel, uint64_t size) {
    uint8_t head[9] = {0};
    for (int i = 0; i < 8; i++) head[1 + i] = uint8_t(size >> (56 - 8 * i));
    s.update(rel.data(), rel.size());
    s.update(head, sizeof head);
}

bool hashFiles(const std::vector<File>& files, std::string& hash, uint64_t& size) {
    airnet::detail::Sha256 s;
    std::vector<char> buf(1 << 20);
    size = 0;
    for (const File& f : files) {
        hashHead(s, f.rel, f.size);
        std::ifstream in(f.full, std::ios::binary);
        if (!in) return false;
        for (uint64_t left = f.size; left;) {
            in.read(buf.data(), std::streamsize(std::min<uint64_t>(left, buf.size())));
            const std::streamsize got = in.gcount();
            if (got <= 0) return false;
            s.update(buf.data(), size_t(got));
            left -= uint64_t(got);
        }
        size += f.size;
    }
    uint8_t out[32];
    s.final(out);
    hash = hexOf(out, 32);
    return true;
}

// The hashes worked out, by folder: kept while every file's size and time stay the same.
struct Cached {
    std::vector<std::tuple<std::string, uint64_t, int64_t>> stamp;
    std::string hash;
    uint64_t size = 0;
};
std::mutex s_mu;
std::map<std::string, Cached> s_cache;

std::string modTitle(const fs::path& dir, const std::string& fallback) {
    std::ifstream in(dir / "mod.json", std::ios::binary);
    if (!in) return fallback;
    std::string text(4096, ' ');
    in.read(text.data(), std::streamsize(text.size()));
    text.resize(size_t(std::max<std::streamsize>(0, in.gcount())));
    const size_t key = text.find("\"name\"");
    if (key == std::string::npos) return fallback;
    const size_t a = text.find('"', text.find(':', key));
    const size_t b = a == std::string::npos ? a : text.find('"', a + 1);
    return b == std::string::npos || b == a + 1 ? fallback : text.substr(a + 1, b - a - 1);
}

}  // namespace

std::string defaultRoot() { return "mods/maps"; }

std::string dirOf(const std::string& root, const std::string& folder) { return utf8Of(pathOf(root) / pathOf(folder)); }

bool folderHash(const std::string& dir, std::string& hash, uint64_t& size) {
    std::vector<File> files;
    if (!listFiles(pathOf(dir), files)) return false;
    Cached c;
    for (const File& f : files) c.stamp.emplace_back(f.rel, f.size, f.time);
    {
        std::lock_guard<std::mutex> lock(s_mu);
        const auto it = s_cache.find(dir);
        if (it != s_cache.end() && it->second.stamp == c.stamp) {
            hash = it->second.hash, size = it->second.size;
            return true;
        }
    }
    if (!hashFiles(files, c.hash, c.size)) return false;
    hash = c.hash, size = c.size;
    std::lock_guard<std::mutex> lock(s_mu);
    s_cache[dir] = std::move(c);
    return true;
}

std::vector<Map> scan(const std::string& root, bool hashes) {
    std::vector<Map> out;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(pathOf(root), ec)) {
        if (!e.is_directory(ec) || !fs::exists(e.path() / "GrCity1.dat", ec)) continue;
        Map m;
        m.folder = utf8Of(e.path().filename());
        if (m.folder.empty() || m.folder[0] == '.') continue;  // (a download being put in place, and the like)
        m.title = modTitle(e.path(), m.folder);
        if (hashes && !folderHash(utf8Of(e.path()), m.hash, m.size)) continue;
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const Map& a, const Map& b) { return a.title < b.title; });
    return out;
}

std::string cleanFolder(const std::string& folder, const std::string& hash) {
    std::string out;
    for (char c : folder)
        if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) && out.size() < 48) out += c;
    return out.empty() ? "Map" + hash.substr(0, 8) : out;
}

std::string find(const std::string& root, const std::string& folder, const std::string& hash) {
    if (!isHash(hash)) return {};
    std::error_code ec;
    for (const std::string& f : {folder, cleanFolder(folder, hash)}) {
        std::string h;
        uint64_t n = 0;
        if (plainName(f) && fs::exists(pathOf(dirOf(root, f)) / "GrCity1.dat", ec) && folderHash(dirOf(root, f), h, n) && h == hash) return f;
    }
    for (const Map& m : scan(root, true))
        if (m.hash == hash) return m.folder;
    return {};
}

bool pack(const std::string& dir, std::vector<uint8_t>& out, uint32_t& manifestBytes, std::string& hash) {
    std::vector<File> files;
    if (!listFiles(pathOf(dir), files) || files.empty() || files.size() > size_t(kFilesMax)) return false;
    // the manifest first (its size known from the list), then each file read once: hashed, and in place after it
    std::string manifest = "karmap 1\n";
    uint64_t total = 0;
    size_t manifestSize = manifest.size();
    for (const File& f : files) {
        if (!safePath(f.rel)) return false;
        total += f.size;
        manifestSize += std::to_string(f.size).size() + 1 + 64 + 1 + f.rel.size() + 1;
    }
    if (total > kBytesMax) return false;
    out.assign(manifestSize + size_t(total), 0);
    airnet::detail::Sha256 all;
    size_t at = manifestSize;
    for (const File& f : files) {
        if (!readFile(f, out, at)) return false;
        uint8_t sum[32];
        airnet::detail::sha256(out.data() + at, size_t(f.size), sum);
        manifest += std::to_string(f.size) + " " + hexOf(sum, 32) + " " + f.rel + "\n";
        hashHead(all, f.rel, f.size);
        all.update(out.data() + at, size_t(f.size));
        at += size_t(f.size);
    }
    if (manifest.size() != manifestSize) return false;
    std::memcpy(out.data(), manifest.data(), manifest.size());
    manifestBytes = uint32_t(manifestSize);
    uint8_t sum[32];
    all.final(sum);
    hash = hexOf(sum, 32);
    return true;
}

bool install(const std::string& root, const std::string& folder, const std::string& hash, const std::vector<uint8_t>& packed,
             uint32_t manifestBytes, std::string& error, std::string* folderOut) {
    auto bad = [&](const char* why) {
        error = why;
        log("[maps] download of %s: %s\n", folder.c_str(), why);
        return false;
    };
    if (!isHash(hash) || manifestBytes < 9 || manifestBytes > packed.size()) return bad("The map that came is not whole.");
    // the manifest: every line checked, the files' sizes adding up to what came after it
    const std::string manifest(reinterpret_cast<const char*>(packed.data()), manifestBytes);
    if (manifest.compare(0, 9, "karmap 1\n") != 0) return bad("The map that came is not one this game reads.");
    struct Entry {
        std::string rel, sum;
        uint64_t size = 0, at = 0;
    };
    std::vector<Entry> entries;
    uint64_t at = manifestBytes;
    bool hasCity = false;
    for (size_t p = 9; p < manifest.size();) {
        const size_t nl = manifest.find('\n', p);
        if (nl == std::string::npos) return bad("The map that came is not whole.");
        const std::string line = manifest.substr(p, nl - p);
        p = nl + 1;
        const size_t s1 = line.find(' '), s2 = s1 == std::string::npos ? s1 : line.find(' ', s1 + 1);
        if (s2 == std::string::npos || s1 == 0 || s1 > 12 || s2 - s1 - 1 != 64) return bad("The map that came is not one this game reads.");
        Entry e;
        for (size_t k = 0; k < s1; k++) {
            if (line[k] < '0' || line[k] > '9') return bad("The map that came is not one this game reads.");
            e.size = e.size * 10 + uint64_t(line[k] - '0');
        }
        e.sum = line.substr(s1 + 1, 64);
        e.rel = line.substr(s2 + 1);
        if (!isHash(e.sum) || !safePath(e.rel) || (!entries.empty() && !(entries.back().rel < e.rel)) || entries.size() >= size_t(kFilesMax))
            return bad("The map that came is not one this game reads.");
        e.at = at;
        at += e.size;
        if (at > packed.size()) return bad("The map that came is not whole.");
        hasCity = hasCity || e.rel == "GrCity1.dat";
        entries.push_back(std::move(e));
    }
    if (at != packed.size() || !hasCity) return bad("The map that came is not whole.");
    for (const Entry& e : entries) {
        uint8_t sum[32];
        airnet::detail::sha256(packed.data() + e.at, size_t(e.size), sum);
        if (hexOf(sum, 32) != e.sum) return bad("The map's files did not arrive intact.");
    }
    // written beside the maps, checked as written, then put in place
    const std::string name = cleanFolder(folder, hash);
    const fs::path base = pathOf(root), tmp = base / (".dl-" + name), dest = base / name, old = base / (".old-" + name);
    std::error_code ec;
    fs::create_directories(base, ec);
    fs::remove_all(tmp, ec);
    for (const Entry& e : entries) {
        const fs::path to = tmp / pathOf(e.rel);
        fs::create_directories(to.parent_path(), ec);
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        if (out) out.write(reinterpret_cast<const char*>(packed.data() + e.at), std::streamsize(e.size));
        if (!out) {
            out.close();
            fs::remove_all(tmp, ec);
            return bad("Could not save the map (is the disk full?).");
        }
    }
    std::string got;
    uint64_t size = 0;
    if (!folderHash(utf8Of(tmp), got, size) || got != hash) {
        fs::remove_all(tmp, ec);
        return bad("The map's files did not check out.");
    }
    if (fs::exists(dest, ec)) {
        std::string there;
        if (folderHash(utf8Of(dest), there, size) && there == hash) {  // (the same map, there already)
            fs::remove_all(tmp, ec);
            if (folderOut) *folderOut = name;
            return true;
        }
        fs::remove_all(old, ec);
        fs::rename(dest, old, ec);
        if (ec) {
            fs::remove_all(tmp, ec);
            return bad("Could not replace the older map of that name (is it open?).");
        }
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        std::error_code ec2;
        if (fs::exists(old, ec2)) fs::rename(old, dest, ec2);
        fs::remove_all(tmp, ec2);
        return bad("Could not put the map in the maps folder.");
    }
    fs::remove_all(old, ec);
    {  // (its hash kept for its new place: the files are the same)
        std::lock_guard<std::mutex> lock(s_mu);
        const auto it = s_cache.find(utf8Of(tmp));
        if (it != s_cache.end()) s_cache[utf8Of(dest)] = it->second, s_cache.erase(utf8Of(tmp));
    }
    if (folderOut) *folderOut = name;
    log("[maps] downloaded %s: %zu files, %llu bytes, in %s\n", folder.c_str(), entries.size(),
        static_cast<unsigned long long>(packed.size() - manifestBytes), utf8Of(dest).c_str());
    return true;
}

}  // namespace rt::mapfiles
