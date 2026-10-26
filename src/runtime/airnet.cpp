// Kirby Air Ride online: the master server's client (see airnet.h; protocol in docs/ONLINE.md).
//
// One background thread per Client does all the networking: a select() loop over the control
// connection (JSON lines over TCP), the UDP socket (the relay, STUN and peer-to-peer traffic), the
// relay stream (TCP, when UDP does not reach the master) and a wake-up socket that UI calls poke.
// UI calls only queue work for that thread; the thread publishes a Snapshot after every change.
//
// The relay's hot path runs on the game's threads: send() builds the datagram on the stack and
// sends it at once; frames that arrive wait in a fixed ring until poll() takes them. No allocation
// on either path (poll() reuses the caller's vector).
//
// Self-contained: sockets (Winsock / POSIX), SHA-256, HMAC, scrypt and a small JSON reader/writer
// are all here; no libraries beyond the OS.
#include "runtime/airnet.h"
#include "runtime/lanport.h"
#include "runtime/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <bcrypt.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rt {
void log(const char* fmt, ...);  // runtime/runtime.h (not included: it brings the guest CPU headers)
}

namespace rt::airnet {

// ================================================================================================
// Platform sockets
// ================================================================================================
namespace {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kBad = INVALID_SOCKET;
using socklen = int;
constexpr int kSendFlags = 0;
int lastError() { return WSAGetLastError(); }
bool wouldBlock(int e) { return e == WSAEWOULDBLOCK; }
bool inProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
void closeSock(sock_t s) { closesocket(s); }
bool nonBlocking(sock_t s) {
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0;
}
#else
using sock_t = int;
constexpr sock_t kBad = -1;
using socklen = socklen_t;
constexpr int kSendFlags = MSG_NOSIGNAL;  // a closed connection is an error, not SIGPIPE
int lastError() { return errno; }
bool wouldBlock(int e) { return e == EWOULDBLOCK || e == EAGAIN; }
bool inProgress(int e) { return e == EINPROGRESS || e == EWOULDBLOCK; }
void closeSock(sock_t s) { ::close(s); }
bool nonBlocking(sock_t s) {
    int flags = fcntl(s, F_GETFL, 0);
    return flags != -1 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool sameAddr(const sockaddr_storage& a, const sockaddr_storage& b) {
    if (a.ss_family != b.ss_family) return false;
    if (a.ss_family == AF_INET) {
        auto& x = reinterpret_cast<const sockaddr_in&>(a);
        auto& y = reinterpret_cast<const sockaddr_in&>(b);
        return x.sin_port == y.sin_port && x.sin_addr.s_addr == y.sin_addr.s_addr;
    }
    if (a.ss_family == AF_INET6) {
        auto& x = reinterpret_cast<const sockaddr_in6&>(a);
        auto& y = reinterpret_cast<const sockaddr_in6&>(b);
        return x.sin6_port == y.sin6_port && std::memcmp(&x.sin6_addr, &y.sin6_addr, sizeof x.sin6_addr) == 0;
    }
    return false;
}

void setPort(sockaddr_storage& a, uint16_t port) {
    if (a.ss_family == AF_INET) reinterpret_cast<sockaddr_in&>(a).sin_port = htons(port);
    if (a.ss_family == AF_INET6) reinterpret_cast<sockaddr_in6&>(a).sin6_port = htons(port);
}

socklen addrLen(const sockaddr_storage& a) {
    return a.ss_family == AF_INET6 ? socklen(sizeof(sockaddr_in6)) : socklen(sizeof(sockaddr_in));
}

// "host", "host:port", "[v6]:port", "v6" -> host, port
void splitAddress(const std::string& address, std::string& host, std::string& port) {
    host = address;
    port = std::to_string(kDefaultPort);
    if (!address.empty() && address[0] == '[') {
        size_t close = address.find(']');
        if (close != std::string::npos) {
            host = address.substr(1, close - 1);
            if (close + 1 < address.size() && address[close + 1] == ':') port = address.substr(close + 2);
        }
        return;
    }
    size_t colon = address.rfind(':');
    if (colon != std::string::npos && address.find(':') == colon) {  // exactly one ':'
        host = address.substr(0, colon);
        port = address.substr(colon + 1);
    }
}

}  // namespace

// ================================================================================================
// Crypto: SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256, scrypt (RFC 7914), random bytes
// ================================================================================================
namespace detail {
namespace {

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t used = 0;
    uint64_t total = 0;

    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void* data, size_t len) {
        auto* p = static_cast<const uint8_t*>(data);
        total += len;
        while (len) {
            size_t n = std::min(len, size_t(64) - used);
            std::memcpy(buf + used, p, n);
            used += n;
            p += n;
            len -= n;
            if (used == 64) {
                block(buf);
                used = 0;
            }
        }
    }
    void final(uint8_t out[32]) {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (used != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 8; i++) {
            out[4 * i] = uint8_t(h[i] >> 24);
            out[4 * i + 1] = uint8_t(h[i] >> 16);
            out[4 * i + 2] = uint8_t(h[i] >> 8);
            out[4 * i + 3] = uint8_t(h[i]);
        }
    }
};

struct Hmac {
    Sha256 inner, outer;
    Hmac(const void* key, size_t keyLen) {
        uint8_t k[64] = {};
        if (keyLen > 64) {
            Sha256 s;
            s.update(key, keyLen);
            s.final(k);
        } else {
            std::memcpy(k, key, keyLen);
        }
        uint8_t pad[64];
        for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
        inner.update(pad, 64);
        for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
        outer.update(pad, 64);
    }
    void update(const void* data, size_t len) { inner.update(data, len); }
    void final(uint8_t out[32]) {
        uint8_t ih[32];
        inner.final(ih);
        outer.update(ih, 32);
        outer.final(out);
    }
};

void pbkdf2(const void* pw, size_t pwLen, const void* salt, size_t saltLen, uint32_t iterations, uint8_t* out,
            size_t outLen) {
    const Hmac base(pw, pwLen);  // keyed once; copied per block
    for (uint32_t blockNo = 1; outLen; blockNo++) {
        Hmac h = base;
        h.update(salt, saltLen);
        uint8_t be[4] = {uint8_t(blockNo >> 24), uint8_t(blockNo >> 16), uint8_t(blockNo >> 8), uint8_t(blockNo)};
        h.update(be, 4);
        uint8_t u[32], t[32];
        h.final(u);
        std::memcpy(t, u, 32);
        for (uint32_t i = 1; i < iterations; i++) {
            Hmac h2 = base;
            h2.update(u, 32);
            h2.final(u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        size_t n = std::min(outLen, size_t(32));
        std::memcpy(out, t, n);
        out += n;
        outLen -= n;
    }
}

inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void salsa208(uint32_t b[16]) {
    uint32_t x[16];
    std::memcpy(x, b, 64);
    for (int i = 0; i < 8; i += 2) {
        x[4] ^= rotl(x[0] + x[12], 7);   x[8] ^= rotl(x[4] + x[0], 9);
        x[12] ^= rotl(x[8] + x[4], 13);  x[0] ^= rotl(x[12] + x[8], 18);
        x[9] ^= rotl(x[5] + x[1], 7);    x[13] ^= rotl(x[9] + x[5], 9);
        x[1] ^= rotl(x[13] + x[9], 13);  x[5] ^= rotl(x[1] + x[13], 18);
        x[14] ^= rotl(x[10] + x[6], 7);  x[2] ^= rotl(x[14] + x[10], 9);
        x[6] ^= rotl(x[2] + x[14], 13);  x[10] ^= rotl(x[6] + x[2], 18);
        x[3] ^= rotl(x[15] + x[11], 7);  x[7] ^= rotl(x[3] + x[15], 9);
        x[11] ^= rotl(x[7] + x[3], 13);  x[15] ^= rotl(x[11] + x[7], 18);
        x[1] ^= rotl(x[0] + x[3], 7);    x[2] ^= rotl(x[1] + x[0], 9);
        x[3] ^= rotl(x[2] + x[1], 13);   x[0] ^= rotl(x[3] + x[2], 18);
        x[6] ^= rotl(x[5] + x[4], 7);    x[7] ^= rotl(x[6] + x[5], 9);
        x[4] ^= rotl(x[7] + x[6], 13);   x[5] ^= rotl(x[4] + x[7], 18);
        x[11] ^= rotl(x[10] + x[9], 7);  x[8] ^= rotl(x[11] + x[10], 9);
        x[9] ^= rotl(x[8] + x[11], 13);  x[10] ^= rotl(x[9] + x[8], 18);
        x[12] ^= rotl(x[15] + x[14], 7); x[13] ^= rotl(x[12] + x[15], 9);
        x[14] ^= rotl(x[13] + x[12], 13); x[15] ^= rotl(x[14] + x[13], 18);
    }
    for (int i = 0; i < 16; i++) b[i] += x[i];
}

// B: 2r 64-byte blocks (as words); y: scratch of the same size.
void blockMix(uint32_t* b, uint32_t* y, uint32_t r) {
    uint32_t x[16];
    std::memcpy(x, b + (2 * r - 1) * 16, 64);
    for (uint32_t i = 0; i < 2 * r; i++) {
        for (int j = 0; j < 16; j++) x[j] ^= b[i * 16 + j];
        salsa208(x);
        std::memcpy(y + i * 16, x, 64);
    }
    for (uint32_t i = 0; i < r; i++) {
        std::memcpy(b + i * 16, y + (2 * i) * 16, 64);
        std::memcpy(b + (r + i) * 16, y + (2 * i + 1) * 16, 64);
    }
}

void roMix(uint8_t* block, uint32_t r, uint64_t n, uint32_t* v, uint32_t* x, uint32_t* y) {
    const size_t words = 32 * size_t(r);
    for (size_t i = 0; i < words; i++)
        x[i] = uint32_t(block[4 * i]) | uint32_t(block[4 * i + 1]) << 8 | uint32_t(block[4 * i + 2]) << 16 |
               uint32_t(block[4 * i + 3]) << 24;
    for (uint64_t i = 0; i < n; i++) {
        std::memcpy(v + i * words, x, words * 4);
        blockMix(x, y, r);
    }
    for (uint64_t i = 0; i < n; i++) {
        uint64_t j = x[(2 * r - 1) * 16] & (n - 1);
        const uint32_t* vj = v + j * words;
        for (size_t k = 0; k < words; k++) x[k] ^= vj[k];
        blockMix(x, y, r);
    }
    for (size_t i = 0; i < words; i++) {
        block[4 * i] = uint8_t(x[i]);
        block[4 * i + 1] = uint8_t(x[i] >> 8);
        block[4 * i + 2] = uint8_t(x[i] >> 16);
        block[4 * i + 3] = uint8_t(x[i] >> 24);
    }
}

}  // namespace

void sha256(const void* data, size_t len, uint8_t out[32]) {
    Sha256 s;
    s.update(data, len);
    s.final(out);
}

void hmacSha256(const void* key, size_t keyLen, const void* data, size_t len, uint8_t out[32]) {
    Hmac h(key, keyLen);
    h.update(data, len);
    h.final(out);
}

bool scrypt(const void* password, size_t passwordLen, const void* salt, size_t saltLen, uint64_t n, uint32_t r,
            uint32_t p, uint8_t* out, size_t outLen) {
    // Bounds: what the server may ask for (at most 256 MiB of memory).
    if (n < 2 || (n & (n - 1)) || r < 1 || r > 8 || p < 1 || p > 4 || n * r * 128 > (256ull << 20)) return false;
    const size_t blockBytes = 128 * size_t(r);
    std::vector<uint8_t> b(blockBytes * p);
    pbkdf2(password, passwordLen, salt, saltLen, 1, b.data(), b.size());
    std::vector<uint32_t> v;
    std::vector<uint32_t> x(32 * size_t(r)), y(32 * size_t(r));
    try {
        v.resize(32 * size_t(r) * n);
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (uint32_t i = 0; i < p; i++) roMix(b.data() + i * blockBytes, r, n, v.data(), x.data(), y.data());
    pbkdf2(password, passwordLen, b.data(), b.size(), 1, out, outLen);
    return true;
}

}  // namespace detail

namespace {

void randomBytes(uint8_t* out, size_t n) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return;
#else
    if (std::FILE* f = std::fopen("/dev/urandom", "rb")) {
        size_t got = std::fread(out, 1, n, f);
        std::fclose(f);
        if (got == n) return;
    }
#endif
    // Never reached on a working OS; still not the same bytes twice.
    static std::mutex mu;
    static uint8_t state[32];
    std::lock_guard<std::mutex> lock(mu);
    for (size_t i = 0; i < n; i += 32) {
        int64_t t = nowMs() ^ int64_t(reinterpret_cast<uintptr_t>(out));
        uint8_t in[40];
        std::memcpy(in, state, 32);
        std::memcpy(in + 32, &t, 8);
        detail::sha256(in, sizeof in, state);
        std::memcpy(out + i, state, std::min(size_t(32), n - i));
    }
}

std::string hex(const uint8_t* p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; i++) {
        s[2 * i] = digits[p[i] >> 4];
        s[2 * i + 1] = digits[p[i] & 15];
    }
    return s;
}

bool unhex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    out.resize(s.size() / 2);
    for (size_t i = 0; i < out.size(); i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + d;
        }
        out[i] = uint8_t(v);
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool validUsername(const std::string& s) {
    if (s.size() < size_t(kUsernameMin) || s.size() > size_t(kUsernameMax)) return false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '_' && c != '.' && c != '-'))) return false;
    }
    return true;
}

// HMAC-SHA256(key, label || parts...)
std::array<uint8_t, 32> mac(const std::vector<uint8_t>& key, std::string_view label,
                            std::initializer_list<std::string_view> parts) {
    detail::Hmac h(key.data(), key.size());
    h.update(label.data(), label.size());
    for (auto p : parts) h.update(p.data(), p.size());
    std::array<uint8_t, 32> out;
    h.final(out.data());
    return out;
}

