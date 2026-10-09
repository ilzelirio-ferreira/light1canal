#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Byte limits are the Matter string limits, not browser character counts.
inline bool portal_text(const char *s, size_t max_bytes, bool allow_empty = false) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n > max_bytes || (!allow_empty && n == 0)) return false;
    for (size_t i = 0; i < n;) {
        uint8_t c = static_cast<uint8_t>(s[i++]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        unsigned tails;
        uint32_t cp;
        if (c >= 0xc2 && c <= 0xdf) { tails = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { tails = 2; cp = c & 0xf; }
        else if (c >= 0xf0 && c <= 0xf4) { tails = 3; cp = c & 7; }
        else return false;
        if (i + tails > n) return false;
        for (unsigned j = 0; j < tails; ++j) {
            c = static_cast<uint8_t>(s[i++]);
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3f);
        }
        if ((tails == 1 && cp < 0x80) || (tails == 2 && cp < 0x800) ||
            (tails == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

inline bool portal_key(const char *s) {
    if (!s || strlen(s) < 8 || strlen(s) > 63) return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p)
        if (*p < 0x20 || *p > 0x7e) return false;
    return true;
}

inline bool portal_key_matches(const char *provided, size_t length, const char *expected) {
    if (!provided || !expected || length == 0 || length > 63) return false;
    const size_t expected_length = strlen(expected);
    unsigned diff = length ^ expected_length;
    for (size_t i = 0; i < length; ++i)
        diff |= static_cast<unsigned char>(provided[i] ^ (i < expected_length ? expected[i] : 0));
    return diff == 0;
}

// Minimal DNS: one uncompressed IN question. All A names point to the AP.
inline size_t portal_dns_reply(uint8_t *b, size_t n, size_t capacity, const uint8_t ip[4]) {
    if (n > capacity || n < 12 || (b[2] & 0xf8) || b[4] != 0 || b[5] != 1) return 0;
    size_t p = 12;
    while (p < n && b[p]) {
        unsigned len = b[p++];
        if (len > 63 || p + len >= n) return 0;
        p += len;
    }
    if (p >= n || p + 5 > n) return 0;
    ++p;
    unsigned type = (b[p] << 8) | b[p+1];
    unsigned klass = (b[p+2] << 8) | b[p+3];
    p += 4;
    if (klass != 1) return 0;
    bool a = type == 1;
    if (p + (a ? 16 : 0) > capacity) return 0;
    b[2] = 0x80 | (b[2] & 1); b[3] = 0x80;
    b[6] = 0; b[7] = a ? 1 : 0;
    b[8] = b[9] = b[10] = b[11] = 0;
    if (!a) return p;
    const uint8_t answer[12] = {0xc0,0x0c,0,1,0,1,0,0,0,30,0,4};
    memcpy(b+p, answer, 12); memcpy(b+p+12, ip, 4);
    return p + 16;
}
