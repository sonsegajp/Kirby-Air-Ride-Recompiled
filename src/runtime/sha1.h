// SHA-1 (FIPS 180-4), for checking the program on the player's disc against the known one (src/runtime/setup.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace rt {

class Sha1 {
public:
    void add(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        m_len += n;
        if (m_fill) {
            const size_t k = n < 64 - m_fill ? n : 64 - m_fill;
            std::memcpy(m_buf + m_fill, p, k);
            m_fill += k, p += k, n -= k;
            if (m_fill < 64) return;
            block(m_buf), m_fill = 0;
        }
        for (; n >= 64; p += 64, n -= 64) block(p);
        std::memcpy(m_buf, p, n);
        m_fill = n;
    }
    std::string hex() {
        const uint64_t bits = m_len * 8;
        const uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (m_fill != 56) add(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        add(len, 8);
        static const char* d = "0123456789abcdef";
        std::string s;
        for (uint32_t v : m_h)
            for (int i = 28; i >= 0; i -= 4) s += d[(v >> i) & 15];
        return s;
    }

private:
    static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        m_h[0] += a, m_h[1] += b, m_h[2] += c, m_h[3] += d, m_h[4] += e;
    }
    uint32_t m_h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t m_buf[64];
    size_t m_fill = 0;
    uint64_t m_len = 0;
};

}  // namespace rt