std::string_view bytesView(const std::vector<uint8_t>& v) {
    return {reinterpret_cast<const char*>(v.data()), v.size()};
}

// ================================================================================================
// JSON (the control protocol): a reader into a small tree, and a writer for flat requests
// ================================================================================================

struct Json {
    enum Kind : uint8_t { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;        // array elements; object values
    std::vector<std::string> keys;  // object keys (keys[i] names items[i])

    const Json& operator[](std::string_view key) const {
        if (kind != Object) return none();
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == key) return items[i];
        return none();
    }
    const Json& at(size_t i) const { return kind == Array && i < items.size() ? items[i] : none(); }
    const std::vector<Json>& list() const {  // an array's elements (nothing for anything else)
        static const std::vector<Json> empty;
        return kind == Array ? items : empty;
    }
    bool has(std::string_view key) const { return &(*this)[key] != &none(); }
    std::string str() const { return kind == String ? s : std::string(); }
    int64_t num(int64_t def = 0) const { return kind == Number ? int64_t(n) : def; }
    bool flag() const { return kind == Bool ? b : kind == Number ? n != 0 : false; }
    static const Json& none() {
        static const Json empty;
        return empty;
    }
};

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : p_(text.data()), end_(text.data() + text.size()) {}
    bool parse(Json& out) {
        if (!value(out, 0)) return false;
        ws();
        return p_ == end_;
    }

private:
    const char* p_;
    const char* end_;

    void ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) p_++;
    }
    bool lit(const char* word) {
        size_t n = std::strlen(word);
        if (size_t(end_ - p_) < n || std::memcmp(p_, word, n)) return false;
        p_ += n;
        return true;
    }
    static void utf8(std::string& s, uint32_t c) {
        if (c < 0x80) {
            s += char(c);
        } else if (c < 0x800) {
            s += char(0xC0 | (c >> 6));
            s += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += char(0xE0 | (c >> 12));
            s += char(0x80 | ((c >> 6) & 0x3F));
            s += char(0x80 | (c & 0x3F));
        } else {
            s += char(0xF0 | (c >> 18));
            s += char(0x80 | ((c >> 12) & 0x3F));
            s += char(0x80 | ((c >> 6) & 0x3F));
            s += char(0x80 | (c & 0x3F));
        }
    }
    bool hex4(uint32_t& c) {
        if (end_ - p_ < 4) return false;
        c = 0;
        for (int i = 0; i < 4; i++) {
            char h = *p_++;
            int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
            if (d < 0) return false;
            c = c * 16 + uint32_t(d);
        }
        return true;
    }
    bool string(std::string& s) {
        p_++;  // "
        while (p_ < end_) {
            char c = *p_++;
            if (c == '"') return true;
            if (c != '\\') {
                s += c;
                continue;
            }
            if (p_ == end_) return false;
            char e = *p_++;
            switch (e) {
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            case '/': s += '/'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    p_ += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(s, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Json& v, int depth) {
        if (depth > 32) return false;
        ws();
        if (p_ == end_) return false;
        char c = *p_;
        if (c == '{') {
            v.kind = Json::Object;
            p_++;
            ws();
            if (p_ < end_ && *p_ == '}') return p_++, true;
            for (;;) {
                ws();
                if (p_ == end_ || *p_ != '"') return false;
                std::string key;
                if (!string(key)) return false;
                ws();
                if (p_ == end_ || *p_++ != ':') return false;
                v.keys.push_back(std::move(key));
                v.items.emplace_back();
                if (!value(v.items.back(), depth + 1)) return false;
                ws();
                if (p_ == end_) return false;
                if (*p_ == ',') { p_++; continue; }
                if (*p_ == '}') return p_++, true;
                return false;
            }
        }
        if (c == '[') {
            v.kind = Json::Array;
            p_++;
            ws();
            if (p_ < end_ && *p_ == ']') return p_++, true;
            for (;;) {
                v.items.emplace_back();
                if (!value(v.items.back(), depth + 1)) return false;
                ws();
                if (p_ == end_) return false;
                if (*p_ == ',') { p_++; continue; }
                if (*p_ == ']') return p_++, true;
                return false;
            }
        }
        if (c == '"') {
            v.kind = Json::String;
            return string(v.s);
        }
        if (lit("true")) { v.kind = Json::Bool; v.b = true; return true; }
        if (lit("false")) { v.kind = Json::Bool; v.b = false; return true; }
        if (lit("null")) { v.kind = Json::Null; return true; }
        // number
        const char* start = p_;
        while (p_ < end_ && (std::strchr("+-0123456789.eE", *p_))) p_++;
        if (p_ == start) return false;
        std::string num(start, p_);
        char* stop = nullptr;
        v.kind = Json::Number;
        v.n = std::strtod(num.c_str(), &stop);
        return stop && *stop == 0;
    }
};

// Appends `s` as a JSON string (UTF-8 kept; invalid bytes become '?', control characters escaped).
void jsonString(std::string& out, std::string_view s) {
    out += '"';
    for (size_t i = 0; i < s.size();) {
        auto c = uint8_t(s[i]);
        if (c < 0x80) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += char(c);
            } else if (c < 0x20 || c == 0x7F) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
            i++;
            continue;
        }
        size_t len = c >= 0xF0 && c < 0xF5 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
        bool ok = len && i + len <= s.size();
        for (size_t k = 1; ok && k < len; k++) ok = (uint8_t(s[i + k]) & 0xC0) == 0x80;
        if (ok) {
            out.append(s.data() + i, len);
            i += len;
        } else {
            out += '?';
            i++;
        }
    }
    out += '"';
}

// A parsed value as JSON text again.
void jsonWrite(std::string& out, const Json& j) {
    switch (j.kind) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Number: {
        char buf[40];
        if (!std::isfinite(j.n)) std::snprintf(buf, sizeof buf, "null");
        else if (j.n == std::floor(j.n) && std::fabs(j.n) < 9e15) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(j.n));
        else std::snprintf(buf, sizeof buf, "%.17g", j.n);
        out += buf;
        break;
    }
    case Json::String: jsonString(out, j.s); break;
    case Json::Array:
        out += '[';
        for (size_t i = 0; i < j.items.size(); i++) {
            if (i) out += ',';
            jsonWrite(out, j.items[i]);
        }
        out += ']';
        break;
    case Json::Object:
        out += '{';
        for (size_t i = 0; i < j.items.size() && i < j.keys.size(); i++) {
            if (i) out += ',';
            jsonString(out, j.keys[i]);
            out += ':';
            jsonWrite(out, j.items[i]);
        }
        out += '}';
        break;
    }
}
std::string jsonText(const Json& j) {
    if (j.kind == Json::Null) return "{}";
    std::string s;
    jsonWrite(s, j);
    return s;
}
// `text` as a JSON object, normalised (false if it is not one).
bool jsonObject(const std::string& text, std::string& out) {
    Json j;
    if (!JsonReader(text).parse(j) || j.kind != Json::Object) return false;
    out = jsonText(j);
    return true;
}

class Msg {
public:
    explicit Msg(const char* type) {
        out_ = "{\"t\":";
        jsonString(out_, type);
    }
    Msg& str(const char* key, std::string_view v) {
        this->key(key);
        jsonString(out_, v);
        return *this;
    }
    Msg& num(const char* key, int64_t v) {
        this->key(key);
        out_ += std::to_string(v);
        return *this;
    }
    Msg& flag(const char* key, bool v) {
        this->key(key);
        out_ += v ? "true" : "false";
        return *this;
    }
    Msg& raw(const char* key, std::string_view json) {
        this->key(key);
        out_ += json;
        return *this;
    }
    std::string finish(int64_t id) {
        if (id) num("id", id);
        out_ += "}\n";
        return std::move(out_);
    }

private:
    std::string out_;
    void key(const char* k) {
        out_ += ',';
        jsonString(out_, k);
        out_ += ':';
    }
};

// ================================================================================================
// Protocol values
// ================================================================================================

constexpr uint8_t kRelayVersion = 1;
constexpr uint8_t kRelayHello = 0x01;
constexpr uint8_t kRelayFrame = 0x02;
constexpr uint8_t kRelayFrameTo = 0x03;   // header + n u8 + n relay ids + frame: to those consoles only
constexpr uint8_t kRelayHelloAck = 0x81;
constexpr int kRelayTargetsMax = 16;
// Peer to peer (between consoles, over UDP): header = type, version, 0, 0, sender's peer id (u32 BE)
constexpr uint8_t kP2pPing = 0x10;   // + sender's key[8] + echo u32 + receiver's peer id u32
constexpr uint8_t kP2pPong = 0x11;   // + sender's key[8] + echo u32 (from the ping) + receiver's peer id
constexpr uint8_t kP2pFrame = 0x12;  // + one Ethernet frame (only from a checked address)
constexpr int64_t kPunchMs = 200;       // ping a candidate this often while finding a path (first 10 s) ...
constexpr int64_t kKeepaliveMs = 2000;  // ... and this often after (keeps NAT mappings open; RTT)
constexpr int64_t kPathLostMs = 6500;   // no answer for this long: the path is gone (relay meanwhile)
constexpr uint32_t kStunCookie = 0x2112A442;
constexpr size_t kRelayHeader = 8;
constexpr size_t kFrameMin = 14;
constexpr size_t kFrameMax = 1514;
constexpr size_t kTokenBytes = 24;
// The relay over TCP (the master's control port), when UDP gets no answer: the connection starts with
// the magic, then carries the same messages as the datagrams, each preceded by its length (u16 BE).
constexpr uint8_t kStreamMagic[8] = {'A', 'I', 'R', 'R', 'E', 'L', 'A', 'Y'};
constexpr size_t kStreamQueue = 64 * 1024;  // bytes waiting for the socket at most: past that, frames are dropped
constexpr int64_t kUdpTrialMs = 1500;       // no answer over UDP for this long: TCP
constexpr size_t kChatKeep = 100;

