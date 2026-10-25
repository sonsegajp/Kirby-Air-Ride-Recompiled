#include "disc/discimage.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <vector>

namespace disc {

namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

class File {
public:
    bool open(const std::filesystem::path& p) {
#ifdef _WIN32
        f_ = _wfopen(p.wstring().c_str(), L"rb");
#else
        f_ = std::fopen(p.string().c_str(), "rb");
#endif
        return f_ != nullptr;
    }
    ~File() {
        if (f_) std::fclose(f_);
    }
    bool read(uint64_t off, void* dst, size_t n) {
#ifdef _WIN32
        if (_fseeki64(f_, int64_t(off), SEEK_SET)) return false;
#else
        if (fseeko(f_, off_t(off), SEEK_SET)) return false;
#endif
        return std::fread(dst, 1, n, f_) == n;
    }
    uint64_t length() {
#ifdef _WIN32
        _fseeki64(f_, 0, SEEK_END);
        return uint64_t(_ftelli64(f_));
#else
        fseeko(f_, 0, SEEK_END);
        return uint64_t(ftello(f_));
#endif
    }

private:
    std::FILE* f_ = nullptr;
};

// ---- .iso / .gcm ----
class Iso final : public Image {
public:
    explicit Iso(std::unique_ptr<File> f) : f_(std::move(f)), size_(f_->length()) {}
    bool read(uint64_t off, void* dst, size_t n) override {
        std::lock_guard g(m_);
        return off + n <= size_ && f_->read(off, dst, n);
    }
    uint64_t size() const override { return size_; }
    const char* format() const override { return "ISO"; }

private:
    std::mutex m_;
    std::unique_ptr<File> f_;
    uint64_t size_;
};

// ---- .ciso ----
class Ciso final : public Image {
public:
    bool init(std::unique_ptr<File> f, std::string& err) {
        f_ = std::move(f);
        std::vector<uint8_t> h(0x8000);
        if (!f_->read(0, h.data(), h.size())) return err = "too short for a CISO file", false;
        block_ = uint32_t(h[4]) | uint32_t(h[5]) << 8 | uint32_t(h[6]) << 16 | uint32_t(h[7]) << 24;
        if (block_ == 0 || block_ > (64u << 20)) return err = "bad CISO block size", false;
        uint32_t n = 0;
        int64_t last = -1;
        for (size_t i = 8; i < h.size(); i++) {
            index_.push_back(h[i] == 1 ? int64_t(n++) : -1);
            if (h[i] == 1) last = int64_t(i - 8);
        }
        size_ = uint64_t(last + 1) * block_;
        return true;
    }
    bool read(uint64_t off, void* dst, size_t n) override {
        std::lock_guard g(m_);
        auto* out = static_cast<uint8_t*>(dst);
        if (off + n > size_) return false;
        while (n) {
            const uint64_t b = off / block_, inner = off % block_;
            const size_t k = size_t(std::min<uint64_t>(n, block_ - inner));
            const int64_t at = b < index_.size() ? index_[size_t(b)] : -1;
            if (at < 0) std::memset(out, 0, k);
            else if (!f_->read(0x8000 + uint64_t(at) * block_ + inner, out, k)) return false;
            off += k, out += k, n -= k;
        }
        return true;
    }
    uint64_t size() const override { return size_; }
    const char* format() const override { return "CISO"; }

private:
    std::mutex m_;
    std::unique_ptr<File> f_;
    uint32_t block_ = 0;
    uint64_t size_ = 0;
    std::vector<int64_t> index_;  // file block of each disc block, -1 unused
};

// ---- the GC/Wii padding generator (lagged Fibonacci, xor, j = 32, k = 521) ----
// From a 17-word seed, starting `offset` bytes into its 32 KiB block (RVZ packing).
void padding(const uint8_t seed[68], uint64_t offset, uint8_t* out, size_t count) {
    std::array<uint32_t, 521> b{};
    for (int i = 0; i < 17; i++) b[size_t(i)] = be32(seed + i * 4);
    for (size_t i = 17; i < 521; i++) b[i] = (b[i - 17] << 23) ^ (b[i - 16] >> 9) ^ b[i - 1];
    auto forward = [&] {
        for (size_t i = 0; i < 32; i++) b[i] ^= b[i + 521 - 32];
        for (size_t i = 32; i < 521; i++) b[i] ^= b[i - 32];
    };
    for (int i = 0; i < 4; i++) forward();
    size_t pos = size_t(offset % 0x8000);  // in output bytes
    while (pos >= 521 * 4) forward(), pos -= 521 * 4;
    while (count) {
        const uint32_t w = b[pos / 4];
        const uint8_t bytes[4] = {uint8_t(w >> 24), uint8_t(w >> 18), uint8_t(w >> 8), uint8_t(w)};  // (>> 18: as mastered)
        *out++ = bytes[pos % 4];
        count--;
        if (++pos == 521 * 4) forward(), pos = 0;
    }
}

// ---- .rvz / .wia ----
class Rvz final : public Image {
public:
    bool init(std::unique_ptr<File> f, std::string& err) {
        f_ = std::move(f);
        uint8_t h[0x48];
        if (!f_->read(0, h, sizeof h)) return err = "too short for an RVZ file", false;
        rvz_ = std::memcmp(h, "RVZ\x01", 4) == 0;
        const uint32_t discSize = be32(h + 12);
        size_ = be64(h + 0x24);
        std::vector<uint8_t> d(std::max<uint32_t>(discSize, 0xDC));
        if (discSize < 0xD4 || !f_->read(0x48, d.data(), discSize)) return err = "damaged RVZ header", false;
        comp_ = be32(&d[4]);
        chunk_ = be32(&d[12]);
        std::memcpy(dhead_, &d[16], 0x80);
        if (be32(&d[0x90])) return err = "a Wii disc (only GameCube discs are supported)", false;
        if (comp_ != 0 && comp_ != 5) return err = "compressed with bzip2/LZMA (convert it to RVZ with Zstandard, or ISO, in Dolphin)", false;
        if (chunk_ < 0x8000) return err = "bad RVZ chunk size", false;
        const uint32_t nRaw = be32(&d[0xB4]), rawSize = be32(&d[0xC0]), nGroups = be32(&d[0xC4]), groupSize = be32(&d[0xD0]);
        const uint64_t rawOff = be64(&d[0xB8]), groupOff = be64(&d[0xC8]);
        std::vector<uint8_t> raw, groups;
        if (!table(rawOff, rawSize, size_t(nRaw) * 24, raw) || !table(groupOff, groupSize, size_t(nGroups) * (rvz_ ? 12 : 8), groups))
            return err = "damaged RVZ tables", false;
        for (uint32_t i = 0; i < nRaw; i++) {
            const uint8_t* r = &raw[size_t(i) * 24];
            const uint64_t off = be64(r), skipped = off % 0x8000;  // rounded down to 32 KiB (format notes)
            raws_.push_back({off - skipped, be64(r + 8) + skipped, be32(r + 16)});
        }
        for (uint32_t i = 0; i < nGroups; i++) {
            const uint8_t* g = &groups[size_t(i) * (rvz_ ? 12 : 8)];
            groups_.push_back({be32(g), be32(g + 4), rvz_ ? be32(g + 8) : 0u});
        }
        return true;
    }
    bool read(uint64_t off, void* dst, size_t n) override {
        std::lock_guard g(m_);
        auto* out = static_cast<uint8_t*>(dst);
        if (off + n > size_) return false;
        const uint64_t start = off;
        while (n) {
            size_t k = 0;
            bool done = false;
            for (const Raw& r : raws_) {
                if (off < r.start || off >= r.start + r.length) continue;
                const uint64_t gi = (off - r.start) / chunk_, inner = (off - r.start) % chunk_;
                const uint64_t gStart = r.start + gi * chunk_;
                const size_t gLen = size_t(std::min<uint64_t>(chunk_, r.start + r.length - gStart));
                const std::vector<uint8_t>* data = group(r.group + uint32_t(gi), gStart, gLen);
                if (!data) return false;
                k = size_t(std::min<uint64_t>(n, gLen - inner));
                std::memcpy(out, data->data() + inner, k);
                done = true;
                break;
            }
            if (!done) {  // not covered by the image: zeros up to the next covered part
                uint64_t next = off + n;
                for (const Raw& r : raws_)
                    if (r.start > off) next = std::min(next, r.start);
                k = size_t(next - off);
                std::memset(out, 0, k);
            }
            off += k, out += k, n -= k;
        }
        // the first 0x80 bytes come from the header
        if (start < 0x80) std::memcpy(static_cast<uint8_t*>(dst), dhead_ + start, size_t(std::min<uint64_t>(0x80 - start, off - start)));
        return true;
    }
    uint64_t size() const override { return size_; }
    const char* format() const override { return rvz_ ? "RVZ" : "WIA"; }

private:
    struct Raw {
        uint64_t start, length;
        uint32_t group;
    };
    struct Group {
        uint32_t off4, size, packed;
    };
    bool unpack(const uint8_t* p, size_t len, uint64_t discOff, std::vector<uint8_t>& out, size_t want) {
        out.clear();
        size_t i = 0;
        while (i + 4 <= len) {
            uint32_t sz = be32(p + i);
            i += 4;
            if (sz & 0x80000000) {
                sz &= 0x7FFFFFFF;
                if (i + 68 > len) return false;
                const size_t at = out.size();
                out.resize(at + sz);
                padding(p + i, discOff + at, out.data() + at, sz);
                i += 68;
            } else {
                if (i + sz > len) return false;
                out.insert(out.end(), p + i, p + i + sz);
                i += sz;
            }
        }
        out.resize(want, 0);
        return true;
    }
    bool table(uint64_t off, uint32_t size, size_t want, std::vector<uint8_t>& out) {
        std::vector<uint8_t> in(size);
        if (!f_->read(off, in.data(), size)) return false;
        if (comp_ == 0) return out = std::move(in), out.size() >= want;
        out.resize(want);
        const size_t got = ZSTD_decompress(out.data(), want, in.data(), in.size());
        return !ZSTD_isError(got) && got >= want;
    }
    const std::vector<uint8_t>* group(uint32_t gi, uint64_t discOff, size_t len) {
        for (auto it = cache_.begin(); it != cache_.end(); ++it)
            if (it->first == gi) {
                cache_.splice(cache_.begin(), cache_, it);
                return &cache_.front().second;
            }
        if (gi >= groups_.size()) return nullptr;
        const Group& g = groups_[gi];
        std::vector<uint8_t> data;
        const uint32_t stored = g.size & 0x7FFFFFFF;
        const bool compressed = rvz_ ? (g.size & 0x80000000) != 0 : comp_ != 0;
        if (stored == 0) {
            data.assign(len, 0);
        } else {
            std::vector<uint8_t> in(stored);
            if (!f_->read(uint64_t(g.off4) * 4, in.data(), stored)) return nullptr;
            std::vector<uint8_t> plain;
            if (compressed && comp_ == 5) {
                plain.resize(g.packed ? g.packed : len);
                const size_t got = ZSTD_decompress(plain.data(), plain.size(), in.data(), in.size());
                if (ZSTD_isError(got)) return nullptr;
                plain.resize(got);
            } else {
                plain = std::move(in);
            }
            if (g.packed) {
                if (!unpack(plain.data(), plain.size(), discOff, data, len)) return nullptr;
            } else {
                data = std::move(plain);
                data.resize(len, 0);
            }
        }
        cache_.emplace_front(gi, std::move(data));
        if (cache_.size() > 8) cache_.pop_back();
        return &cache_.front().second;
    }

    std::mutex m_;
    std::unique_ptr<File> f_;
    bool rvz_ = true;
    uint32_t comp_ = 0, chunk_ = 0;
    uint64_t size_ = 0;
    uint8_t dhead_[0x80] = {};
    std::vector<Raw> raws_;
    std::vector<Group> groups_;
    std::list<std::pair<uint32_t, std::vector<uint8_t>>> cache_;  // decoded groups, most recent first
};
}  // namespace

std::unique_ptr<Image> open(const std::filesystem::path& path, std::string& error) {
    auto f = std::make_unique<File>();
    if (!f->open(path)) return error = "can't open the file", nullptr;
    uint8_t magic[4] = {};
    f->read(0, magic, 4);
    if (std::memcmp(magic, "CISO", 4) == 0) {
        auto img = std::make_unique<Ciso>();
        if (!img->init(std::move(f), error)) return nullptr;
        return img;
    }
    if (std::memcmp(magic, "RVZ\x01", 4) == 0 || std::memcmp(magic, "WIA\x01", 4) == 0) {
        auto img = std::make_unique<Rvz>();
        if (!img->init(std::move(f), error)) return nullptr;
        return img;
    }
    return std::make_unique<Iso>(std::move(f));
}

}  // namespace disc