const char* modeId(Mode m) {
    switch (m) {
    case Mode::AirRide: return "air_ride";
    case Mode::CityTrial: return "city_trial";
    default: return "any";
    }
}
Mode parseMode(const std::string& s) {
    if (s == "air_ride") return Mode::AirRide;
    if (s == "city_trial") return Mode::CityTrial;
    return Mode::Any;
}
SessionState parseSession(const std::string& s) {
    if (s == "countdown") return SessionState::Countdown;
    if (s == "match") return SessionState::InMatch;
    if (s == "results") return SessionState::Results;
    return SessionState::Lobby;
}
uint32_t rd32be(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void wr32be(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

Profile parseProfile(const Json& j) {
    Profile p;
    p.username = j["username"].str();
    p.colour = j["colour"].str();
    p.machine = j["machine"].str();
    p.played = int(j["played"].num());
    p.wins = int(j["wins"].num());
    p.created = j["created"].num();
    p.lastSeen = j["last_seen"].num();
    p.online = j["online"].flag();
    p.lobby = uint32_t(j["lobby"].num());
    return p;
}

Lobby parseLobby(const Json& j) {
    Lobby l;
    const int64_t now = nowMs();
    l.id = uint32_t(j["id"].num());
    l.name = j["name"].str();
    l.host = j["host"].str();
    l.region = j["region"].str();
    l.mode = parseMode(j["mode"].str());
    l.max = int(j["max"].num(kLobbyMax));
    l.maxPlayers = int(j["max_players"].num(kMatchDefault));
    l.locked = j["locked"].flag();
    l.migrate = j["migrate"].flag();
    l.autoStart = j["auto_start"].flag();
    l.session = parseSession(j["state"].str());
    l.hold = j["hold"].flag();
    if (l.session == SessionState::Countdown) l.countdownEndsMs = now + j["countdown_ms"].num();
    if (l.session == SessionState::Results) l.resultsEndsMs = now + j["results_ms"].num();
    const Json& match = j["match"];
    l.matchId = uint32_t(match["id"].num());
    for (auto& n : match["players"].list()) l.matchPlayers.push_back(n.str());
    l.settingsJson = jsonText(j["settings"]);
    l.resultsJson = jsonText(j["results"]);
    Group roster;  // (old API: the roster as group 1)
    roster.id = 1;
    roster.playing = l.session == SessionState::InMatch;
    for (auto& m : j["members"].list()) {
        Member x;
        x.name = m["name"].str();
        x.colour = m["colour"].str();
        x.machine = m["machine"].str();
        x.host = m["host"].flag();
        x.player = m["player"].flag();
        x.peerId = uint32_t(m["peer"].num());
        x.group = x.player ? 1 : 0;
        x.relay = m["relay"].flag();
        x.relayTcp = m["relay_tcp"].flag();
        x.away = m["away"].flag();
        x.pingMs = int(m["ping"].num(-1));
        if (x.player) roster.members.push_back(x.name);
        l.members.push_back(std::move(x));
    }
    if (!roster.members.empty()) l.groups.push_back(std::move(roster));
    return l;
}

ChatLine parseChat(const Json& j) {
    ChatLine c;
    c.from = j["from"].str();
    c.text = j["text"].str();
    c.time = j["time"].num();
    c.system = j["sys"].flag();
    return c;
}

}  // namespace

// ================================================================================================
// Choices and names
// ================================================================================================

const Choice kColours[] = {
    {"pink", "Pink"}, {"yellow", "Yellow"}, {"red", "Red"},       {"blue", "Blue"},
    {"green", "Green"}, {"purple", "Purple"}, {"brown", "Brown"}, {"white", "White"},
};
const int kColourCount = int(sizeof kColours / sizeof kColours[0]);

const Choice kMachines[] = {
    {"warp_star", "Warp Star"},       {"compact_star", "Compact Star"}, {"winged_star", "Winged Star"},
    {"shadow_star", "Shadow Star"},   {"bulk_star", "Bulk Star"},       {"slick_star", "Slick Star"},
    {"formula_star", "Formula Star"}, {"wagon_star", "Wagon Star"},     {"rocket_star", "Rocket Star"},
    {"swerve_star", "Swerve Star"},   {"turbo_star", "Turbo Star"},     {"jet_star", "Jet Star"},
    {"wheelie_bike", "Wheelie Bike"}, {"rex_wheelie", "Rex Wheelie"},   {"wheelie_scooter", "Wheelie Scooter"},
    {"hydra", "Hydra"},               {"dragoon", "Dragoon"},
};
const int kMachineCount = int(sizeof kMachines / sizeof kMachines[0]);

const char* colourName(const std::string& id) {
    for (const Choice& c : kColours)
        if (id == c.id) return c.name;
    return "?";
}
const char* machineName(const std::string& id) {
    for (const Choice& c : kMachines)
        if (id == c.id) return c.name;
    return "?";
}

const char* stateName(State s) {
    switch (s) {
    case State::Offline: return "Offline";
    case State::Connecting: return "Connecting";
    case State::Connected: return "Connected";
    case State::LoggingIn: return "Logging in";
    case State::Online: return "Online";
    case State::Reconnecting: return "Reconnecting";
    }
    return "?";
}

const char* sessionStateName(SessionState s) {
    switch (s) {
    case SessionState::Lobby: return "Lobby";
    case SessionState::Countdown: return "Countdown";
    case SessionState::InMatch: return "In match";
    case SessionState::Results: return "Results";
    }
    return "?";
}

const char* pathName(Path p) {
    switch (p) {
    case Path::None: return "-";
    case Path::Relay: return "Relay";
    case Path::DirectLan: return "Direct (LAN)";
    case Path::DirectInternet: return "Direct";
    }
    return "?";
}

int64_t clockMs() { return nowMs(); }

const char* modeName(Mode m) {
    switch (m) {
    case Mode::AirRide: return "Air Ride";
    case Mode::CityTrial: return "City Trial";
    default: return "Any";
    }
}

// ================================================================================================
// The client
// ================================================================================================

struct Client::Impl {
    // The relay as the game's LAN port.
    struct RelayPort final : lan::Port {
        Impl* impl;
        explicit RelayPort(Impl* i) : impl(i) {}
        void send(const uint8_t* frame, size_t len) override { impl->relaySend(frame, len); }
        bool poll(std::vector<uint8_t>& frame) override { return impl->relayPoll(frame); }
        bool linkUp() const override { return impl->link.load(std::memory_order_relaxed); }
    };

    using Handler = std::function<void(const Json&)>;
    using Verifier = std::array<uint8_t, 32>;

    explicit Impl(bool install) : installPort(install), portObj(this) {
        streamOut.reserve(kStreamQueue + 2048);
        streamIn.reserve(64 * 1024);
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        address = serviceAddress();
        cur.savedLogin = savedLoginHere();
        cur.serial = 1;
        pub = cur;
    }

    ~Impl() {
        stopThread();
        if (job.valid()) job.wait();  // it wakes this thread through wakeSock when done
        if (installPort && lan::port() == &portObj) lan::setPort(prevPort);
        closeTcp();
        closeUdp();
        closeStream();
        if (stunResolve.valid()) stunResolve.wait();
        if (wakeSock != kBad) closeSock(wakeSock);
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // ---- shared with UI threads -------------------------------------------------------------------
    const bool installPort;
    mutable std::mutex snapMu;
    Snapshot pub;  // what snapshot() hands out
    std::mutex cmdMu;
    std::vector<std::function<void()>> cmds;
    std::thread thread;
    std::atomic<bool> quit{false};
    std::once_flag started;
    sock_t wakeSock = kBad;
    sockaddr_in wakeAddr{};

    // ---- shared with the game's threads (the relay) -----------------------------------------------
    std::mutex relayMu;  // udp, udpAddr, relayId
    sock_t udp = kBad;
    int udpFamily = 0;
    sockaddr_storage udpAddr{};
    uint32_t relayId = 0;
    std::atomic<bool> inGroup{false};
    std::atomic<bool> link{false};
    std::atomic<int> rttMs{-1};
    std::atomic<uint64_t> framesSent{0};
    std::atomic<uint64_t> framesReceived{0};
    struct Slot {
        uint16_t len;
        uint8_t data[kFrameMax];
    };
    static constexpr unsigned kRing = 64;
    std::mutex rxMu;
    std::vector<Slot> ring = std::vector<Slot>(kRing);
    unsigned rxHead = 0, rxCount = 0;
    RelayPort portObj;
    lan::Port* prevPort = nullptr;  // the port installed before ours (restored when we leave)
    // The TCP relay stream (guarded by relayMu, like udp)
    std::atomic<bool> relayTcp{false};  // the relay runs over the stream (else UDP)
    std::atomic<int> tcpOverride{-1};   // forceRelayTcp(): -1 the setting decides, else 0 / 1
    sock_t stream = kBad;
    bool streamReady = false;           // connected (the magic went first)
    bool streamBroken = false;          // a send failed: the network thread reconnects
    std::vector<uint8_t> streamOut;     // bytes the socket has not taken yet (at most kStreamQueue)
    size_t streamOutHead = 0;
    // Peer to peer (guarded by relayMu: the game's thread routes frames by it)
    struct Cand {
        sockaddr_storage addr{};
        int kind = 1;       // 0: LAN address, 1: public / seen by the master, 2: seen from the peer
        bool works = false; // a pong came back from it
        int64_t since = 0, lastPing = 0, lastPong = 0;
        int rtt = -1;
    };
    struct Peer {
        uint32_t id = 0;
        std::string name;
        uint8_t mac[6] = {};
        bool hasMac = false;
        uint8_t key[8] = {};
        std::vector<Cand> cands;
        int best = -1;      // the candidate frames go to; -1: the relay
    };
    std::vector<Peer> peers;            // the roster's other consoles
    uint8_t myKey[8] = {};
    std::atomic<int> p2pOverride{-1};   // enableP2p(): -1 the setting decides
    std::atomic<bool> pathsDirty{false};
    bool gatewayOn = false;             // (tests) UDP through a NAT simulator
    sockaddr_storage gatewayAddr{};
    bool macSet = false;                // (tests) this console's MAC
    uint8_t macOverride[6] = {};

    // ---- the network thread's own -----------------------------------------------------------------
    Snapshot cur;
    bool dirty = false;
    enum class Phase { None, Connecting, Hello, Ready } phase = Phase::None;
    sock_t tcp = kBad;
    std::string rbuf, wbuf;
    int64_t phaseStart = 0, lastRx = 0;
    std::string address;            // the master ("host:port"; internal: never shown to players)
    std::string resolvedFor;
    std::vector<sockaddr_storage> addrs;  // everything the master's name resolves to, tried in turn
    size_t addrIndex = 0;
    int attemptsLeft = 0;
    sockaddr_storage tcpAddr{};     // the address being connected to
    sockaddr_storage masterAddr{};  // ... and the one that answered (the relay goes there too)
    bool serverRelayTcp = false;    // the master takes relay streams (its welcome says)
    uint16_t udpPort = 0;
    int scN = 16384, scR = 8, scP = 1;
    std::vector<std::function<void()>> afterWelcome;
    bool autoLogin = false;
    std::unordered_map<int64_t, Handler> pending;
    int64_t nextId = 1;
    // account
    std::string user;
    std::vector<uint8_t> token;  // the session (in memory even when not remembered: reconnects)
    bool remember = false;
    struct {
        bool valid = false;
        std::array<uint8_t, 32> pw{};
        std::string salt;
        int64_t n = 0, r = 0, p = 0;
        Verifier v{};
    } kdf;  // the last derived verifier (register then login derives once)
    std::future<Verifier> job;
    std::function<void(bool, const Verifier&)> jobDone;
    std::vector<std::function<void()>> afterJob;
    bool jobOk = false;
    // reconnecting
    bool reconnecting = false;
    int64_t reconnectUntil = 0, nextAttempt = 0;
    // relay
    bool relayKnown = false;
    uint8_t relaySecret[8] = {};
    int64_t lastHello = 0, lastAck = 0, groupSince = 0;
    bool everAcked = false, udpWarned = false;
    bool ackedOnTransport = false;  // an answer came by the transport in use (else, over UDP: fall back)
    bool tcpForced = false;         // TCP because forced (not because UDP failed)
    std::string udpFailedFor;       // the master whose relay never answered over UDP (TCP at once next time)
    int64_t udpTrialStart = 0;
    bool streamConnecting = false;
    int64_t streamStart = 0, streamNextAttempt = 0;
    std::vector<uint8_t> streamIn;
    size_t streamInHead = 0;
    // candidates (the network thread's)
    bool p2pWasOn = false;
    bool peerInfoDirty = false;
    int64_t pathsAt = 0;
    std::vector<sockaddr_storage> lanCands;
    bool havePublic = false;
    sockaddr_storage publicAddr{};
    std::string stunFor;
    std::future<std::vector<sockaddr_storage>> stunResolve;
    std::vector<sockaddr_storage> stunServers;
    uint8_t stunTx[12] = {};
    bool stunWaiting = false;
    int64_t stunNext = 0;
    int stunTries = 0;
    size_t stunIdx = 0;

    // ================================================================================================
    // Threading
    // ================================================================================================

    void post(std::function<void()> fn) {
        std::call_once(started, [this] { startThread(); });
        {
            std::lock_guard<std::mutex> lock(cmdMu);
            cmds.push_back(std::move(fn));
        }
        wake();
    }

    void startThread() {
        wakeSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (wakeSock != kBad) {
            wakeAddr.sin_family = AF_INET;
            wakeAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            wakeAddr.sin_port = 0;
            socklen len = sizeof wakeAddr;
            if (bind(wakeSock, reinterpret_cast<sockaddr*>(&wakeAddr), sizeof wakeAddr) != 0 ||
                getsockname(wakeSock, reinterpret_cast<sockaddr*>(&wakeAddr), &len) != 0) {
                closeSock(wakeSock);
                wakeSock = kBad;
            } else {
                nonBlocking(wakeSock);
            }
        }
        thread = std::thread([this] { run(); });
    }

    void stopThread() {
        if (!thread.joinable()) return;
        quit = true;
        wake();
        thread.join();
    }

    void wake() {
        if (wakeSock == kBad) return;
        char b = 1;
        sendto(wakeSock, &b, 1, 0, reinterpret_cast<const sockaddr*>(&wakeAddr), sizeof wakeAddr);
    }

    void run() {
        while (!quit) {
            std::vector<std::function<void()>> todo;
            {
                std::lock_guard<std::mutex> lock(cmdMu);
                todo.swap(cmds);
            }
            for (auto& fn : todo) fn();
            tick(nowMs());
            publish();

            fd_set rd, wr, ex;
            FD_ZERO(&rd);
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            sock_t maxFd = 0;
            auto add = [&](sock_t s, fd_set& set) {
                FD_SET(s, &set);
                if (s > maxFd) maxFd = s;
            };
            if (wakeSock != kBad) add(wakeSock, rd);
            if (tcp != kBad) {
                if (phase == Phase::Connecting) {
                    add(tcp, wr);
                    add(tcp, ex);
                } else {
                    add(tcp, rd);
                    if (!wbuf.empty()) add(tcp, wr);
                }
            }
            sock_t u = udp;  // only this thread replaces it
            if (u != kBad) add(u, rd);
            sock_t st = stream;  // (likewise)
            if (st != kBad) {
                if (streamConnecting) {
                    add(st, wr);
                    add(st, ex);
                } else {
                    add(st, rd);
                    if (streamPending()) add(st, wr);
                }
            }
            timeval tv{0, 50 * 1000};  // timers tick at least 20 times a second
            int n = select(int(maxFd + 1), &rd, &wr, &ex, &tv);
            if (n < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (n == 0) continue;
            if (wakeSock != kBad && FD_ISSET(wakeSock, &rd)) {
                char buf[64];
                while (recv(wakeSock, buf, sizeof buf, 0) > 0) {
                }
            }
            if (u != kBad && FD_ISSET(u, &rd)) readUdp();
            if (st != kBad && st == stream) {
                if (streamConnecting) {
                    if (FD_ISSET(st, &wr) || FD_ISSET(st, &ex)) finishStream();
                } else {
                    if (FD_ISSET(st, &rd)) readStream();
                    if (st == stream && FD_ISSET(st, &wr)) flushStream();
                }
            }
            if (tcp != kBad) {
                if (phase == Phase::Connecting) {
                    if (FD_ISSET(tcp, &wr) || FD_ISSET(tcp, &ex)) finishConnect();
                } else {
                    if (FD_ISSET(tcp, &rd)) readTcp();
                    if (tcp != kBad && FD_ISSET(tcp, &wr)) flush();
                }
            }
        }
        // Leaving: say goodbye properly if we can.
        if (phase == Phase::Ready && cur.inLobby) {
            sendLine(Msg("leave").finish(0));
        }
        closeTcp();
    }

    void changed() { dirty = true; }

    void publish() {
        bool busy = !pending.empty() || job.valid() || phase == Phase::Connecting || phase == Phase::Hello;
        if (busy != cur.busy) {
            cur.busy = busy;
            dirty = true;
        }
        if (!dirty) return;
        dirty = false;
        std::lock_guard<std::mutex> lock(snapMu);
        cur.serial = pub.serial + 1;
        pub = cur;
    }

    void fail(const std::string& msg) {
        cur.error = msg.empty() ? std::string("Something went wrong.") : msg;
        cur.errorSerial++;
        changed();
        log("[online] %s\n", cur.error.c_str());
    }

    void setState(State s) {
        if (cur.state != s) {
            cur.state = s;
            changed();
        }
    }

    // ================================================================================================
    // Control connection
    // ================================================================================================

    void startConnect(const std::string& address, bool autoLoginAfter) {
        closeTcp();
        reconnecting = false;
        if (cur.loggedIn) signedOut();
        this->address = address.empty() ? serviceAddress() : address;
        cur.updateRequired = false;
        cur.savedLogin = savedLoginHere();
        if (this->address.empty()) {  // (a build without the official service: CMake's KAR_ONLINE_MASTER)
            setState(State::Offline);
            fail("This copy has no official online service: choose a dedicated server in Online Settings.");
            return;
        }
        autoLogin = autoLoginAfter;
        setState(State::Connecting);
        openTcp();
    }

    void openTcp() {
        closeTcp();
        if (resolvedFor != address || addrs.empty()) {
            addrs.clear();
            addrIndex = 0;
            std::string host, port;
            splitAddress(address, host, port);
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            addrinfo* res = nullptr;
            if (!host.empty() && getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0) {
                for (addrinfo* ai = res; ai; ai = ai->ai_next) {
                    if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
                    sockaddr_storage a{};
                    std::memcpy(&a, ai->ai_addr, std::min(sizeof a, size_t(ai->ai_addrlen)));
                    bool dup = false;
                    for (auto& b : addrs) dup = dup || sameAddr(a, b);
                    if (!dup) addrs.push_back(a);
                }
                freeaddrinfo(res);
            }
            if (addrs.empty()) {
                connectFailed("Could not find the online service.");
                return;
            }
            resolvedFor = address;
        }
        attemptsLeft = int(addrs.size());  // from the one that worked last, then the others
        connectAddr();
    }

    void connectAddr() {
        tcpAddr = addrs[addrIndex % addrs.size()];
        tcp = socket(tcpAddr.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (tcp == kBad || !nonBlocking(tcp)) {
            nextAddress("Could not open a network connection.");
            return;
        }
        int one = 1;
        setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        if (::connect(tcp, reinterpret_cast<const sockaddr*>(&tcpAddr), addrLen(tcpAddr)) != 0 &&
            !inProgress(lastError())) {
            nextAddress("Could not reach the online service.");
            return;
        }
        phase = Phase::Connecting;
        phaseStart = nowMs();
        changed();
    }

    // This address did not answer: the next one the name resolved to, if any is left.
    void nextAddress(const std::string& why) {
        closeTcp();
        if (--attemptsLeft > 0) {
            log("[online] master %s: address %zu of %zu did not answer; trying the next\n", address.c_str(),
                addrIndex % addrs.size() + 1, addrs.size());
            addrIndex = (addrIndex + 1) % addrs.size();
            connectAddr();
            return;
        }
        connectFailed(why);
    }

    void finishConnect() {
        int err = 0;
        socklen len = sizeof err;
        getsockopt(tcp, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err) {
            nextAddress("Could not reach the online service.");
            return;
        }
        masterAddr = tcpAddr;
        phase = Phase::Hello;
        phaseStart = lastRx = nowMs();
        sendLine(Msg("hello").num("proto", kProtoVersion).str("client", "KirbyAirRideRecomp").finish(0));
    }

    // `why` is shown to the player: it never names the master (the log does).
    void connectFailed(const std::string& why) {
        log("[online] master %s: %s\n", address.c_str(), why.c_str());
        closeTcp();
        if (reconnecting) return;  // tick() tries again
        afterWelcome.clear();
        setState(State::Offline);
        fail(why);
    }

    void closeTcp() {
        if (tcp != kBad) closeSock(tcp);
        tcp = kBad;
        phase = Phase::None;
        rbuf.clear();
        wbuf.clear();
        pending.clear();
        changed();
    }

    // The connection broke (or the server said goodbye).
    void connectionLost(const std::string& why, bool mayReconnect) {
        bool wasOnline = cur.loggedIn && !token.empty();
        closeTcp();
        if (mayReconnect && wasOnline) {
            if (!reconnecting) {
                reconnecting = true;
                reconnectUntil = nowMs() + 25000;  // the server keeps a lobby place for 30 s
                nextAttempt = nowMs() + 500;
                log("[online] %s Reconnecting...\n", why.c_str());
            }
            setState(State::Reconnecting);
            return;
        }
        reconnecting = false;
        signedOut();
        setState(State::Offline);
        fail(why);
    }

    void signedOut() {
        if (cur.inLobby) leaveLobbyLocal();
        cur.loggedIn = false;
        cur.me = Profile{};
        token.clear();
        changed();
    }

    void sendLine(const std::string& line) {
        if (tcp == kBad) return;
        wbuf += line;
        flush();
    }

    void flush() {
        while (!wbuf.empty() && tcp != kBad) {
            int n = ::send(tcp, wbuf.data(), int(std::min(wbuf.size(), size_t(1) << 20)), kSendFlags);
            if (n > 0) {
                wbuf.erase(0, size_t(n));
                continue;
            }
            if (n < 0 && wouldBlock(lastError())) return;
            connectionLost("Lost connection to the online service.", true);
            return;
        }
    }

    void readTcp() {
        char buf[16384];
        for (;;) {
            int n = recv(tcp, buf, sizeof buf, 0);
            if (n > 0) {
                rbuf.append(buf, size_t(n));
                lastRx = nowMs();
                if (rbuf.size() > (1u << 20)) {
                    connectionLost("The online service sent too much data.", true);
                    return;
                }
                if (size_t(n) < sizeof buf) break;
                continue;
            }
            if (n < 0 && wouldBlock(lastError())) break;
            connectionLost("Lost connection to the online service.", true);
            return;
        }
        size_t start = 0;
        for (;;) {
            size_t nl = rbuf.find('\n', start);
            if (nl == std::string::npos) break;
            std::string_view line(rbuf.data() + start, nl - start);
            start = nl + 1;
            Json msg;
            if (!line.empty() && JsonReader(line).parse(msg)) handle(msg);
            if (tcp == kBad) return;  // the message closed the connection
        }
        rbuf.erase(0, start);
    }

    // ---- requests ---------------------------------------------------------------------------------

    // Sends a request; `ok` runs on success; failures show the server's message (and run `bad`).
    void request(Msg msg, std::function<void(const Json&)> ok, std::function<void(const Json&)> bad = {}) {
        if (phase != Phase::Ready) {
            fail("Not connected to the online service.");
            return;
        }
        int64_t id = nextId++;
        pending[id] = [this, ok = std::move(ok), bad = std::move(bad)](const Json& r) {
            if (r["ok"].flag()) {
                if (ok) ok(r);
            } else {
                fail(r["msg"].str());
                if (bad) bad(r);
            }
        };
        sendLine(msg.finish(id));
        changed();
    }

    bool needOnline() {
        if (phase == Phase::Ready && cur.loggedIn) return true;
        fail(cur.state == State::Reconnecting ? "Reconnecting to the online service..." : "Please log in first.");
        return false;
    }

    // Runs `fn` once connected (connecting first if needed).
    void whenConnected(std::function<void()> fn) {
        if (phase == Phase::Ready) {
            fn();
            return;
        }
        afterWelcome.push_back(std::move(fn));
        if (tcp == kBad && !reconnecting) startConnect(address, false);
    }

    // ---- server messages --------------------------------------------------------------------------

    void handle(const Json& msg) {
        const std::string t = msg["t"].str();
        if (t == "reply") {
            auto it = pending.find(msg["id"].num());
            if (it != pending.end()) {
                Handler h = std::move(it->second);
                pending.erase(it);
                changed();
                h(msg);
            }
        } else if (t == "ping") {
            sendLine(Msg("pong").num("n", msg["n"].num()).finish(0));
        } else if (t == "welcome") {
            welcome(msg);
        } else if (t == "lobby") {
            if (cur.inLobby) applyLobby(msg["lobby"]);
        } else if (t == "chat") {
            if (cur.inLobby) {
                cur.chat.push_back(parseChat(msg));
                if (cur.chat.size() > kChatKeep) cur.chat.erase(cur.chat.begin());
                changed();
            }
        } else if (t == "peers") {
            applyPeers(msg["peers"]);
        } else if (t == "match_start") {
            matchStarts(msg["match"]);
        } else if (t == "lobby_gone") {
            leaveLobbyLocal();
            fail(msg["msg"].str());
        } else if (t == "bye") {
            const std::string code = msg["code"].str();
            if (code == "update_required") cur.updateRequired = true;
            const bool again = code == "shutdown";
            connectionLost(msg["msg"].str(), again);
        }
    }

    void welcome(const Json& msg) {
        if (msg["proto"].num() < 1) {
            cur.updateRequired = true;
            connectionLost("The online service is not compatible with this version of the game.", false);
            return;
        }
        phase = Phase::Ready;
        udpPort = uint16_t(msg["udp_port"].num());
        serverRelayTcp = msg["relay_tcp"].flag();
        const Json& sc = msg["scrypt"];
        scN = int(sc["n"].num(16384));
        scR = int(sc["r"].num(8));
        scP = int(sc["p"].num(1));
        cur.serverVersion = msg["server"].str();
        cur.motd = msg["motd"].str();
        cur.usersOnline = int(msg["online"].num());
        log("[online] connected to %s (%s)\n", address.c_str(), cur.serverVersion.c_str());
        openUdp();
        if (reconnecting) {
            setState(State::LoggingIn);
            resume(user, token, remember, true);
            return;
        }
        setState(State::Connected);
        auto todo = std::move(afterWelcome);
        afterWelcome.clear();
        if (todo.empty() && autoLogin && savedLoginHere()) resumeSaved();
        for (auto& fn : todo) fn();
    }

    // ================================================================================================
    // Accounts
    // ================================================================================================

    // Derives the verifier off this thread (scrypt takes tens of milliseconds and 16 MiB), then
    // calls `done` here.
    void derive(const std::string& password, const std::vector<uint8_t>& salt, int64_t n, int64_t r, int64_t p,
                std::function<void(bool, const Verifier&)> done) {
        std::array<uint8_t, 32> pw;
        detail::sha256(password.data(), password.size(), pw.data());
        const std::string saltStr(bytesView(salt));
        if (kdf.valid && kdf.pw == pw && kdf.salt == saltStr && kdf.n == n && kdf.r == r && kdf.p == p) {
            done(true, kdf.v);
            return;
        }
        if (job.valid()) {  // one derivation at a time: this one runs after
            afterJob.push_back([=, this] { derive(password, salt, n, r, p, done); });
            return;
        }
        jobDone = [this, pw, saltStr, n, r, p, done = std::move(done)](bool ok, const Verifier& v) {
            if (ok) {
                kdf.valid = true;
                kdf.pw = pw;
                kdf.salt = saltStr;
                kdf.n = n;
                kdf.r = r;
                kdf.p = p;
                kdf.v = v;
            }
            done(ok, v);
        };
        job = std::async(std::launch::async, [this, password, salt, n, r, p] {
            Verifier v{};
            jobOk = n > 0 && r > 0 && p > 0 &&
                    detail::scrypt(password.data(), password.size(), salt.data(), salt.size(), uint64_t(n),
                                   uint32_t(r), uint32_t(p), v.data(), v.size());
            wake();
            return v;
        });
        changed();
    }

    bool checkCredentials(const std::string& username, const std::string& password) {
        if (!validUsername(username)) {
            fail("Usernames are 3 to 16 letters, digits, '_', '.' or '-', starting with a letter or digit.");
            return false;
        }
        if (password.size() < size_t(kPasswordMin)) {
            fail("Passwords are at least 6 characters.");
            return false;
        }
        return true;
    }

    void registerAccount(const std::string& username, const std::string& password, bool keep) {
        if (!checkCredentials(username, password)) return;
        whenConnected([=, this] {
            setState(State::LoggingIn);
            std::vector<uint8_t> salt(16);
            randomBytes(salt.data(), salt.size());
            const int n = scN, r = scR, p = scP;
            derive(password, salt, n, r, p, [=, this](bool ok, const Verifier& v) {
                if (!ok) {
                    afterLoginFailed();
                    fail("Could not secure the password on this PC.");
                    return;
                }
                request(Msg("register").str("username", username).str("salt", hex(salt.data(), salt.size()))
                            .num("n", n).num("r", r).num("p", p).str("verifier", hex(v.data(), v.size())),
                        [=, this](const Json&) {
                            log("[online] registered %s\n", username.c_str());
                            login(username, password, keep);  // the same salt: no second derivation
                        },
                        [=, this](const Json&) { afterLoginFailed(); });
            });
        });
    }

    void login(const std::string& username, const std::string& password, bool keep) {
        if (!checkCredentials(username, password)) return;
        whenConnected([=, this] {
            setState(State::LoggingIn);
            request(Msg("login_begin").str("username", username), [=, this](const Json& r) {
                std::vector<uint8_t> salt, nonce;
                if (!unhex(r["salt"].str(), salt) || !unhex(r["nonce"].str(), nonce)) {
                    afterLoginFailed();
                    fail("The online service sent a bad reply.");
                    return;
                }
                derive(password, salt, r["n"].num(), r["r"].num(), r["p"].num(), [=, this](bool ok, const Verifier& v) {
                    if (!ok) {
                        afterLoginFailed();
                        fail("Could not check the password on this PC.");
                        return;
                    }
                    std::vector<uint8_t> cnonce(16);
                    randomBytes(cnonce.data(), cnonce.size());
                    const std::vector<uint8_t> key(v.begin(), v.end());
                    const std::string name = lower(username);
                    auto proof = mac(key, std::string_view("airride-login\0", 14),
                                     {bytesView(nonce), bytesView(cnonce), name});
                    request(Msg("login").str("username", username).str("cnonce", hex(cnonce.data(), cnonce.size()))
                                .str("proof", hex(proof.data(), proof.size())),
                            [=, this](const Json& reply) {
                                std::vector<uint8_t> enc;
                                if (!unhex(reply["token"].str(), enc) || enc.size() != kTokenBytes) {
                                    afterLoginFailed();
                                    fail("The online service sent a bad reply.");
                                    return;
                                }
                                auto mask = mac(key, std::string_view("airride-token\0", 14),
                                                {bytesView(nonce), bytesView(cnonce)});
                                for (size_t i = 0; i < kTokenBytes; i++) enc[i] ^= mask[i];
                                signedIn(username, std::move(enc), keep, reply);
                            },
                            [=, this](const Json&) { afterLoginFailed(); });
                });
            }, [=, this](const Json&) { afterLoginFailed(); });
        });
    }

    void resumeSaved() {
        const std::string u = settings::onlineUser.get();
        std::vector<uint8_t> t;
        if (u.empty() || !savedLoginHere() || !unhex(settings::onlineToken.get(), t) || t.size() != kTokenBytes) {
            fail("No saved login on this PC.");
            return;
        }
        whenConnected([=, this] {
            setState(State::LoggingIn);
            resume(u, t, true, false);
        });
    }

    // Logs in with a session token: a saved login, or the session of a dropped connection.
    void resume(const std::string& username, const std::vector<uint8_t>& tok, bool keep, bool afterDrop) {
        auto failed = [=, this](const Json& r) {
            if (r["err"].str() == "bad_token" && !afterDrop) forgetSavedLogin();
            if (afterDrop) {
                reconnecting = false;
                signedOut();
            }
            afterLoginFailed();
        };
        request(Msg("login_begin").str("username", username), [=, this](const Json& r) {
            std::vector<uint8_t> nonce;
            if (!unhex(r["nonce"].str(), nonce)) return failed(r);
            std::vector<uint8_t> cnonce(16);
            randomBytes(cnonce.data(), cnonce.size());
            auto proof = mac(tok, std::string_view("airride-resume\0", 15),
                             {bytesView(nonce), bytesView(cnonce), lower(username)});
            request(Msg("resume").str("username", username).str("cnonce", hex(cnonce.data(), cnonce.size()))
                        .str("proof", hex(proof.data(), proof.size())),
                    [=, this](const Json& reply) {
                        reconnecting = false;
                        signedIn(username, tok, keep, reply);
                    },
                    failed);
        }, failed);
    }

    void afterLoginFailed() { setState(cur.loggedIn ? State::Online : phase == Phase::Ready ? State::Connected : State::Offline); }

    void signedIn(const std::string& username, std::vector<uint8_t> tok, bool keep, const Json& reply) {
        cur.me = parseProfile(reply["profile"]);
        user = cur.me.username.empty() ? username : cur.me.username;
        token = std::move(tok);
        remember = keep;
        cur.loggedIn = true;
        if (keep) {
            settings::onlineUser.set(user);
            settings::onlineToken.set(hex(token.data(), token.size()));
            settings::onlineTokenServer.set(address == settings::onlineMaster.get() ? std::string() : address);
            settings::save();
        } else if (lower(settings::onlineUser.get()) == lower(user)) {
            forgetSavedLogin();  // this login is not to be kept: neither is an older one
        }
        cur.savedLogin = savedLoginHere();
        setState(State::Online);
        if (reply.has("lobby")) {
            enterLobby(reply["lobby"], reply["relay"], reply["chat"]);
        } else if (cur.inLobby) {
            leaveLobbyLocal();
            fail("Your place in the server was lost while you were disconnected.");
        }
        log("[online] logged in as %s\n", user.c_str());
        changed();
    }

    // The login kept on this PC is for the server connected to (online_token_server; empty: the master).
    bool savedLoginHere() const {
        if (settings::onlineToken.get().empty()) return false;
        const std::string kept = settings::onlineTokenServer.get();
        return (kept.empty() ? settings::onlineMaster.get() : kept) == address;
    }

    void forgetSavedLogin() {
        if (settings::onlineToken.get().empty()) return;
        settings::onlineToken.set("");
        settings::save();
        cur.savedLogin = false;
        changed();
    }

    void logout() {
        forgetSavedLogin();
        if (phase != Phase::Ready || !cur.loggedIn) {
            signedOut();
            return;
        }
        request(Msg("logout"), [this](const Json&) {
            signedOut();
            setState(State::Connected);
        });
    }

    void changePassword(const std::string& password) {
        if (!needOnline()) return;
        if (password.size() < size_t(kPasswordMin)) {
            fail("Passwords are at least 6 characters.");
            return;
        }
        std::vector<uint8_t> salt(16);
        randomBytes(salt.data(), salt.size());
        const int n = scN, r = scR, p = scP;
        derive(password, salt, n, r, p, [=, this](bool ok, const Verifier& v) {
            if (!ok) return fail("Could not secure the password on this PC.");
            request(Msg("set_password").str("salt", hex(salt.data(), salt.size())).num("n", n).num("r", r)
                        .num("p", p).str("verifier", hex(v.data(), v.size())),
                    [=, this](const Json&) { login(user, password, remember); });  // a fresh session token
        });
    }

    // ================================================================================================
    // Lobby
    // ================================================================================================

    void enterLobby(const Json& lobby, const Json& relay, const Json& chat) {
        cur.inLobby = true;
        cur.chat.clear();
        for (auto& line : chat.list()) cur.chat.push_back(parseChat(line));
        while (cur.chat.size() > kChatKeep) cur.chat.erase(cur.chat.begin());
        std::vector<uint8_t> secret;
        bool fresh = false;
        if (unhex(relay["secret"].str(), secret) && secret.size() == sizeof relaySecret) {
            std::lock_guard<std::mutex> lock(relayMu);
            const uint32_t id = uint32_t(relay["id"].num());
            std::vector<uint8_t> key;
            if (unhex(relay["key"].str(), key) && key.size() == sizeof myKey) std::memcpy(myKey, key.data(), sizeof myKey);
            sockaddr_storage addr = masterAddr;
            setPort(addr, uint16_t(relay["port"].num(udpPort)));
            // Back in the same place after a reconnect: the relay never stopped (no cable blip).
            const bool same = relayKnown && id == relayId && sameAddr(addr, udpAddr) &&
                              std::memcmp(relaySecret, secret.data(), sizeof relaySecret) == 0;
            if (!same) {
                std::memcpy(relaySecret, secret.data(), sizeof relaySecret);
                relayId = id;
                udpAddr = addr;
                relayKnown = true;
                everAcked = false;
                ackedOnTransport = false;
                lastHello = 0;
                fresh = true;
            }
        }
        if (fresh) {  // a new place in a lobby: the transport is chosen afresh (tick)
            closeStream();
            relayTcp = false;
            tcpForced = false;
            udpTrialStart = nowMs();
            streamNextAttempt = 0;
            peerInfoDirty = true;  // tell the master where this console can be reached
            lanCands.clear();
        }
        applyLobby(lobby);
    }

    void applyLobby(const Json& j) {
        cur.lobby = parseLobby(j);
        const std::string me = lower(cur.me.username);
        cur.isHost = lower(cur.lobby.host) == me;
        cur.player = false;
        for (auto& m : cur.lobby.members)
            if (lower(m.name) == me) cur.player = m.player;
        cur.myGroup = cur.player ? 1 : 0;
        updateGroup();
        refreshPaths(nowMs(), true);
        changed();
    }

    void leaveLobbyLocal() {
        cur.inLobby = false;
        cur.lobby = Lobby{};
        cur.isHost = false;
        cur.player = false;
        cur.myGroup = 0;
        cur.chat.clear();
        relayKnown = false;
        {
            std::lock_guard<std::mutex> lock(relayMu);
            relayId = 0;
        }
        closeStream();
        relayTcp = false;
        updateGroup();
        changed();
    }

    // On the roster: the peers and the relay are this console's LAN. Off it: no cable.
    void updateGroup() {
        const bool g = cur.inLobby && cur.player;
        if (g == inGroup.load()) return;
        clearRing();
        if (g) {
            groupSince = nowMs();
            udpWarned = false;
            inGroup = true;
            if (installPort) {
                lan::Port* before = lan::port();
                if (before != &portObj) prevPort = before;  // e.g. the developer loopback: back after
                lan::setPort(&portObj);
                lan::setLocalDhcp(true);  // an address at once, not after the IP stack's ~35 s of DHCP tries
            }
            log("[online] a player of this server's match: LAN plugged in\n");
        } else {
            inGroup = false;
            link = false;
            {
                std::lock_guard<std::mutex> lock(relayMu);
                peers.clear();
            }
            pathsDirty = true;
            if (installPort && lan::port() == &portObj) lan::setPort(prevPort), lan::setLocalDhcp(false);
            prevPort = nullptr;
            log("[online] not a player (waiting): LAN unplugged\n");
        }
    }

    // ================================================================================================
    // Relay (UDP)
    // ================================================================================================

    void openUdp() {
        std::lock_guard<std::mutex> lock(relayMu);
        if (udp != kBad && udpFamily == masterAddr.ss_family) return;
        if (udp != kBad) closeSock(udp);
        udpFamily = masterAddr.ss_family;
        udp = socket(udpFamily, SOCK_DGRAM, IPPROTO_UDP);
        if (udp == kBad) return;
        nonBlocking(udp);
        int size = 1 << 20;
        setsockopt(udp, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof size);
#ifdef _WIN32
        // An ICMP "port unreachable" must not make the next recvfrom fail.
        BOOL off = FALSE;
        DWORD bytes = 0;
        WSAIoctl(udp, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &bytes, nullptr, nullptr);
#endif
        sockaddr_storage any{};
        any.ss_family = decltype(any.ss_family)(udpFamily);
        bind(udp, reinterpret_cast<const sockaddr*>(&any), addrLen(any));
    }

    void closeUdp() {
        std::lock_guard<std::mutex> lock(relayMu);
        if (udp != kBad) closeSock(udp);
        udp = kBad;
        udpFamily = 0;
    }

    void sendHello(int64_t now) {
        uint8_t msg[2 + kRelayHeader + 8 + 4];  // (the length prefix is for the stream)
        uint8_t* d = msg + 2;
        std::lock_guard<std::mutex> lock(relayMu);
        lastHello = now;
        if (!relayId) return;
        const uint32_t echo = uint32_t(now);
        d[0] = kRelayHello;
        d[1] = kRelayVersion;
        d[2] = d[3] = 0;
        d[4] = uint8_t(relayId >> 24); d[5] = uint8_t(relayId >> 16); d[6] = uint8_t(relayId >> 8); d[7] = uint8_t(relayId);
        std::memcpy(d + 8, relaySecret, 8);
        d[16] = uint8_t(echo >> 24); d[17] = uint8_t(echo >> 16); d[18] = uint8_t(echo >> 8); d[19] = uint8_t(echo);
        if (relayTcp.load()) {
            msg[0] = 0;
            msg[1] = uint8_t(sizeof msg - 2);
            streamWriteLocked(msg, sizeof msg);
        } else if (udp != kBad) {
            udpSendLocked(udpAddr, d, sizeof msg - 2);
        }
    }

    void readUdp() {
        uint8_t buf[2048];
        for (int i = 0; i < 256; i++) {
            sockaddr_storage from{};
            socklen fromLen = sizeof from;
            int n = recvfrom(udp, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n < 0) {
                int e = lastError();
                if (wouldBlock(e)) return;
#ifdef _WIN32
                if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;
#endif
                return;
            }
            const uint8_t* d = buf;
            size_t len = size_t(n);
            if (gatewayOn) {  // (tests) through the NAT simulator: unwrap who it is really from
                if (!sameAddr(from, gatewayAddr) || len < 6) continue;
                sockaddr_in a{};
                a.sin_family = AF_INET;
                std::memcpy(&a.sin_addr, d, 4);
                std::memcpy(&a.sin_port, d + 4, 2);
                from = sockaddr_storage{};
                std::memcpy(&from, &a, sizeof a);
                d += 6;
                len -= 6;
            }
            if (sameAddr(from, udpAddr)) {
                handleRelayMessage(d, len, false);
                continue;
            }
            if (stunAnswer(d, len)) continue;
            handleP2p(d, len, from);
        }
    }

    // UDP out (relayMu held): straight, or (tests) wrapped for the NAT simulator.
    int udpSendLocked(const sockaddr_storage& to, const uint8_t* d, size_t n) {
        if (udp == kBad) return -1;
        if (!gatewayOn)
            return sendto(udp, reinterpret_cast<const char*>(d), int(n), 0, reinterpret_cast<const sockaddr*>(&to), addrLen(to));
        uint8_t w[6 + 1700];
        if (to.ss_family != AF_INET || n > sizeof w - 6) return -1;
        const auto& a = reinterpret_cast<const sockaddr_in&>(to);
        std::memcpy(w, &a.sin_addr, 4);  // (both already in network order)
        std::memcpy(w + 4, &a.sin_port, 2);
        std::memcpy(w + 6, d, n);
        return sendto(udp, reinterpret_cast<const char*>(w), int(n + 6), 0, reinterpret_cast<const sockaddr*>(&gatewayAddr),
                      addrLen(gatewayAddr));
    }

    // ================================================================================================
    // Peer to peer
    // ================================================================================================

    bool p2pEnabled() const {
        const int o = p2pOverride.load();
        return o >= 0 ? o != 0 : settings::onlineP2p.load();
    }

    void myMac(uint8_t out[6]) {
        if (macSet) std::memcpy(out, macOverride, 6);
        else lan::macAddress(out);
    }

    static bool parseIp(const std::string& ip, uint16_t port, sockaddr_storage& out) {
        out = sockaddr_storage{};
        if (!port) return false;
        sockaddr_in a{};
        if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) == 1) {
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            std::memcpy(&out, &a, sizeof a);
            return true;
        }
        sockaddr_in6 b{};
        if (inet_pton(AF_INET6, ip.c_str(), &b.sin6_addr) == 1) {
            b.sin6_family = AF_INET6;
            b.sin6_port = htons(port);
            std::memcpy(&out, &b, sizeof b);
            return true;
        }
        return false;
    }

    static std::string ipText(const sockaddr_storage& a, uint16_t* port = nullptr) {
        char buf[64] = "?";
        if (a.ss_family == AF_INET) {
            const auto& v = reinterpret_cast<const sockaddr_in&>(a);
            inet_ntop(AF_INET, &v.sin_addr, buf, sizeof buf);
            if (port) *port = ntohs(v.sin_port);
        } else if (a.ss_family == AF_INET6) {
            const auto& v = reinterpret_cast<const sockaddr_in6&>(a);
            inet_ntop(AF_INET6, &v.sin6_addr, buf, sizeof buf);
            if (port) *port = ntohs(v.sin6_port);
        }
        return buf;
    }

    Peer* findPeer(uint32_t id) {
        for (auto& p : peers)
            if (p.id == id) return &p;
        return nullptr;
    }

    Cand* addCand(Peer& p, const sockaddr_storage& a, int kind, int64_t now) {
        for (auto& c : p.cands)
            if (sameAddr(c.addr, a)) {
                c.kind = std::min(c.kind, kind);
                return &c;
            }
        if (p.cands.size() >= 12) return nullptr;
        Cand c;
        c.addr = a;
        c.kind = kind;
        c.since = now;
        p.cands.push_back(c);
        return &p.cands.back();
    }

    // The path to `p`: its best working candidate (a better kind wins; among equals, a clearly
    // lower round trip), else the relay.
    void pickBest(Peer& p) {
        int best = p.best >= 0 && p.best < int(p.cands.size()) && p.cands[size_t(p.best)].works ? p.best : -1;
        for (int i = 0; i < int(p.cands.size()); i++) {
            const Cand& c = p.cands[size_t(i)];
            if (!c.works || i == best) continue;
            if (best < 0) {
                best = i;
                continue;
            }
            const Cand& b = p.cands[size_t(best)];
            if (c.kind < b.kind || (c.kind == b.kind && c.rtt >= 0 && c.rtt + 5 < b.rtt)) best = i;
        }
        if (best != p.best) {
            if (best >= 0) {
                uint16_t port = 0;
                const std::string ip = ipText(p.cands[size_t(best)].addr, &port);
                log("[online] %s: direct path %s:%u (%s, %d ms)\n", p.name.c_str(), ip.c_str(), unsigned(port),
                    p.cands[size_t(best)].kind == 0 ? "LAN" : "internet", p.cands[size_t(best)].rtt);
            } else {
                log("[online] %s: through the relay\n", p.name.c_str());
            }
            p.best = best;
            pathsDirty = true;
        }
    }

    // The master's list of the roster's other consoles (what is known about each is kept).
    void applyPeers(const Json& list) {
        const int64_t now = nowMs();
        std::lock_guard<std::mutex> lock(relayMu);
        std::vector<Peer> next;
        for (auto& j : list.list()) {
            const uint32_t id = uint32_t(j["id"].num());
            Peer p;
            if (Peer* old = findPeer(id)) p = *old;
            p.id = id;
            p.name = j["name"].str();
            std::vector<uint8_t> b;
            p.hasMac = unhex(j["mac"].str(), b) && b.size() == 6;
            if (p.hasMac) std::memcpy(p.mac, b.data(), 6);
            if (unhex(j["key"].str(), b) && b.size() == sizeof p.key) std::memcpy(p.key, b.data(), sizeof p.key);
            for (auto& c : j["candidates"].list()) {
                sockaddr_storage a;
                if (!parseIp(c["ip"].str(), uint16_t(c["port"].num()), a) || int(a.ss_family) != udpFamily) continue;
                addCand(p, a, c["type"].str() == "lan" ? 0 : 1, now);
            }
            next.push_back(std::move(p));
        }
        peers = std::move(next);
        for (auto& p : peers) pickBest(p);
        pathsDirty = true;
    }

    void p2pHeader(uint8_t* d, uint8_t type) {
        d[0] = type;
        d[1] = kRelayVersion;
        d[2] = d[3] = 0;
        wr32be(d + 4, relayId);
    }

    void sendPing(Peer& p, Cand& c, int64_t now) {  // relayMu held
        uint8_t d[kRelayHeader + 8 + 4 + 4];
        p2pHeader(d, kP2pPing);
        std::memcpy(d + 8, myKey, 8);
        wr32be(d + 16, uint32_t(now));
        wr32be(d + 20, p.id);
        udpSendLocked(c.addr, d, sizeof d);
        c.lastPing = now;
    }

    // A datagram from another console.
    void handleP2p(const uint8_t* d, size_t n, const sockaddr_storage& from) {
        if (n < kRelayHeader || d[1] != kRelayVersion) return;
        const uint8_t type = d[0];
        const uint32_t id = rd32be(d + 4);
        const int64_t now = nowMs();
        if (type == kP2pFrame) {
            const size_t len = n - kRelayHeader;
            if (len < kFrameMin || len > kFrameMax || !inGroup.load(std::memory_order_relaxed)) return;
            {
                std::lock_guard<std::mutex> lock(relayMu);
                Peer* p = findPeer(id);
                if (!p) return;
                bool ok = false;  // only from an address that answered our ping
                for (auto& c : p->cands) ok = ok || (c.works && sameAddr(c.addr, from));
                if (!ok) return;
            }
            pushFrame(d + kRelayHeader, len);
            return;
        }
        if ((type != kP2pPing && type != kP2pPong) || n < kRelayHeader + 16 || !p2pEnabled()) return;
        std::lock_guard<std::mutex> lock(relayMu);
        Peer* p = findPeer(id);
        if (!p || std::memcmp(d + 8, p->key, 8) != 0 || rd32be(d + 20) != relayId) return;
        if (type == kP2pPing) {
            uint8_t r[kRelayHeader + 16];
            p2pHeader(r, kP2pPong);
            std::memcpy(r + 8, myKey, 8);
            std::memcpy(r + 16, d + 16, 4);
            wr32be(r + 20, id);
            udpSendLocked(from, r, sizeof r);
            // Where it came from may be a way back we did not know (a peer-reflexive address).
            if (Cand* c = addCand(*p, from, 2, now))
                if (!c->works && now - c->lastPing > 50) sendPing(*p, *c, now);
        } else {
            Cand* c = addCand(*p, from, 2, now);
            const int rtt = int(uint32_t(now) - rd32be(d + 16));
            if (!c || rtt < 0 || rtt > 10000) return;
            c->works = true;
            c->lastPong = now;
            c->rtt = c->rtt < 0 ? rtt : (c->rtt * 3 + rtt) / 4;
            pickBest(*p);
            pathsDirty = true;
        }
    }

    void pushFrame(const uint8_t* frame, size_t len) {
        std::lock_guard<std::mutex> lock(rxMu);
        unsigned slot;
        if (rxCount == kRing) {  // the game is not taking them: the oldest is the least useful
            slot = rxHead;
            rxHead = (rxHead + 1) % kRing;
        } else {
            slot = (rxHead + rxCount) % kRing;
            rxCount++;
        }
        ring[slot].len = uint16_t(len);
        std::memcpy(ring[slot].data, frame, len);
        framesReceived.fetch_add(1, std::memory_order_relaxed);
    }

    // Punching and keepalives: every candidate of every peer is pinged (fast while no path is
    // known, every 2 s after); a path that stops answering is dropped (the relay carries on).
    void p2pTick(int64_t now) {
        std::lock_guard<std::mutex> lock(relayMu);
        const bool on = p2pEnabled() && inGroup.load();
        if (on && !p2pWasOn)
            for (auto& p : peers)
                for (auto& c : p.cands) c.since = now;  // (punch fast again)
        p2pWasOn = on;
        for (auto& p : peers) {
            for (auto& c : p.cands) {
                if (c.works && (!on || now - c.lastPong > kPathLostMs)) {
                    c.works = false;
                    c.since = now;
                }
                if (!on) continue;
                const int64_t every = c.works ? kKeepaliveMs : now - c.since < 10000 ? kPunchMs : kKeepaliveMs;
                if (now - c.lastPing >= every) sendPing(p, c, now);
            }
            pickBest(p);
        }
    }

    // This console's LAN addresses (IPv4), with the UDP socket's port.
    void gatherLan() {
        lanCands.clear();
        if (udp == kBad || udpFamily != AF_INET) return;
        sockaddr_storage local{};
        socklen len = sizeof local;
        if (getsockname(udp, reinterpret_cast<sockaddr*>(&local), &len) != 0) return;
        const uint16_t port = ntohs(reinterpret_cast<sockaddr_in&>(local).sin_port);
        const bool masterLocal = masterAddr.ss_family == AF_INET &&
                                 (ntohl(reinterpret_cast<sockaddr_in&>(masterAddr).sin_addr.s_addr) >> 24) == 127;
        auto add = [&](in_addr ip) {
            const uint32_t h = ntohl(ip.s_addr);
            if (!h || ((h >> 24) == 127 && !masterLocal) || (h >> 16) == 0xA9FE) return;  // none, loopback, link-local
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr = ip;
            a.sin_port = htons(port);
            sockaddr_storage st{};
            std::memcpy(&st, &a, sizeof a);
            for (auto& x : lanCands)
                if (sameAddr(x, st)) return;
            if (lanCands.size() < 4) lanCands.push_back(st);
        };
        // The interface toward the master, and toward the internet (a UDP "connect" sends nothing).
        sockaddr_in toward[2]{};
        toward[0] = reinterpret_cast<const sockaddr_in&>(masterAddr);
        toward[1].sin_family = AF_INET;
        toward[1].sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &toward[1].sin_addr);
        for (auto& t : toward) {
            sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == kBad) continue;
            sockaddr_in me{};
            socklen ml = sizeof me;
            if (::connect(s, reinterpret_cast<const sockaddr*>(&t), sizeof t) == 0 &&
                getsockname(s, reinterpret_cast<sockaddr*>(&me), &ml) == 0)
                add(me.sin_addr);
            closeSock(s);
        }
        // Every address of this computer's name.
        char name[256];
        if (gethostname(name, sizeof name) == 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(name, nullptr, &hints, &res) == 0) {
                for (addrinfo* ai = res; ai; ai = ai->ai_next) add(reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr);
                freeaddrinfo(res);
            }
        }
    }

    // Tells the master this console's MAC and where it can be reached (fire and forget).
    void sendPeerInfo() {
        uint8_t mac[6];
        myMac(mac);
        std::string list = "[";
        auto add = [&](const char* type, const sockaddr_storage& a) {
            uint16_t port = 0;
            const std::string ip = ipText(a, &port);
            if (list.size() > 1) list += ',';
            list += "{\"type\":\"";
            list += type;
            list += "\",\"ip\":\"" + ip + "\",\"port\":" + std::to_string(port) + "}";
        };
        for (auto& a : lanCands) add("lan", a);
        if (havePublic) add("public", publicAddr);
        list += ']';
        sendLine(Msg("peer_info").str("mac", hex(mac, 6)).raw("candidates", list).finish(0));
        log("[online] reachable at %s\n", list.c_str());
    }

    // ---- STUN (RFC 5389 Binding): this console's public address, as the internet sees its socket

    static std::vector<sockaddr_storage> resolveStun(std::string list) {
        std::vector<sockaddr_storage> out;
        size_t start = 0;
        while (start < list.size()) {
            size_t comma = list.find(',', start);
            if (comma == std::string::npos) comma = list.size();
            std::string host, port;
            splitAddress(list.substr(start, comma - start), host, port);
            if (port == std::to_string(kDefaultPort) && list.substr(start, comma - start).find(':') == std::string::npos)
                port = "3478";
            start = comma + 1;
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (host.empty() || getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) continue;
            sockaddr_storage a{};
            std::memcpy(&a, res->ai_addr, std::min(sizeof a, size_t(res->ai_addrlen)));
            freeaddrinfo(res);
            out.push_back(a);
        }
        return out;
    }

    void stunTick(int64_t now) {
        if (!cur.inLobby || udp == kBad || udpFamily != AF_INET) return;
        const std::string list = settings::onlineStun.get();
        if (list != stunFor && !stunResolve.valid()) {
            stunFor = list;
            stunServers.clear();
            if (!list.empty()) stunResolve = std::async(std::launch::async, resolveStun, list);
        }
        if (stunResolve.valid() && stunResolve.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            stunServers = stunResolve.get();
            stunNext = now;
            stunTries = 0;
        }
        if (stunServers.empty() || now < stunNext) return;
        randomBytes(stunTx, sizeof stunTx);
        uint8_t req[20] = {0x00, 0x01, 0x00, 0x00};
        wr32be(req + 4, kStunCookie);
        std::memcpy(req + 8, stunTx, sizeof stunTx);
        {
            std::lock_guard<std::mutex> lock(relayMu);
            udpSendLocked(stunServers[stunIdx++ % stunServers.size()], req, sizeof req);
        }
        stunWaiting = true;
        if (++stunTries >= 6) {  // nobody answers: try again in a while
            stunTries = 0;
            stunNext = now + 30000;
        } else {
            stunNext = now + 500;
        }
    }

    bool stunAnswer(const uint8_t* d, size_t n) {
        if (n < 20 || d[0] != 0x01 || d[1] != 0x01 || rd32be(d + 4) != kStunCookie) return false;
        if (!stunWaiting || std::memcmp(d + 8, stunTx, sizeof stunTx) != 0) return true;  // (a stale answer)
        const size_t end = std::min(n, size_t(20) + (size_t(d[2]) << 8 | d[3]));
        for (size_t off = 20; off + 4 <= end;) {
            const unsigned type = unsigned(d[off]) << 8 | d[off + 1];
            const size_t alen = size_t(d[off + 2]) << 8 | d[off + 3];
            const uint8_t* v = d + off + 4;
            if (off + 4 + alen > end) break;
            if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {  // (XOR-)MAPPED-ADDRESS, IPv4
                const bool x = type == 0x0020;
                const uint16_t port = uint16_t((unsigned(v[2]) << 8 | v[3]) ^ (x ? 0x2112 : 0));
                const uint32_t ip = rd32be(v + 4) ^ (x ? kStunCookie : 0);
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_port = htons(port);
                a.sin_addr.s_addr = htonl(ip);
                sockaddr_storage st{};
                std::memcpy(&st, &a, sizeof a);
                stunWaiting = false;
                stunTries = 0;
                stunNext = nowMs() + 30000;  // (mappings change: look again now and then)
                if (!havePublic || !sameAddr(st, publicAddr)) {
                    havePublic = true;
                    publicAddr = st;
                    cur.publicAddress = true;
                    peerInfoDirty = true;
                    changed();
                    uint16_t p = 0;
                    const std::string text = ipText(st, &p);
                    log("[online] public address %s:%u (STUN)\n", text.c_str(), unsigned(p));
                }
                return true;
            }
            off += 4 + ((alen + 3) & ~size_t(3));
        }
        return true;
    }

    // Members' paths and round trips for the Snapshot (now and then, or when they changed).
    void refreshPaths(int64_t now, bool force = false) {
        if (!pathsDirty.exchange(false) && !force && now - pathsAt < 1000) return;
        pathsAt = now;
        const std::string me = lower(cur.me.username);
        int myPing = -1;
        for (auto& m : cur.lobby.members)
            if (lower(m.name) == me) myPing = m.pingMs;
        bool diff = false;
        std::lock_guard<std::mutex> lock(relayMu);
        for (auto& m : cur.lobby.members) {
            Path path = Path::None;
            int rtt = -1;
            if (m.player && cur.player && lower(m.name) != me) {
                path = Path::Relay;
                // (estimated: their trip to the master and ours; until those are known, ours twice)
                const int relayRtt = rttMs.load();
                rtt = m.pingMs >= 0 && myPing >= 0 ? m.pingMs + myPing : relayRtt >= 0 ? 2 * relayRtt : -1;
                if (const Peer* p = findPeer(m.peerId); p && p->best >= 0) {
                    const Cand& c = p->cands[size_t(p->best)];
                    path = c.kind == 0 ? Path::DirectLan : Path::DirectInternet;
                    rtt = c.rtt;
                }
            }
            if (path != m.path || rtt != m.peerRttMs) {
                m.path = path;
                m.peerRttMs = rtt;
                diff = true;
            }
        }
        if (diff) changed();
    }

    // The master's match start.
    void matchStarts(const Json& m) {
        MatchStart s;
        s.id = uint32_t(m["id"].num());
        s.seed = uint32_t(m["seed"].num());
        s.startAtUnixMs = m["start_at"].num();
        const int rtt = rttMs.load();
        s.startAtMs = nowMs() + m["start_in_ms"].num() - (rtt > 0 ? rtt / 2 : 0);
        s.mode = parseMode(m["mode"].str());
        s.settingsJson = jsonText(m["settings"]);
        const std::string me = lower(cur.me.username);
        for (auto& p : m["players"].list()) {
            MatchStart::Console c;
            c.console = int(p["console"].num());
            c.name = p["name"].str();
            c.peerId = uint32_t(p["peer"].num());
            std::vector<uint8_t> mac;
            if (unhex(p["mac"].str(), mac) && mac.size() == 6) std::memcpy(c.mac, mac.data(), 6);
            c.me = lower(c.name) == me;
            if (c.me) s.myConsole = c.console;
            s.consoles.push_back(std::move(c));
        }
        cur.match = std::move(s);
        cur.matchSerial++;
        changed();
        log("[online] match %u starts: %zu consoles, this one is %d\n", cur.match.id, cur.match.consoles.size(),
            cur.match.myConsole);
    }

    // A message from the relay (a datagram, or one from the stream).
    void handleRelayMessage(const uint8_t* d, size_t n, bool viaTcp) {
        if (n < kRelayHeader || d[1] != kRelayVersion) return;
        if (d[0] == kRelayFrame) {
            size_t len = n - kRelayHeader;
            if (len < kFrameMin || len > kFrameMax || !inGroup.load(std::memory_order_relaxed)) return;
            pushFrame(d + kRelayHeader, len);
        } else if (d[0] == kRelayHelloAck && n >= kRelayHeader + 4) {
            if (viaTcp != relayTcp.load()) return;  // a late answer by the transport left behind
            const uint32_t echo = uint32_t(d[8]) << 24 | uint32_t(d[9]) << 16 | uint32_t(d[10]) << 8 | d[11];
            const int64_t now = nowMs();
            int rtt = int(uint32_t(now) - echo);
            if (rtt >= 0 && rtt < 10000) {
                int old = rttMs.load();
                rttMs = old < 0 || !ackedOnTransport ? rtt : (old * 3 + rtt) / 4;
            }
            lastAck = now;
            everAcked = true;
            ackedOnTransport = true;
        }
    }

    // ================================================================================================
    // Relay over TCP (when UDP does not get through)
    // ================================================================================================

    bool streamPending() {
        std::lock_guard<std::mutex> lock(relayMu);
        return streamOutHead < streamOut.size();
    }

    // Queues one whole message (length prefix included) for the stream; relayMu held. Sends at once
    // when nothing is waiting (the usual case); never blocks; drops the message when the stream is
    // backed up past kStreamQueue (old LAN frames are worthless).
    bool streamWriteLocked(const uint8_t* d, size_t n) {
        if (stream == kBad || !streamReady || streamBroken) return false;
        if (streamOutHead == streamOut.size()) {
            streamOut.clear();
            streamOutHead = 0;
            int k = ::send(stream, reinterpret_cast<const char*>(d), int(n), kSendFlags);
            if (k == int(n)) return true;
            if (k < 0) {
                if (!wouldBlock(lastError())) {
                    streamBroken = true;
                    wake();
                    return false;
                }
                k = 0;
            }
            streamOut.insert(streamOut.end(), d + k, d + n);
            wake();  // the network thread sends the rest when the socket takes it
            return true;
        }
        if (streamOut.size() - streamOutHead + n > kStreamQueue) return false;
        streamOut.insert(streamOut.end(), d, d + n);
        return true;
    }

    void flushStream() {
        std::lock_guard<std::mutex> lock(relayMu);
        while (stream != kBad && streamOutHead < streamOut.size()) {
            int k = ::send(stream, reinterpret_cast<const char*>(streamOut.data() + streamOutHead),
                           int(streamOut.size() - streamOutHead), kSendFlags);
            if (k > 0) {
                streamOutHead += size_t(k);
                continue;
            }
            if (k < 0 && wouldBlock(lastError())) break;
            streamBroken = true;
            break;
        }
        if (streamOutHead == streamOut.size()) {
            streamOut.clear();
            streamOutHead = 0;
        } else if (streamOutHead > kStreamQueue / 2) {
            streamOut.erase(streamOut.begin(), streamOut.begin() + std::ptrdiff_t(streamOutHead));
            streamOutHead = 0;
        }
    }

    void openStream(int64_t now) {
        closeStream();
        streamNextAttempt = now + 1000;  // if this fails
        sock_t s = socket(masterAddr.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == kBad) return;
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        if (!nonBlocking(s) || (::connect(s, reinterpret_cast<const sockaddr*>(&masterAddr), addrLen(masterAddr)) != 0 &&
                                !inProgress(lastError()))) {
            closeSock(s);
            return;
        }
        std::lock_guard<std::mutex> lock(relayMu);
        stream = s;
        streamConnecting = true;
        streamStart = now;
    }

    void finishStream() {
        int err = 0;
        socklen len = sizeof err;
        getsockopt(stream, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err) {
            closeStream();
            return;
        }
        streamConnecting = false;
        {
            std::lock_guard<std::mutex> lock(relayMu);
            streamReady = true;
            streamWriteLocked(kStreamMagic, sizeof kStreamMagic);
        }
        sendHello(nowMs());
        log("[online] relay stream connected\n");
    }

    void closeStream() {
        {
            std::lock_guard<std::mutex> lock(relayMu);
            if (stream != kBad) closeSock(stream);
            stream = kBad;
            streamReady = false;
            streamBroken = false;
            streamOut.clear();
            streamOutHead = 0;
        }
        streamConnecting = false;
        streamIn.clear();
        streamInHead = 0;
    }

    void readStream() {
        uint8_t buf[16384];
        for (;;) {
            int n = recv(stream, reinterpret_cast<char*>(buf), sizeof buf, 0);
            if (n > 0) {
                streamIn.insert(streamIn.end(), buf, buf + n);
                if (size_t(n) < sizeof buf) break;
                continue;
            }
            if (n < 0 && wouldBlock(lastError())) break;
            log("[online] relay stream closed\n");
            closeStream();  // chooseTransport() connects it again
            return;
        }
        while (streamIn.size() - streamInHead >= 2) {
            const uint8_t* p = streamIn.data() + streamInHead;
            const size_t len = size_t(p[0]) << 8 | p[1];
            if (len < kRelayHeader || len > kRelayHeader + kFrameMax) {
                closeStream();
                return;
            }
            if (streamIn.size() - streamInHead < 2 + len) break;
            handleRelayMessage(p + 2, len, true);
            streamInHead += 2 + len;
        }
        if (streamInHead == streamIn.size()) {
            streamIn.clear();
            streamInHead = 0;
        } else if (streamInHead > 32 * 1024) {
            streamIn.erase(streamIn.begin(), streamIn.begin() + std::ptrdiff_t(streamInHead));
            streamInHead = 0;
        }
    }

    // Which transport the relay uses: UDP, unless forced to TCP or UDP got no answer in time.
    void chooseTransport(int64_t now) {
        const int ov = tcpOverride.load();
        const bool forced = ov >= 0 ? ov != 0 : settings::onlineRelayTcp.load();
        const bool udpDead = udpFailedFor == address;
        bool want = relayTcp.load();
        if (forced) {
            want = true;
        } else if (want && tcpForced) {
            want = udpDead;  // no longer forced: back to UDP (unless it is known not to work)
        } else if (!want && serverRelayTcp && !ackedOnTransport && (udpDead || now - udpTrialStart >= kUdpTrialMs)) {
            want = true;
            if (!udpDead) {
                udpFailedFor = address;
                log("[online] the relay does not answer over UDP: using TCP\n");
            }
        }
        if (want != relayTcp.load()) {
            if (want) {
                tcpForced = forced;
                streamNextAttempt = 0;
            } else {
                closeStream();
                tcpForced = false;
                udpTrialStart = now;
            }
            relayTcp = want;
            ackedOnTransport = false;
            lastHello = 0;
            log("[online] relay over %s%s\n", want ? "TCP" : "UDP", want && forced ? " (forced)" : "");
            changed();
        }
        if (relayTcp.load()) {
            bool broken;
            {
                std::lock_guard<std::mutex> lock(relayMu);
                broken = streamBroken;
            }
            if (broken || (streamConnecting && now - streamStart > 5000)) {
                closeStream();
                streamNextAttempt = now + 1000;
            }
            if (stream == kBad && now >= streamNextAttempt) openStream(now);
        }
    }

    void clearRing() {
        std::lock_guard<std::mutex> lock(rxMu);
        rxHead = rxCount = 0;
    }

    // Game thread. Each frame goes to each console that should get it exactly once: peer to peer
    // where a direct path works, else through the relay (FRAME_TO naming those consoles; a plain
    // FRAME - "everyone" - while the peers are not known yet, or peer to peer is off).
    void relaySend(const uint8_t* frame, size_t len) {
        if (len < kFrameMin || len > kFrameMax || !inGroup.load(std::memory_order_relaxed)) return;
        uint8_t msg[2 + kRelayHeader + 1 + 4 * kRelayTargetsMax + kFrameMax];  // (the length prefix is for the stream)
        uint8_t* d = msg + 2;
        std::lock_guard<std::mutex> lock(relayMu);
        if (!relayId) return;
        bool sent = false;
        size_t head = kRelayHeader;  // the relay message's header size
        if (p2pEnabled() && !peers.empty()) {
            Peer* owner = nullptr;  // a unicast frame goes to the console with that MAC only
            if (!(frame[0] & 1))
                for (auto& p : peers)
                    if (p.hasMac && std::memcmp(p.mac, frame, 6) == 0) owner = &p;
            uint8_t direct[kRelayHeader + kFrameMax];
            bool built = false;
            int n = 0;
            for (auto& p : peers) {
                if (owner && &p != owner) continue;
                if (p.best >= 0) {
                    if (!built) {
                        p2pHeader(direct, kP2pFrame);
                        std::memcpy(direct + kRelayHeader, frame, len);
                        built = true;
                    }
                    sent = udpSendLocked(p.cands[size_t(p.best)].addr, direct, kRelayHeader + len) > 0 || sent;
                } else if (n < kRelayTargetsMax) {
                    wr32be(d + kRelayHeader + 1 + 4 * n, p.id);
                    n++;
                }
            }
            if (!n) {
                if (sent) framesSent.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            d[0] = kRelayFrameTo;
            d[8] = uint8_t(n);
            head = kRelayHeader + 1 + 4 * size_t(n);
        } else {
            d[0] = kRelayFrame;
        }
        d[1] = kRelayVersion;
        d[2] = d[3] = 0;
        wr32be(d + 4, relayId);
        std::memcpy(d + head, frame, len);
        const size_t total = head + len;
        bool relayed = false;
        if (relayTcp.load(std::memory_order_relaxed)) {
            msg[0] = uint8_t(total >> 8);
            msg[1] = uint8_t(total);
            relayed = streamWriteLocked(msg, total + 2);  // never blocks: sent now, queued, or dropped
        } else {
            relayed = udpSendLocked(udpAddr, d, total) > 0;
        }
        if (sent || relayed) framesSent.fetch_add(1, std::memory_order_relaxed);
    }

    // Game thread.
    bool relayPoll(std::vector<uint8_t>& frame) {
        std::lock_guard<std::mutex> lock(rxMu);
        if (!rxCount) return false;
        const Slot& s = ring[rxHead];
        frame.assign(s.data, s.data + s.len);
        rxHead = (rxHead + 1) % kRing;
        rxCount--;
        return true;
    }

    // ================================================================================================
    // Timers
    // ================================================================================================

    void tick(int64_t now) {
        if (job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            Verifier v = job.get();
            auto done = std::move(jobDone);
            jobDone = nullptr;
            changed();
            if (done) done(jobOk, v);
            auto next = std::move(afterJob);
            afterJob.clear();
            for (auto& fn : next) fn();
        }
        if (phase == Phase::Connecting && now - phaseStart > 4000)  // per address
            nextAddress("The online service did not answer.");
        if (phase == Phase::Hello && now - phaseStart > 8000) {
            if (reconnecting) closeTcp();
            else connectFailed("The online service did not answer.");
        }
        if (phase == Phase::Ready && now - lastRx > 20000)  // the server pings every 5 s
            connectionLost("Lost connection to the online service.", true);
        if (reconnecting && tcp == kBad && now >= nextAttempt) {
            if (now > reconnectUntil) {
                reconnecting = false;
                signedOut();
                setState(State::Offline);
                fail("Lost connection to the online service.");
            } else {
                nextAttempt = now + 2000;
                openTcp();
            }
        }
        if (relayKnown) {
            chooseTransport(now);
            if (now - lastHello >= (ackedOnTransport ? 1000 : 250)) sendHello(now);
            if (cur.inLobby && phase == Phase::Ready && udp != kBad && peerInfoDirty) {
                peerInfoDirty = false;
                if (lanCands.empty()) gatherLan();
                sendPeerInfo();
            }
            stunTick(now);
            p2pTick(now);
            refreshPaths(now);
            bool allDirect;
            {
                std::lock_guard<std::mutex> lock(relayMu);
                allDirect = !peers.empty() && p2pEnabled();
                for (auto& p : peers) allDirect = allDirect && p.best >= 0;
            }
            const bool up = inGroup.load() && ((everAcked && now - lastAck < 10000) || allDirect);
            if (up != link.load()) {
                link = up;
                log("[online] link %s (relay over %s)\n", up ? "up" : "down", relayTcp.load() ? "TCP" : "UDP");
            }
            if (inGroup.load() && !link.load() && !everAcked && !udpWarned && now - groupSince > 5000) {
                udpWarned = true;
                log("[online] relay at %s (UDP port %u) does not answer\n", address.c_str(), unsigned(udpPort));
                fail(serverRelayTcp ? "The match connection does not answer." :
                                      "The match connection does not answer: a firewall may be blocking it.");
            }
        }
    }
};

// ================================================================================================
// Client: every call is queued for the network thread
// ================================================================================================

Client::Client(bool installLanPort) : impl_(std::make_unique<Impl>(installLanPort)) {}
Client::~Client() = default;

std::string serviceAddress() {
    const std::string server = settings::onlineServer.get();
    return server.empty() ? settings::onlineMaster.get() : server;
}

void Client::connect(const std::string& address, bool autoLogin) {
    impl_->post([i = impl_.get(), address, autoLogin] { i->startConnect(address, autoLogin); });
}

void Client::disconnect() {
    impl_->post([i = impl_.get()] {
        if (i->phase == Impl::Phase::Ready && i->cur.inLobby) i->sendLine(Msg("leave").finish(0));
        i->reconnecting = false;
        i->afterWelcome.clear();
        i->closeTcp();
        i->closeUdp();
        i->closeStream();
        i->signedOut();
        i->setState(State::Offline);
    });
}

void Client::registerAccount(const std::string& username, const std::string& password, bool remember) {
    impl_->post([i = impl_.get(), username, password, remember] { i->registerAccount(username, password, remember); });
}

void Client::login(const std::string& username, const std::string& password, bool remember) {
    impl_->post([i = impl_.get(), username, password, remember] { i->login(username, password, remember); });
}

void Client::resumeSaved() {
    impl_->post([i = impl_.get()] { i->resumeSaved(); });
}

void Client::logout() {
    impl_->post([i = impl_.get()] { i->logout(); });
}

void Client::changePassword(const std::string& newPassword) {
    impl_->post([i = impl_.get(), newPassword] { i->changePassword(newPassword); });
}

void Client::setProfile(const std::string& colour, const std::string& machine) {
    impl_->post([i = impl_.get(), colour, machine] {
        if (!i->needOnline()) return;
        i->request(Msg("profile_set").str("colour", colour).str("machine", machine), [i](const Json& r) {
            i->cur.me = parseProfile(r["profile"]);
            i->changed();
        });
    });
}

void Client::viewProfile(const std::string& username) {
    impl_->post([i = impl_.get(), username] {
        if (!i->needOnline()) return;
        i->request(Msg("profile_get").str("username", username), [i](const Json& r) {
            i->cur.viewed = parseProfile(r["profile"]);
            i->cur.viewedValid = true;
            i->changed();
        });
    });
}

void Client::reportMatch(bool won) {
    impl_->post([i = impl_.get(), won] {
        if (!i->needOnline()) return;
        i->request(Msg("report_match").flag("won", won), [i](const Json& r) {
            i->cur.me = parseProfile(r["profile"]);
            i->changed();
        });
    });
}

void Client::refreshServers(Mode filter) {
    impl_->post([i = impl_.get(), filter] {
        if (!i->needOnline()) return;
        i->request(Msg("list_servers").str("mode", modeId(filter)), [i](const Json& r) {
            i->cur.servers.clear();
            for (auto& s : r["servers"].list()) {
                ServerInfo x;
                x.id = uint32_t(s["id"].num());
                x.name = s["name"].str();
                x.host = s["host"].str();
                x.region = s["region"].str();
                x.mode = parseMode(s["mode"].str());
                x.session = parseSession(s["state"].str());
                x.maxPlayers = int(s["max_players"].num(kMatchDefault));
                x.members = int(s["members"].num());
                x.max = int(s["max"].num());
                x.locked = s["locked"].flag();
                x.playing = int(s["playing"].num());
                x.pingMs = int(s["ping"].num(-1));
                i->cur.servers.push_back(std::move(x));
            }
            i->cur.usersOnline = int(r["online"].num());
            i->cur.serversSerial++;
            i->changed();
        });
    });
}

namespace {
Msg lobbyOptions(const char* type, const HostOptions& o, bool includeEmptyPassword) {
    Msg m(type);
    m.str("name", o.name).num("max", o.maxMembers).num("max_players", o.maxPlayers).str("mode", modeId(o.mode))
        .str("region", o.region).flag("auto_start", o.autoStart).flag("migrate", o.migrateHost);
    if (!o.password.empty() || includeEmptyPassword) m.str("password", o.password);
    std::string settings;
    if (!o.settingsJson.empty() && jsonObject(o.settingsJson, settings)) m.raw("settings", settings);
    return m;
}
}  // namespace

void Client::host(const HostOptions& options) {
    impl_->post([i = impl_.get(), options] {
        if (!i->needOnline()) return;
        i->request(lobbyOptions("host", options, true), [i](const Json& r) {
            i->enterLobby(r["lobby"], r["relay"], r["chat"]);
        });
    });
}

void Client::updateServer(const HostOptions& options) {
    impl_->post([i = impl_.get(), options] {
        if (!i->needOnline()) return;
        i->request(lobbyOptions("lobby_set", options, false), [i](const Json& r) { i->applyLobby(r["lobby"]); });
    });
}

void Client::clearServerPassword() {
    impl_->post([i = impl_.get()] {
        if (!i->needOnline()) return;
        i->request(Msg("lobby_set").str("password", ""), [i](const Json& r) { i->applyLobby(r["lobby"]); });
    });
}

void Client::join(uint32_t serverId, const std::string& password) {
    impl_->post([i = impl_.get(), serverId, password] {
        if (!i->needOnline()) return;
        Msg m("join");
        m.num("lobby", serverId);
        if (!password.empty()) m.str("password", password);
        i->request(std::move(m), [i](const Json& r) { i->enterLobby(r["lobby"], r["relay"], r["chat"]); });
    });
}

void Client::leave() {
    impl_->post([i = impl_.get()] {
        if (!i->needOnline() || !i->cur.inLobby) return;
        i->request(Msg("leave"), [i](const Json&) { i->leaveLobbyLocal(); });
    });
}

void Client::kick(const std::string& username) {
    impl_->post([i = impl_.get(), username] {
        if (!i->needOnline()) return;
        i->request(Msg("kick").str("username", username), nullptr);
    });
}

void Client::startMatch(bool now) {
    impl_->post([i = impl_.get(), now] {
        if (!i->needOnline()) return;
        i->request(Msg("session_start").flag("now", now), nullptr);
    });
}

void Client::cancelMatch() {
    impl_->post([i = impl_.get()] {
        if (!i->needOnline()) return;
        i->request(Msg("session_cancel"), nullptr);
    });
}

void Client::continueSession() {
    impl_->post([i = impl_.get()] {
        if (!i->needOnline()) return;
        i->request(Msg("session_continue"), nullptr);
    });
}

void Client::endMatch(const std::string& resultsJson) {
    std::string results = "{}";
    if (!resultsJson.empty() && !jsonObject(resultsJson, results)) results = "{}";
    impl_->post([i = impl_.get(), results] {
        if (!i->needOnline()) return;
        i->request(Msg("match_end").raw("results", results), nullptr);
    });
}

void Client::setGameSettings(const std::string& json) {
    std::string settings;
    const bool ok = jsonObject(json, settings) && settings.size() <= size_t(kSettingsMax);
    impl_->post([i = impl_.get(), ok, settings] {
        if (!ok) return i->fail("Those game settings are not valid.");
        if (!i->needOnline()) return;
        i->request(Msg("settings_set").raw("settings", settings), nullptr);
    });
}

void Client::enableP2p(bool on) {
    impl_->p2pOverride = on ? 1 : 0;
    impl_->post([i = impl_.get()] { i->pathsDirty = true; });
}

void Client::chat(const std::string& text) {
    std::string t = text.substr(0, size_t(kChatMax) * 4);  // the server cuts to 200 characters
    impl_->post([i = impl_.get(), t] {
        if (!i->needOnline()) return;
        i->request(Msg("chat").str("text", t), nullptr);
    });
}

void Client::forceRelayTcp(bool force) {
    impl_->tcpOverride = force ? 1 : 0;
    impl_->post([] {});  // (the network thread looks at once)
}

void Client::clearError() {
    impl_->post([i = impl_.get()] {
        i->cur.error.clear();
        i->changed();
    });
}

bool Client::snapshot(Snapshot& out) const {
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(impl_->snapMu);
        if (out.serial != impl_->pub.serial) {
            out = impl_->pub;
            fresh = true;
        }
    }
    out.linkUp = impl_->link.load(std::memory_order_relaxed);
    out.relayTcp = impl_->relayTcp.load(std::memory_order_relaxed);
    out.pingMs = impl_->rttMs.load(std::memory_order_relaxed);
    out.p2p = impl_->p2pEnabled();
    out.framesSent = impl_->framesSent.load(std::memory_order_relaxed);
    out.framesReceived = impl_->framesReceived.load(std::memory_order_relaxed);
    return fresh;
}

Snapshot Client::snapshot() const {
    Snapshot s;
    s.serial = ~uint64_t(0);
    snapshot(s);
    return s;
}

lan::Port& Client::port() { return impl_->portObj; }

void detail::simulateConnectionLoss(Client& client) {
    Client::Impl* i = client.impl_.get();
    i->post([i] { i->connectionLost("Connection dropped (test).", true); });
}

void detail::setUdpGateway(Client& client, const std::string& hostPort) {
    Client::Impl* i = client.impl_.get();
    std::string host, port;
    splitAddress(hostPort, host, port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return;
    std::lock_guard<std::mutex> lock(i->relayMu);
    i->gatewayAddr = sockaddr_storage{};
    std::memcpy(&i->gatewayAddr, res->ai_addr, std::min(sizeof i->gatewayAddr, size_t(res->ai_addrlen)));
    i->gatewayOn = true;
    freeaddrinfo(res);
}

void detail::setMacAddress(Client& client, const uint8_t mac[6]) {
    Client::Impl* i = client.impl_.get();
    std::lock_guard<std::mutex> lock(i->relayMu);
    std::memcpy(i->macOverride, mac, 6);
    i->macSet = true;
}

// ================================================================================================
// The game's client
// ================================================================================================

namespace {
std::mutex g_clientMu;
std::unique_ptr<Client> g_client;
}  // namespace

Client& client() {
    std::lock_guard<std::mutex> lock(g_clientMu);
    if (!g_client) g_client = std::make_unique<Client>(true);
    return *g_client;
}

void shutdown() {
    std::unique_ptr<Client> c;
    {
        std::lock_guard<std::mutex> lock(g_clientMu);
        c = std::move(g_client);
    }
    c.reset();  // leaves the lobby, closes the connection, joins the thread
}

}  // namespace rt::airnet
