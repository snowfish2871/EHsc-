// ============================================================================
//  EHsc · hashes.cpp
//  纯 C++ 实现：CRC32 / MD5 / SHA-1 / SHA-256 / SHA-384 / SHA-512 /
//               SHA3-256 / SHA3-512 / BLAKE2s-256 / BLAKE2b-512
//
//  参考标准：
//    * CRC-32/ISO-HDLC (PKZIP)      —— 反射多项式 0xEDB88320
//    * MD5        RFC 1321
//    * SHA-1      RFC 3174 / FIPS 180-4
//    * SHA-2 系列 FIPS 180-4
//    * SHA-3 系列 FIPS 202（Keccak-f[1600] 海绵结构）
//    * BLAKE2s/b  RFC 7693（未加盐、未加密钥、默认参数块）
//
//  所有实现均为流式状态机：数据可以任意大小分块喂入，结果与一次性计算一致。
// ============================================================================
#include "hashes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>

namespace ehsc {
namespace {

// ============================================================ 基础位运算 ====
inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint64_t rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }
inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

inline uint32_t load32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint32_t load32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline uint64_t load64le(const uint8_t* p) {
    return static_cast<uint64_t>(load32le(p)) | (static_cast<uint64_t>(load32le(p + 4)) << 32);
}
inline uint64_t load64be(const uint8_t* p) {
    return (static_cast<uint64_t>(load32be(p)) << 32) | static_cast<uint64_t>(load32be(p + 4));
}
inline void store32le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
inline void store32be(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
inline void store64be(uint8_t* p, uint64_t v) {
    store32be(p, static_cast<uint32_t>(v >> 32));
    store32be(p + 4, static_cast<uint32_t>(v));
}

// ================================================================== CRC32 ===
// 生成 256 项查表（首次调用时线程安全地初始化一次）
const std::array<uint32_t, 256>& crc32Table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

void crc32Init(detail::Crc32State& s) { s.crc = 0xFFFFFFFFu; }

void crc32Update(detail::Crc32State& s, const uint8_t* data, size_t len) {
    const std::array<uint32_t, 256>& t = crc32Table();
    uint32_t crc = s.crc;
    for (size_t i = 0; i < len; ++i) crc = t[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    s.crc = crc;
}

void crc32Final(detail::Crc32State& s, uint8_t* out) {
    uint32_t v = s.crc ^ 0xFFFFFFFFu;
    store32be(out, v);  // 与 zlib / PKZIP 一致：按大端输出
}

// ==================================================================== MD5 ===
constexpr uint32_t kMd5K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};

constexpr int kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                           5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                           4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                           6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

void md5Init(detail::Md5State& s) {
    s.h[0] = 0x67452301u;
    s.h[1] = 0xefcdab89u;
    s.h[2] = 0x98badcfeu;
    s.h[3] = 0x10325476u;
    s.total = 0;
    s.buflen = 0;
}

void md5Block(detail::Md5State& s, const uint8_t* p) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = load32le(p + 4 * i);
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + kMd5K[i] + m[g], kMd5S[i]);
        a = tmp;
    }
    s.h[0] += a;
    s.h[1] += b;
    s.h[2] += c;
    s.h[3] += d;
}

void md5Update(detail::Md5State& s, const uint8_t* data, size_t len) {
    s.total += len;
    if (s.buflen) {
        const size_t need = 64 - s.buflen;
        const size_t take = (len < need) ? len : need;
        std::memcpy(s.buf + s.buflen, data, take);
        s.buflen += take;
        data += take;
        len -= take;
        if (s.buflen == 64) {
            md5Block(s, s.buf);
            s.buflen = 0;
        }
    }
    while (len >= 64) {
        md5Block(s, data);
        data += 64;
        len -= 64;
    }
    if (len) {
        std::memcpy(s.buf, data, len);
        s.buflen = len;
    }
}

void md5Final(detail::Md5State& s, uint8_t* out) {
    const uint64_t bits = s.total * 8u;
    uint8_t pad[72];
    const size_t padLen = (s.buflen < 56) ? (56 - s.buflen) : (120 - s.buflen);
    pad[0] = 0x80;
    std::memset(pad + 1, 0, padLen - 1);
    md5Update(s, pad, padLen);
    uint8_t lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = static_cast<uint8_t>(bits >> (8 * i));  // 小端
    md5Update(s, lenb, 8);
    for (int i = 0; i < 4; ++i) store32le(out + 4 * i, s.h[i]);
}

// ================================================================== SHA-1 ===
void sha1Init(detail::Sha1State& s) {
    s.h[0] = 0x67452301u;
    s.h[1] = 0xEFCDAB89u;
    s.h[2] = 0x98BADCFEu;
    s.h[3] = 0x10325476u;
    s.h[4] = 0xC3D2E1F0u;
    s.total = 0;
    s.buflen = 0;
}

void sha1Block(detail::Sha1State& s, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load32be(p + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = tmp;
    }
    s.h[0] += a;
    s.h[1] += b;
    s.h[2] += c;
    s.h[3] += d;
    s.h[4] += e;
}

void sha1Update(detail::Sha1State& s, const uint8_t* data, size_t len) {
    s.total += len;
    if (s.buflen) {
        const size_t need = 64 - s.buflen;
        const size_t take = (len < need) ? len : need;
        std::memcpy(s.buf + s.buflen, data, take);
        s.buflen += take;
        data += take;
        len -= take;
        if (s.buflen == 64) {
            sha1Block(s, s.buf);
            s.buflen = 0;
        }
    }
    while (len >= 64) {
        sha1Block(s, data);
        data += 64;
        len -= 64;
    }
    if (len) {
        std::memcpy(s.buf, data, len);
        s.buflen = len;
    }
}

void sha1Final(detail::Sha1State& s, uint8_t* out) {
    const uint64_t bits = s.total * 8u;
    uint8_t pad[72];
    const size_t padLen = (s.buflen < 56) ? (56 - s.buflen) : (120 - s.buflen);
    pad[0] = 0x80;
    std::memset(pad + 1, 0, padLen - 1);
    sha1Update(s, pad, padLen);
    uint8_t lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));  // 大端
    sha1Update(s, lenb, 8);
    for (int i = 0; i < 5; ++i) store32be(out + 4 * i, s.h[i]);
}

// ================================================================ SHA-256 ===
constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

void sha256Init(detail::Sha256State& s) {
    s.h[0] = 0x6a09e667u;
    s.h[1] = 0xbb67ae85u;
    s.h[2] = 0x3c6ef372u;
    s.h[3] = 0xa54ff53au;
    s.h[4] = 0x510e527fu;
    s.h[5] = 0x9b05688cu;
    s.h[6] = 0x1f83d9abu;
    s.h[7] = 0x5be0cd19u;
    s.total = 0;
    s.buflen = 0;
}

void sha256Block(detail::Sha256State& s, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = load32be(p + 4 * i);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3];
    uint32_t e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
        const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s.h[0] += a;
    s.h[1] += b;
    s.h[2] += c;
    s.h[3] += d;
    s.h[4] += e;
    s.h[5] += f;
    s.h[6] += g;
    s.h[7] += h;
}

void sha256Update(detail::Sha256State& s, const uint8_t* data, size_t len) {
    s.total += len;
    if (s.buflen) {
        const size_t need = 64 - s.buflen;
        const size_t take = (len < need) ? len : need;
        std::memcpy(s.buf + s.buflen, data, take);
        s.buflen += take;
        data += take;
        len -= take;
        if (s.buflen == 64) {
            sha256Block(s, s.buf);
            s.buflen = 0;
        }
    }
    while (len >= 64) {
        sha256Block(s, data);
        data += 64;
        len -= 64;
    }
    if (len) {
        std::memcpy(s.buf, data, len);
        s.buflen = len;
    }
}

void sha256Final(detail::Sha256State& s, uint8_t* out) {
    const uint64_t bits = s.total * 8u;
    uint8_t pad[72];
    const size_t padLen = (s.buflen < 56) ? (56 - s.buflen) : (120 - s.buflen);
    pad[0] = 0x80;
    std::memset(pad + 1, 0, padLen - 1);
    sha256Update(s, pad, padLen);
    uint8_t lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    sha256Update(s, lenb, 8);
    for (int i = 0; i < 8; ++i) store32be(out + 4 * i, s.h[i]);
}

// ========================================================= SHA-512 / SHA-384 =
constexpr uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

constexpr uint64_t kSha512H0[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
                                   0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                   0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                   0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

constexpr uint64_t kSha384H0[8] = {0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL,
                                   0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                                   0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
                                   0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};

void sha512Init(detail::Sha512State& s, bool sha384) {
    for (int i = 0; i < 8; ++i) s.h[i] = sha384 ? kSha384H0[i] : kSha512H0[i];
    s.totalLo = 0;
    s.totalHi = 0;
    s.buflen = 0;
}

void sha512Block(detail::Sha512State& s, const uint8_t* p) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load64be(p + 8 * i);
    for (int i = 16; i < 80; ++i) {
        const uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        const uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3];
    uint64_t e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
    for (int i = 0; i < 80; ++i) {
        const uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        const uint64_t ch = (e & f) ^ (~e & g);
        const uint64_t t1 = h + S1 + ch + kSha512K[i] + w[i];
        const uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint64_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s.h[0] += a;
    s.h[1] += b;
    s.h[2] += c;
    s.h[3] += d;
    s.h[4] += e;
    s.h[5] += f;
    s.h[6] += g;
    s.h[7] += h;
}

void sha512Update(detail::Sha512State& s, const uint8_t* data, size_t len) {
    // 128 位长度计数器（实际文件长度不会溢出 64 位，此处形式完整）
    const uint64_t prev = s.totalLo;
    s.totalLo += len;
    if (s.totalLo < prev) s.totalHi++;

    if (s.buflen) {
        const size_t need = 128 - s.buflen;
        const size_t take = (len < need) ? len : need;
        std::memcpy(s.buf + s.buflen, data, take);
        s.buflen += take;
        data += take;
        len -= take;
        if (s.buflen == 128) {
            sha512Block(s, s.buf);
            s.buflen = 0;
        }
    }
    while (len >= 128) {
        sha512Block(s, data);
        data += 128;
        len -= 128;
    }
    if (len) {
        std::memcpy(s.buf, data, len);
        s.buflen = len;
    }
}

void sha512Final(detail::Sha512State& s, uint8_t* out, int outBytes) {
    const uint64_t bitsLo = s.totalLo << 3;
    const uint64_t bitsHi = (s.totalHi << 3) | (s.totalLo >> 61);
    uint8_t pad[240];
    const size_t padLen = (s.buflen < 112) ? (112 - s.buflen) : (240 - s.buflen);
    pad[0] = 0x80;
    std::memset(pad + 1, 0, padLen - 1);
    sha512Update(s, pad, padLen);
    uint8_t lenb[16];
    store64be(lenb, bitsHi);
    store64be(lenb + 8, bitsLo);
    sha512Update(s, lenb, 16);
    uint8_t full[64];
    for (int i = 0; i < 8; ++i) store64be(full + 8 * i, s.h[i]);
    std::memcpy(out, full, static_cast<size_t>(outBytes));
}

// ================================================================ SHA3-256/512
// Keccak-f[1600] 轮常数
constexpr uint64_t kKeccakRC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};

constexpr int kKeccakRotc[24] = {1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
                                 27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44};
constexpr int kKeccakPiln[24] = {10, 7,  11, 17, 18, 3, 5,  16, 8,  21, 24, 4,
                                 15, 23, 19, 13, 12, 2, 20, 14, 22, 9,  6,  1};

void keccakF(uint64_t st[25]) {
    for (int round = 0; round < 24; ++round) {
        uint64_t bc[5];
        // Theta
        for (int i = 0; i < 5; ++i) bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        for (int i = 0; i < 5; ++i) {
            const uint64_t t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5) st[j + i] ^= t;
        }
        // Rho + Pi
        uint64_t t = st[1];
        for (int i = 0; i < 24; ++i) {
            const int j = kKeccakPiln[i];
            const uint64_t tmp = st[j];
            st[j] = rotl64(t, kKeccakRotc[i]);
            t = tmp;
        }
        // Chi
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; ++i) bc[i] = st[j + i];
            for (int i = 0; i < 5; ++i) st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }
        // Iota
        st[0] ^= kKeccakRC[round];
    }
}

void sha3Init(detail::Sha3State& s, int outBytes) {
    std::memset(s.s, 0, sizeof(s.s));
    std::memset(s.buf, 0, sizeof(s.buf));
    s.outBytes = outBytes;
    s.rate = static_cast<size_t>(200 - 2 * outBytes);  // SHA3-256:136  SHA3-512:72
    s.pt = 0;
}

void sha3AbsorbBlock(detail::Sha3State& s) {
    const size_t lanes = s.rate / 8;
    for (size_t i = 0; i < lanes; ++i) s.s[i] ^= load64le(s.buf + 8 * i);
    keccakF(s.s);
}

void sha3Update(detail::Sha3State& s, const uint8_t* data, size_t len) {
    while (len > 0) {
        const size_t take = std::min(len, s.rate - s.pt);
        std::memcpy(s.buf + s.pt, data, take);
        s.pt += take;
        data += take;
        len -= take;
        if (s.pt == s.rate) {
            sha3AbsorbBlock(s);
            s.pt = 0;
        }
    }
}

void sha3Final(detail::Sha3State& s, uint8_t* out) {
    // 填充规则：先在块内剩余位置补零，再写入域分隔后缀 0x06（pad10*1 起始位 + 域位），
    // 最后在块末尾置 0x80（pad10*1 结束位）。注意 buf 中残留的是上一块的原始数据，
    // 因此这里必须“赋值”而不是“异或”。
    std::memset(s.buf + s.pt, 0, s.rate - s.pt);
    s.buf[s.pt] = 0x06;
    s.buf[s.rate - 1] |= 0x80;  // 当 pt == rate-1 时与 0x06 合并为 0x86
    sha3AbsorbBlock(s);
    // 挤出（rate 足够容纳一次输出，无需再次置换）
    for (int i = 0; i < s.outBytes; ++i)
        out[i] = static_cast<uint8_t>(s.s[i / 8] >> (8 * (i % 8)));
}

// ================================================================ BLAKE2s/b =
constexpr int kBlake2Sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

// BLAKE2s 使用 SHA-256 的 IV，BLAKE2b 使用 SHA-512 的 IV
void blake2sInit(detail::Blake2sState& s, int outBytes) {
    const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    for (int i = 0; i < 8; ++i) s.h[i] = iv[i];
    // 参数块：digest_length | key_length<<8 | fanout<<16 | depth<<24
    // 未加密钥、单线程树模式（fanout=1, depth=1 => 0x01010000）
    s.h[0] ^= 0x01010000u ^ static_cast<uint32_t>(outBytes);
    s.t[0] = 0;
    s.t[1] = 0;
    s.buflen = 0;
    std::memset(s.buf, 0, sizeof(s.buf));
}

void blake2sCompress(detail::Blake2sState& s, const uint8_t* block, bool last) {
    uint32_t v[16];
    uint32_t m[16];
    for (int i = 0; i < 8; ++i) v[i] = s.h[i];
    const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    for (int i = 0; i < 8; ++i) v[8 + i] = iv[i];
    for (int i = 0; i < 16; ++i) m[i] = load32le(block + 4 * i);

    v[12] ^= s.t[0];
    v[13] ^= s.t[1];
    if (last) v[14] = ~v[14];

    auto G = [&](int a, int b, int c, int d, uint32_t x, uint32_t y) {
        v[a] = v[a] + v[b] + x;
        v[d] = rotr32(v[d] ^ v[a], 16);
        v[c] = v[c] + v[d];
        v[b] = rotr32(v[b] ^ v[c], 12);
        v[a] = v[a] + v[b] + y;
        v[d] = rotr32(v[d] ^ v[a], 8);
        v[c] = v[c] + v[d];
        v[b] = rotr32(v[b] ^ v[c], 7);
    };

    for (int r = 0; r < 10; ++r) {
        const int* sg = kBlake2Sigma[r % 10];
        G(0, 4, 8, 12, m[sg[0]], m[sg[1]]);
        G(1, 5, 9, 13, m[sg[2]], m[sg[3]]);
        G(2, 6, 10, 14, m[sg[4]], m[sg[5]]);
        G(3, 7, 11, 15, m[sg[6]], m[sg[7]]);
        G(0, 5, 10, 15, m[sg[8]], m[sg[9]]);
        G(1, 6, 11, 12, m[sg[10]], m[sg[11]]);
        G(2, 7, 8, 13, m[sg[12]], m[sg[13]]);
        G(3, 4, 9, 14, m[sg[14]], m[sg[15]]);
    }
    for (int i = 0; i < 8; ++i) s.h[i] ^= v[i] ^ v[i + 8];
}

void blake2sUpdate(detail::Blake2sState& s, const uint8_t* data, size_t len) {
    while (len > 0) {
        // 缓冲区已满且还有后续数据 —— 作为非最终块压缩
        if (s.buflen == 64) {
            s.t[0] += 64;
            if (s.t[0] < 64) s.t[1]++;
            blake2sCompress(s, s.buf, false);
            s.buflen = 0;
        }
        const size_t take = std::min(len, static_cast<size_t>(64 - s.buflen));
        std::memcpy(s.buf + s.buflen, data, take);
        s.buflen += take;
        data += take;
        len -= take;
    }
}

void blake2sFinal(detail::Blake2sState& s, uint8_t* out, int outBytes) {
    s.t[0] += static_cast<uint32_t>(s.buflen);
    if (s.t[0] < s.buflen) s.t[1]++;
    std::memset(s.buf + s.buflen, 0, 64 - s.buflen);
    blake2sCompress(s, s.buf, true);
    for (int i = 0; i < outBytes; ++i)
        out[i] = static_cast<uint8_t>(s.h[i / 4] >> (8 * (i % 4)));
}

void blake2bInit(detail::Blake2bState& s, int outBytes) {
    for (int i = 0; i < 8; ++i) s.h[i] = kSha512H0[i];
    s.h[0] ^= 0x01010000ULL ^ static_cast<uint64_t>(outBytes);
    s.t[0] = 0;
    s.t[1] = 0;
    s.buflen = 0;
    std::memset(s.buf, 0, sizeof(s.buf));
}

void blake2bCompress(detail::Blake2bState& s, const uint8_t* block, bool last) {
    uint64_t v[16];
    uint64_t m[16];
    for (int i = 0; i < 8; ++i) v[i] = s.h[i];
    for (int i = 0; i < 8; ++i) v[8 + i] = kSha512H0[i];
    for (int i = 0; i < 16; ++i) m[i] = load64le(block + 8 * i);

    v[12] ^= s.t[0];
    v[13] ^= s.t[1];
    if (last) v[14] = ~v[14];

    auto G = [&](int a, int b, int c, int d, uint64_t x, uint64_t y) {
        v[a] = v[a] + v[b] + x;
        v[d] = rotr64(v[d] ^ v[a], 32);
        v[c] = v[c] + v[d];
        v[b] = rotr64(v[b] ^ v[c], 24);
        v[a] = v[a] + v[b] + y;
        v[d] = rotr64(v[d] ^ v[a], 16);
        v[c] = v[c] + v[d];
        v[b] = rotr64(v[b] ^ v[c], 63);
    };

    for (int r = 0; r < 12; ++r) {
        const int* sg = kBlake2Sigma[r % 10];
        G(0, 4, 8, 12, m[sg[0]], m[sg[1]]);
        G(1, 5, 9, 13, m[sg[2]], m[sg[3]]);
        G(2, 6, 10, 14, m[sg[4]], m[sg[5]]);
        G(3, 7, 11, 15, m[sg[6]], m[sg[7]]);
        G(0, 5, 10, 15, m[sg[8]], m[sg[9]]);
        G(1, 6, 11, 12, m[sg[10]], m[sg[11]]);
        G(2, 7, 8, 13, m[sg[12]], m[sg[13]]);
        G(3, 4, 9, 14, m[sg[14]], m[sg[15]]);
    }
    for (int i = 0; i < 8; ++i) s.h[i] ^= v[i] ^ v[i + 8];
}

void blake2bUpdate(detail::Blake2bState& s, const uint8_t* data, size_t len) {
    while (len > 0) {
        if (s.buflen == 128) {
            s.t[0] += 128;
            if (s.t[0] < 128) s.t[1]++;
            blake2bCompress(s, s.buf, false);
            s.buflen = 0;
        }
        const size_t take = std::min(len, static_cast<size_t>(128 - s.buflen));
        std::memcpy(s.buf + s.buflen, data, take);
        s.buflen += take;
        data += take;
        len -= take;
    }
}

void blake2bFinal(detail::Blake2bState& s, uint8_t* out, int outBytes) {
    s.t[0] += static_cast<uint64_t>(s.buflen);
    if (s.t[0] < s.buflen) s.t[1]++;
    std::memset(s.buf + s.buflen, 0, 128 - s.buflen);
    blake2bCompress(s, s.buf, true);
    for (int i = 0; i < outBytes; ++i)
        out[i] = static_cast<uint8_t>(s.h[i / 8] >> (8 * (i % 8)));
}

// ==================================================== 自检向量（权威值） ====
// 说明：下表所有期望值均由 Python 3.14 的 hashlib / zlib（独立参考实现）生成，
//       与 NIST / RFC 官方测试向量一致。
enum class NamedInput { Empty, Abc, MessageDigest, Fox, Digits, Sha2_56, Sha512_112, MillionA };

const char* namedInputName(NamedInput n) {
    switch (n) {
        case NamedInput::Empty: return "空输入";
        case NamedInput::Abc: return "\"abc\"";
        case NamedInput::MessageDigest: return "\"message digest\"";
        case NamedInput::Fox: return "quick brown fox";
        case NamedInput::Digits: return "\"123456789\"";
        case NamedInput::Sha2_56: return "56 字节标准串";
        case NamedInput::Sha512_112: return "112 字节标准串";
        case NamedInput::MillionA: return "100 万个 'a'";
    }
    return "?";
}

void makeNamedInput(NamedInput n, std::string& out) {
    switch (n) {
        case NamedInput::Empty: out.clear(); break;
        case NamedInput::Abc: out = "abc"; break;
        case NamedInput::MessageDigest: out = "message digest"; break;
        case NamedInput::Fox: out = "The quick brown fox jumps over the lazy dog"; break;
        case NamedInput::Digits: out = "123456789"; break;
        case NamedInput::Sha2_56:
            out = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
            break;
        case NamedInput::Sha512_112:
            out =
                "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
            break;
        case NamedInput::MillionA: out.assign(1000000, 'a'); break;
    }
}

struct AlgoVector {
    Algo        algo;
    const char* expected;
};

struct VectorRow {
    NamedInput  input;
    AlgoVector  v[kAlgoCount];
};

const VectorRow kVectorRows[] = {
    {NamedInput::Empty,
     {{Algo::CRC32, "00000000"},
      {Algo::MD5, "d41d8cd98f00b204e9800998ecf8427e"},
      {Algo::SHA1, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
      {Algo::SHA256, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {Algo::SHA384,
       "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b"},
      {Algo::SHA512,
       "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"},
      {Algo::SHA3_256, "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"},
      {Algo::SHA3_512,
       "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26"},
      {Algo::BLAKE2S_256, "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9"},
      {Algo::BLAKE2B_512,
       "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce"}}},
    {NamedInput::Abc,
     {{Algo::CRC32, "352441c2"},
      {Algo::MD5, "900150983cd24fb0d6963f7d28e17f72"},
      {Algo::SHA1, "a9993e364706816aba3e25717850c26c9cd0d89d"},
      {Algo::SHA256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {Algo::SHA384,
       "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7"},
      {Algo::SHA512,
       "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
      {Algo::SHA3_256, "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"},
      {Algo::SHA3_512,
       "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0"},
      {Algo::BLAKE2S_256, "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982"},
      {Algo::BLAKE2B_512,
       "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"}}},
    {NamedInput::MessageDigest,
     {{Algo::CRC32, "20159d7f"},
      {Algo::MD5, "f96b697d7cb7938d525a2f31aaf161d0"},
      {Algo::SHA1, "c12252ceda8be8994d5fa0290a47231c1d16aae3"},
      {Algo::SHA256, "f7846f55cf23e14eebeab5b4e1550cad5b509e3348fbc4efa3a1413d393cb650"},
      {Algo::SHA384,
       "473ed35167ec1f5d8e550368a3db39be54639f828868e9454c239fc8b52e3c61dbd0d8b4de1390c256dcbb5d5fd99cd5"},
      {Algo::SHA512,
       "107dbf389d9e9f71a3a95f6c055b9251bc5268c2be16d6c13492ea45b0199f3309e16455ab1e96118e8a905d5597b72038ddb372a89826046de66687bb420e7c"},
      {Algo::SHA3_256, "edcdb2069366e75243860c18c3a11465eca34bce6143d30c8665cefcfd32bffd"},
      {Algo::SHA3_512,
       "3444e155881fa15511f57726c7d7cfe80302a7433067b29d59a71415ca9dd141ac892d310bc4d78128c98fda839d18d7f0556f2fe7acb3c0cda4bff3a25f5f59"},
      {Algo::BLAKE2S_256, "fa10ab775acf89b7d3c8a6e823d586f6b67bdbac4ce207fe145b7d3ac25cd28c"},
      {Algo::BLAKE2B_512,
       "3c26ce487b1c0f062363afa3c675ebdbf5f4ef9bdc022cfbef91e3111cdc283840d8331fc30a8a0906cff4bcdbcd230c61aaec60fdfad457ed96b709a382359a"}}},
    {NamedInput::Fox,
     {{Algo::CRC32, "414fa339"},
      {Algo::MD5, "9e107d9d372bb6826bd81d3542a419d6"},
      {Algo::SHA1, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"},
      {Algo::SHA256, "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
      {Algo::SHA384,
       "ca737f1014a48f4c0b6dd43cb177b0afd9e5169367544c494011e3317dbf9a509cb1e5dc1e85a941bbee3d7f2afbc9b1"},
      {Algo::SHA512,
       "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb642e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0538f3db854fee6"},
      {Algo::SHA3_256, "69070dda01975c8c120c3aada1b282394e7f032fa9cf32f4cb2259a0897dfc04"},
      {Algo::SHA3_512,
       "01dedd5de4ef14642445ba5f5b97c15e47b9ad931326e4b0727cd94cefc44fff23f07bf543139939b49128caf436dc1bdee54fcb24023a08d9403f9b4bf0d450"},
      {Algo::BLAKE2S_256, "606beeec743ccbeff6cbcdf5d5302aa855c256c29b88c8ed331ea1a6bf3c8812"},
      {Algo::BLAKE2B_512,
       "a8add4bdddfd93e4877d2746e62817b116364a1fa7bc148d95090bc7333b3673f82401cf7aa2e4cb1ecd90296e3f14cb5413f8ed77be73045b13914cdcd6a918"}}},
    {NamedInput::Digits,
     {{Algo::CRC32, "cbf43926"},
      {Algo::MD5, "25f9e794323b453885f5181f1b624d0b"},
      {Algo::SHA1, "f7c3bc1d808e04732adf679965ccc34ca7ae3441"},
      {Algo::SHA256, "15e2b0d3c33891ebb0f1ef609ec419420c20e320ce94c65fbc8c3312448eb225"},
      {Algo::SHA384,
       "eb455d56d2c1a69de64e832011f3393d45f3fa31d6842f21af92d2fe469c499da5e3179847334a18479c8d1dedea1be3"},
      {Algo::SHA512,
       "d9e6762dd1c8eaf6d61b3c6192fc408d4d6d5f1176d0c29169bc24e71c3f274ad27fcd5811b313d681f7e55ec02d73d499c95455b6b5bb503acf574fba8ffe85"},
      {Algo::SHA3_256, "87cd084d190e436f147322b90e7384f6a8e0676c99d21ef519ea718e51d45f9c"},
      {Algo::SHA3_512,
       "e1e44d20556e97a180b6dd3ed7ae5c465cafd553fa8747dca038fb95635b77a37318f7ddf7aec1f6c3c14bb160ba2497007decf38dd361cab199e3b8c8fe1f5c"},
      {Algo::BLAKE2S_256, "7acc2dd21a2909140507f37396acce906864b5f118dfa766b107962b7a82a0d4"},
      {Algo::BLAKE2B_512,
       "f5ab8bafa6f2f72b431188ac38ae2de7bb618fb3d38b6cbf639defcdd5e10a86b22fccff571da37e42b23b80b657ee4d936478f582280a87d6dbb1da73f5c47d"}}},
    {NamedInput::Sha2_56,
     {{Algo::CRC32, "171a3f5f"},
      {Algo::MD5, "8215ef0796a20bcaaae116d3876c664a"},
      {Algo::SHA1, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
      {Algo::SHA256, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {Algo::SHA384,
       "3391fdddfc8dc7393707a65b1b4709397cf8b1d162af05abfe8f450de5f36bc6b0455a8520bc4e6f5fe95b1fe3c8452b"},
      {Algo::SHA512,
       "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c33596fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445"},
      {Algo::SHA3_256, "41c0dba2a9d6240849100376a8235e2c82e1b9998a999e21db32dd97496d3376"},
      {Algo::SHA3_512,
       "04a371e84ecfb5b8b77cb48610fca8182dd457ce6f326a0fd3d7ec2f1e91636dee691fbe0c985302ba1b0d8dc78c086346b533b49c030d99a27daf1139d6e75e"},
      {Algo::BLAKE2S_256, "6f4df5116a6f332edab1d9e10ee87df6557beab6259d7663f3bcd5722c13f189"},
      {Algo::BLAKE2B_512,
       "7285ff3e8bd768d69be62b3bf18765a325917fa9744ac2f582a20850bc2b1141ed1b3e4528595acc90772bdf2d37dc8a47130b44f33a02e8730e5ad8e166e888"}}},
    {NamedInput::Sha512_112,
     {{Algo::CRC32, "191f3349"},
      {Algo::MD5, "03dd8807a93175fb062dfb55dc7d359c"},
      {Algo::SHA1, "a49b2446a02c645bf419f995b67091253a04a259"},
      {Algo::SHA256, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
      {Algo::SHA384,
       "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039"},
      {Algo::SHA512,
       "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"},
      {Algo::SHA3_256, "916f6061fe879741ca6469b43971dfdb28b1a32dc36cb3254e812be27aad1d18"},
      {Algo::SHA3_512,
       "afebb2ef542e6579c50cad06d2e578f9f8dd6881d7dc824d26360feebf18a4fa73e3261122948efcfd492e74e82e2189ed0fb440d187f382270cb455f21dd185"},
      {Algo::BLAKE2S_256, "358dd2ed0780d4054e76cb6f3a5bce2841e8e2f547431d4d09db21b66d941fc7"},
      {Algo::BLAKE2B_512,
       "ce741ac5930fe346811175c5227bb7bfcd47f42612fae46c0809514f9e0e3a11ee1773287147cdeaeedff50709aa716341fe65240f4ad6777d6bfaf9726e5e52"}}},
    {NamedInput::MillionA,
     {{Algo::CRC32, "dc25bfbc"},
      {Algo::MD5, "7707d6ae4e027c70eea2a935c2296f21"},
      {Algo::SHA1, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"},
      {Algo::SHA256, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
      {Algo::SHA384,
       "9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b07b8b3dc38ecc4ebae97ddd87f3d8985"},
      {Algo::SHA512,
       "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"},
      {Algo::SHA3_256, "5c8875ae474a3634ba4fd55ec85bffd661f32aca75c6d699d0cdcb6c115891c1"},
      {Algo::SHA3_512,
       "3c3a876da14034ab60627c077bb98f7e120a2a5370212dffb3385a18d4f38859ed311d0a9d5141ce9cc5c66ee689b266a8aa18ace8282a0e0db596c90b0a7b87"},
      {Algo::BLAKE2S_256, "bec0c0e6cde5b67acb73b81f79a67a4079ae1c60dac9d2661af18e9f8b50dfa5"},
      {Algo::BLAKE2B_512,
       "98fb3efb7206fd19ebf69b6f312cf7b64e3b94dbe1a17107913975a793f177e1d077609d7fba363cbba00d05f7aa4e4fa8715d6428104c0a75643b0ff3fd3eaf"}}},
};

// 跨块边界扫描：数据为 data[i] = (i*37+11) & 0xFF，覆盖各算法的填充临界长度
const int kSweepSizes[] = {1,   3,   55,  56,  57,  63,  64,  65,  71,  72,  73,  111,
                           112, 113, 127, 128, 129, 135, 136, 137, 167, 168, 169, 200};
constexpr int kSweepRows = static_cast<int>(sizeof(kSweepSizes) / sizeof(kSweepSizes[0]));

const char* kSweepExpected[kSweepRows][kAlgoCount] = {
    {"45d03605", "13c8ffd977013703a701cf8e11deac65", "067d5096f219c64b53bb1c7d5e3754285b565a47", "e7cf46a078fed4fafd0b5e3aff144802b853f8ae459a4f0c14add3314b7cc3a6", "2deb5d512a82fb61b846799f41367b0172a8a82a4b1539fa9f3fd148bdd1db7eb053b452eb916523186cbbe41e9f2fbb", "527cff2b6fdfbc0f54fe092b17d6d8c7e22500242635fa56981e85a64da6ce8a12a3a66cf69fd48f588bcba9bad141b8e351a0cdd4925ae57289933eec1fc153", "962f8420917d7fa5479f4a767bf9b9a30a4ab377af26d72dbcff167d6ce3f6f5", "f2ce06537b57df321b623522d6cd5a09a6fd85c65ddf058827c52589ef632e2a6d49d473496f15a84322ccb05b7594be00dfb778091abad5b172ee963dfe2777", "b480c25c1e06eea9e9c3e36754715cf0958e4d14b22a8c2a7bc34ebf75208602", "c3fd8f085f18d380c9cafff7213ef009ca6d8b21f87dfbef9d99368be8f58e5e794f20b409cd8f74e573bdbb40f54ca07c5dc498f7de99ddffae43cf033eeebd"},
    {"3753a57b", "876d16c575f9d3d7f51f12fef37237eb", "5e8b290f00d31c341d769e40255c757924864ef3", "b39fad1a1075f64570b3226d339ea818f9c66ecd2f1c59fd8b9c5a32b54c513f", "da78ef0d5c2d8b8c8cfb5331799cf5d82fdb7c797031b8a808f2620ae844083c2cf3cbc1146d56b7cbca8aaaa0adffc0", "b634899da10b86dd03ad5e7cef4279fa2addbc82a4fdc782c99d95d776ceec8777201d4acdf259e9f935fa43ff71ac7e7317220734b9f43bd9192c8bdf46ac8c", "49e580eb827c41eef7a105c0d4accdc58663ad774662ec919ee23c99214d856b", "a3e84e7e94ffa3a487df8295969df6f2338c4dec641e9c95f17e2407082147c670051cf9659b001f0cb13bb44ff9cdcd665f6fbbf562a3413d52bc412caa39d7", "4448727dbef2656f922f15cb13e02118d9187fa52dd7409e1cc891ea34478726", "f57bf23a0958ff528420dd321893d1aae3239e522ce6d358dfa5cd21d1071834cc4ab3c1abd34be820a20a683db3942d5b23d774e1f831bf573ed22cd0b3487c"},
    {"e6c5dacc", "d872aa0473a24da995ce4ac518ade767", "c4622048cfef59b72875839ee7ae1cbcf55e7658", "2900465fcb533e05a158fd2b3be0e5e3b03740d83060aa3580e0d98a96bf2384", "5af70c3661eb084de491e3a9cc1d584c28c2e752933aa13f047f7d2cd5285607b9d9750373e59fc0b5ff84ccf9099738", "2fd061a12ae849786141611e02a18ad9dad5d1cf6980f67fd4021fe369e4fef874548e9e5146e53622a919b3dcbd652f9abdd9578c4bd0c8e28d38008ea922b5", "0ec32ab142bd2dd2993d2359d82f42e240ed761af42812408e6cdbed1db020d5", "aedd07a1a8ecfb04f6bacc34c78507135518c5352096238caaf12f03d933e576bdc6a08452eb0152930dad173f499204913ba7bcb27388310f8de84ce90b2b92", "b27d59a6057af55eb60443d9b00dfa4989ce1d875b2e1c161fda2ee8e5c532b6", "85cf270a7bc6cc0cb33ea2df167cb553044a6d57f83ea934092d996a0fdeeb97b712e1a300e58ebba47236fd3f09d3361f2d03c8475e00dbbc05d67914ac61dd"},
    {"1a337bd7", "e23567645846677c205de80f9779081b", "ddc12942656468475970fa4fa49161f52ed138e4", "31454ff48ef36af2f08fd511bdc37d9d5855ac23e992e5ff5445cb6b7674a674", "c1a6736be1fcc8891a626b08a10097fbf2fcce3acd19ec22154761c485f874ac8ca986a1c213b790ebb57adeb0008b52", "78ade6b0c755effd2b093d02bbeb8354d57c3e0c36457f941b47e2b384bc0a279c4cdaae1f9968e47bfa1e94438c33af1253067677087890f35f944a6fb8693e", "736ec12fb2e163865ca1fcc770ea117acf215520fc28d5ed0c5917e246dd8b76", "612a7791da31baf353c02efdae6b7cd41ec45248767cb7d3d492cb20eca9fa4264e05d01d903f5c706cc31b81fda704ff6d31e765d9174215b2f4db6045f3daa", "c21eb3c7c8c0812ec428b73e38f9fe00ab4aaf79e54df3100629251f656d1ba3", "f7ecef334552706b6741109a3a88ae88d751ccd388d194e4d63ec77032ff08c7e50ed6270015e99a79ddf6c6b56546df8b2830b725dd08633ad5bde5828c2d8a"},
    {"68c8eaf3", "1fe5e4a27e12484d7203d0b60c21c51f", "c3299df7d6a56509d38a55d09ccf403d4a56d044", "bcc0a5d3791b985b7550e04ca660a6c63a589ba1edd2283c8e110e5b515df124", "2fe0b7bcb376fc6ef5e263f93457d2c872b8a05173becf6de2ae9000ac32642739ba08ccdf7201ac58c211a6aadb0d35", "4e5e6def94589f306ca004ae1313f5fc2bfb4b618beaf24f1af85d7c341658ecb51033b127d8d728680754a072e85664718a7bb9a10a54cd9c7f8807accc051b", "27436930a1633dd43c8b96b42e35f8000d9a9586e9d94b15385400f89f4f1019", "ec05ee7ffac035baa15669ce204ff0b54ecc531e93840848d3084d1d51df63eb83831e42460d01ba33cea1f7889d301231c993719554a659eac5f00b1aced345", "4039fddecfb09329eb5693db1ca7c72279821b3181e53706dae72de89b25e4f1", "448e128262763edc37fcef84ca3c9de36d2676bb4828e9b1134591163cde83f9a0ef49aebc98d614877243099339fa734afb6c2f01a5dbd92c03f885eef0f192"},
    {"bae609d9", "4775b66278a8fc132ff80923378216cd", "7f8c3fa49f1297bd8b9feb964b6b419987f9f0d1", "5f6401b96532c36de4e65beec0409b69b1d181864c8009b7a04f43e5d56350d1", "a4664ae68697964c5f1a1e4dde77f0237dab9e9ea7a6e5ceb2e50e984cdfad14bc55fd1c8830a5acff820e564a3c6a34", "853d2f48b4e8c74aa7c547ad831787b4f2bb85aa2851241a7fd1a38f651c2eb5816990092a4cd336d86dba73f0191f69a8825c7f5f2991bdcc2fb9661af22f46", "29efb8419f6baa97ef5358f9992246687e8f79a282561d5f51fd1da27fa4afdd", "aecae7f7f0ebde666f2937f51b7f0d94e2eeddff17ab4b502c1bb4fc2cb12d72791a8c8632fd521352c111f39997f13add4cf5afbb0fb51cdc5b5aef9feacd46", "6200ac9e33d28b2d2ed08ec902fa53ac4da9a0b469607f6526df92696b29b4d4", "8dac5ef4df48783790d11ee20c2376151c6db7b345a7c48d137764c05db2073022c0c4780ff512c4ca0cd8e6ab05105004ab87b12cfce78cb7a51670ea41d8c8"},
    {"ffbae609", "71e123b70c7aa64826fcfe472694cd1c", "a334b47180c61fd522f99905ec02c36f9e848211", "94eb5de4943613fd048dc93393ab06877405faa39c11f53e9386083339833e7e", "13c3b81071b4ff54bcb80912eeb97bfadc12c29df001f83defaf970a4e43b104aeda6a6e6fa71d0d2cc6c745f2952406", "be7cccdacc7551d40828f4f3568f741a224ec5e5274ca547e06ad5dc8313456706033259aa46937a061bc30f9913597e0b591c6492da5334e9148bbb91ce805b", "e8e686424d5eb6ab8534b163dfa3a801c689583238b4443d92d4626a6b320cef", "c646f47cc7a35076480aa62f2a179f5f04bf0a772dc1ce3438a3d2389a2378bae55ad989884fcd8eaae6620434a566e0ca22088345020b55f63f5e3da35472b1", "ea02c165725b42ba04a1e9eb5a12ad5db75d52297b69d83b8ce11895d144a8f4", "c615d77563a0a0943ad9c38350a8f929bd05b6d0516c69e5dfebf8ebb15da584739a07d59aac48c96d9733803cc553c99bfa523b954bc6558f62708300d0a681"},
    {"4a2f75d7", "5949948f26e35203661075214faa3966", "dd27d9eb923d39687e10872c3e8133ba2f0a68a1", "fc518669b6eb4b4dd91827ecacef86689c725bd5bab888fd3b26dbb196eec954", "af669b8d0d69de941a5b881f960fcc4404b9cef88c62c5cb171212ba76fe44448eedc3c5c983b5552b745643ed16eeaa", "3be887ac920f6d448401e32d90815cbff19ec9b8042d9e14bd5d289db18437736f1a00b7573607684d83dc2f398ee82f5296beae0231e5bd2fe139b20292a18c", "ea5bb1c243ff8094c1e22a12443de5955503ade39719c4608b25e4f4785bb12e", "7c0ee9a9cd6e7626d2515b390940f598cc1aaa607b1ad997c1a576b6b8912c9ecbfcb2aced3430b6c340832611b948a57ceb8f73022458b302fd6186f7461578", "ce85d4e6a3820eaca8d06a596b9c8e2dc1564a1f9ff270df917e1c6aad64a019", "458bf9323fcca84fd6a87bfa04ba9725d44bb870e6e4b5b55625671f8ee54bb36f6bec379eb3febdb9e1892bf504cb65ea469db8e4ad6f70c24e0c7315934dd1"},
    {"ca9771cf", "6cab733ac73421d082d01d26c3870913", "bdb828f97254d1d5bf733d1acb7adf894a41d343", "2da56abe0ee37a408a94693d95f7af5e9774f0659a3c2593b33df5fb0a236e35", "6be2b6e61af6e2183f8ae47fa84b2bda3cfa9a544ba23ffd2b9d929c24f30418a7d4afb9d603717b91b554677ac00665", "6337e6240225ad64dee905f6a8b1ab65d17dbbfc92b2c5bb38a74254ee0f94b61f2f08d4c904f3542af7a8fc1484f8db02d6a9311c082513d8bbec229a7d3c6c", "0ac3a06a527eab9ecf5919ebe035b8c8149ba22ccaf469a1f1c50e7184d913e4", "bf5828621a2ea8d1d96d5a0100852a995705cd5a60bf13e8d5161dd8fc051f0a51c5c0cae86438659f1a0fb30efc8a1ed01a837fd93725245f24f3efcc79e1de", "2286cb1ae2d718e208bb29347fc06c695a0e70f756947e94a8181f24080654aa", "3ca36f4524cfcdb37efcbc009e3b2f52048ed4001f775e19c8335cfdf1dca2b72bbce0e6f473d1c6020cb60a8f83b06bd1840b32012d7c17bf12e0fc676815f3"},
    {"4877cb4a", "a6d36774de81858e2bbe7fd11c17aa94", "cca562c1390ae4cbfc61e35d9455e98056994879", "f1ea2423d41c019f186e07091035dfdd3627392a51584d753295a86810066c61", "e2301a6f1fe474b7bb1a1a9902c025f951214145e180663fea31f02df4cea76685e65572d453c8c0a4f7c7ec2c280ded", "9bc02ed987ee6a7ea6b507e3d2dfd572ee475b13d1350469ecba53a82d8e4f20b36065138ae69e5350e722b233d9b82b68d4d869feb3145d6955de8fe08f7022", "401dd44fde549e7492a9fd98636f2628017afe9e9fa097d68c2826cf8a1b6d44", "7163023fb85589362e8f184399accaa14e0d17da87dcc69cece725c9bef23b0e4dfb12ed015213d3e6f5c5cc98f23c20303dd5a2091df35815f5f242e7a9bfeb", "cf584a0f763f002735c2447b79cbd20438cca895e98374d0f8ddff8776892a87", "0269c2dabef5f39eea08e75cb747146e900a782dee72daf53b1e2974021f5e919a09c1217a61105c2f18f58985aeba7ff7242c987f4cd623d89b628877f2cd11"},
    {"8d4f104e", "ce95231d6fb384a4b144857c98eb3c98", "9190a5474c084a40af87f21a69d50ddefd7b52bf", "d3d699995b8dca504d20bdaf82d9ef8bc2f7bf3a2126eb7192a6e831f5b4babb", "929d2f1b08e3197890b6fa975a833c143a171dbcdb45d3a3168e45613dec81a04826070b171bca311d9adc5c00123fd7", "048668fe5d1556c178cb761b22536b4f55531c130803b07e551bbc442db7c170b2e5c553069cffb08f8de37592a8806ab5ec8dc38473333d41db558378c944cf", "251563a59aa1a2870976627239fb83020e9472e33cc5b2621d89fc9bb230926d", "c2823e8b428215ecacce41592b976554f6dbdb5aeba524cdcda69d3e86ac791f7750c24720fc0ffec0966909c519fb53a915f8c9c7b9caba1d2b25d5a3b97cab", "abcd0539c6002ceb188d45682faa2594c7b4d61fac4ac3cb67b78c77c379fb0b", "01044a52520f2c14b77ba5b2edb3e68c2c9285f10d07615bfe1615be8dfe5cb74a7f0b1b00cbba204c04cd63baf57de2e7ca713209d81f940fc226e18e47bd29"},
    {"bdc9b383", "971d0e793c1b82b4ade7bd998916b4da", "aa138368b05cd08f2e4c0d7ef9be54552bb00f7f", "aeca4f5a02aead30b044df2ce00b97f2f4874c25352eaa1916a1831819dcc3dd", "d6517d210fc664af0e137a4ca19d7a299465109f40cc0fa9ac7f9d4ac9c77e6687f3cb060890ef7e96f9f5252dc528ab", "4d1db900250c96436052fbca79c13acbf378aad9c35b87d94c3803264df61fd22cbd327c8938d024db372abf4208934ee09367d571d6c670bf74ee07b83e7506", "873aa56abf0f48bcaf7a8933c51c92b253fc67d1d439e79822d26ef82cb3210b", "6185a81812b4a52b251f4264942ba7f5fa6949129a0f918ea41f02157d9633aeab270189cdf6376627ca3bb9f5950dd9510d9fa7278e1f381894554daec1bc53", "cc946235678ffe1e252ee6a54d4e60b2dd0d1e6ab65a974b72fa28dd7997e558", "c02c5bb4f72ca6627aa2f8267d1806a5359c6423d78de0e655fd4c7a431c520048d4afdec2d14944a761ddbd30e4a400e80f6dcca6ca8ed25a17f8ca49a9d820"},
    {"52da41f5", "0940460d6eb047e4dc1442f81f696669", "28528b51ef6d26dc41f33652db6540d0f5c0f124", "cec7a189fcea0a38f025f11208b1bd2b12ec291ff00ec5448a325c5c8db0c7ac", "76078618f7c580d9ca1f9990c2adb38a899f7feda86153dda507b72f1bbafb046ab835aa8d325a89a67e3deb81ad1ad5", "dfb715ca3478a894302ace39c42d1d6646e1044f2247a6274d8b42d155d2fdbe7017195e85cfba96bedc51f84c44638978a540039ff09c64cef6c0c5ccc8f7b6", "3639a271368dc89706f62c5bef7db6c804e89e333dc0c02ee6dc34e81d1e9451", "f0efc353d7efbb55588d25a1a3c97a3de8c7a45b0ecd0d57fcd61b8e0a975f3019ff37f35d99b7477d3bf248b56bd72503bb40ca0f4f0327798493abbe19ce29", "82bbb46a4ecc61325d7d3ecf5ef01db79a8267de4c5c39700108b534c369b8c7", "5deff8298f011da79cc5365e8b71d90a907006aa51caacd07508e85d5bc16ae6a114107c449c45a2d903b504b770804d0295bc8ff178cf3ba0cc7ead6ed1208c"},
    {"ae8cda7b", "71b58eb1aebe1c7a4fbe1dca809939bd", "6d1ae0def5a2dfda8af7cf8414b1a1a2400accf4", "cf1a963953155c44d16895e04631102a3e8c239321e045b9a62d71eac1c4a67a", "887b0a7f829917975def0654b45bfe8644389c8e59033e246912a7e12d9de8fb245e28d87befa0a3b5f2d38eefc100c6", "604b00570a65f49111782330fd36bef680bcc58e13288cbec1b8554e81de17b05f53e1293d33673872d0d48a57aba35f539643a3b8210d2bef35531f2b441451", "148ec0bf00e1c883f214104c3327b2f69e640198c699d6fe42b3f5582675bffb", "300a2f6f08fac6891166d9e2a27aac1e0d30ea0693bf61cce5bb37181041e3cc076cd7bf26ea8631c2a51783f3098873efa8563c75e7c1dc443fb729d0fc925d", "d726e748e0bab9a7affbf470689dd041829e4b03dbf4ae5a1642086e563dcca7", "bf589f5dcaeb05969a090ddb4c921d8f27e83167981b1250f4c8249d8f93adc5b4d9922409e4b08ccd62f09cbb2c2c71995ac2cd0039e7a9c9e38811789db810"},
    {"226e9192", "520bdc2dbab7c25d64263ffb242d9e98", "b2b4bfd7b2112a167b77a600cca227593523c406", "0fe729ff19257bd6fec853acc2ea355f6b34b58e6c0f684c3e188fcdfcd9baae", "17ed2932ea5246073ae8240a06ec458b15bfd72d792520d0c319b84f97532d408fb7d2c39cd3710165914994f44a70f8", "f93a0e7465b294188e8aa2b1cc2e98bc8d5115d46f51c7a9ec599b9d9f96a80fef6a4f226b648c89bd9eac23b3d64264898b568d915c66666c44cd0319e2ef56", "aba5f46e2e39f1190fc0eea953aa88e0203dd1e8bc9f3dc5128817eea94f4604", "b30a61992e06bbaaa4e2614c5e60690acc4f1e02c4f61660aedb4752c573c55487e5e499cbed093746b49cc075e964a9b2d5c255a050902196aef78d45619da8", "ad597347a2770752f8e1698926d0f1d70cdd55cf6d889175c8aff216c05c7e1e", "dea83733edb602d92e3fb563b369d7f0ca8beca792b42547726041b6d1e3f6d0687c7e9d48fe7f4a316c6ac42230a0d68d8755219c2950a366d726f9ebc434f3"},
    {"68f0b719", "3e93b378458b77da96b2357c3bda8cc2", "3b1953091899492377f686c266b81d84b5d40f70", "0aedd4856f8eba0963627336ad5144a9a7dbe12498e6066f0165fc97d8ddee4c", "13274b7eaabb6a3203608240e792e825ddb816853a66fde047b368cc7638d61b9f215323c4160677301ae4d178cd8579", "0b4815d35f9d07b1a30de2790e1be2a720234295cd7b4d9e9af51719ff90019f1fe6d4e402a7dcc4177085023dc460ab743dad9b2c1dda42662bda5d3b2e155b", "003ed3631fb844e5aadb065521d1cab90c5a5359cffd838246b51b3ebe23cfc2", "8bef569e00f14360ae4df768b063f0675700c49eb8c2ee354cadb3d86d4b31f42c82b77e8bebff9d1356483deccc345345e60e5e7156ae7be5bb0cd041539e14", "26df1814f244df0be714f13c2249100ccfa495f3494ac8ddf9129160d95573d0", "d6c9b71996f650444b1c8e121c9440c263d609d8a5fcc70d4dff5fa8ea7c108f3e50c73dc04c3efeb6f03816a4edda920307eacd767dd96f5c46ea1204a253c1"},
    {"cc6bed52", "d1ae06bbf9128955a34bedb6231eee62", "4fd6558b2a93925fb7129447e1d1fac8cff56287", "4f1757ae4bffbae86d775b831765b75af154d52f7deaa46dd378051a2d3ad57f", "f07f92376b47fb9d34ff613a44f18b04b55fb1c480b07de4e08a0e9bd484ffa765c21671c341c70b4024e6bdcda709e4", "1809db04d02717483e04bc4333a14308bd2d0213ba7bf2c63f11eb1b8a0af8252e67fd104fd466fb95f945539824d8e4183155fa5ced0bee3dad46d9384a0bd5", "fe8077efdd5ceeabbdc158395484c5d553489b9718fe4a1f28b6821c358f4aca", "c4148471d4c463929e5af7df25fb4f66875ddf1c473e277e0170665afef3363984e9be12b11949f87a1d00b9e9c330b0fb3f64f31496037ca153f33fc2a5f382", "0016263755d41d676ed40f3ffcf3a2bed30b627dc6838d4fe8aaaa99b4c0cae4", "8768b618745af3659b27d0f1fe30760e02d14a42d56abf416c5c907cc0c1d5ac16142e2d506ab9fef557348d599ac03d0166877922f63508aa61fd2f247cbea7"},
    {"5157b638", "92c67f8eca8400b8636fb90c244bafd9", "08d2ba78b23c5d0b55eb11f3dbede7fce1ec27c0", "04334c8eae2c6af8053fe8b44325a6d5354f0e3baac1881681e425acc58c4ac4", "7d7eace10ddde18ac91db88bdb639fb7df9bd4b4130dc89991b734a89eb975206a370a35e010de696df537bae14b4c4a", "2b486175d5f9fd25919251a56b0de92dfac8e52dd123583adb6a8dd5654c87c055ab7fa9c3f24445d396707261670d0b17d0e760ba40f3eec31f92ef9c42c2ad", "3aa81a5b233ce753b2ab56b3c922338134eb11b8dc3d877d0bc8d19751684b76", "005f39bf14bf6415e8fefc32818ed75fe9b3d699d7ec433aa9c1a6eadda13b5dbafdd891e8479c18e35d5900574cacafd738147318247e3801390b03b8197024", "b461568f4075c6922f44770071c559c6161eb297e8262fee027cb0855d248693", "8ed1fa94f9c8d96d43ffa119c4c9a544ec851bbca670e00e88ecf14a5ea80d1be3b9e5bb51bf542f5a6fdda824653abb852423d57bc69d86d325a3e80cb582b9"},
    {"f051ae82", "a67dac48a8b9c94e7e58fe89ac0764d4", "9750f4c3e51afaf540aa6da1e0b59136c9fafbfc", "ffaf7a9992cb4e7b6dd67667c96d176e4f228ac4276857f740ed4b6e98cadf7c", "3f636c03630185f9bbf04b1ec44625ac4b4a6a1285b7255caea102087e11144a33ffa0b7dc31835572aa7134e32b0b0e", "619e581511b3c73489594a519e076a46eb7eb0af4f720deb02aa1d3632e44dc2c2b9c5ec23677d595a1037f939cab2e9fb6bfaf462352093231f4e81b0421dda", "9f065722983c1b643b3fabbed6e791f6d74f77e6cf5a2d38c07c124465ed5d9f", "fb248e7301ec5132372ef30c98b0da3d1c32aa9d9bd72a976fc5c707443ea92951487b9f7112a8c53b87b95701a6a1fadcbf97d9330ec919f04397bec7098aa4", "399f54c7faf7ff9f6017ab424b4653b394a8f4d8f5877eedc17fcd573fa3e8bf", "405221b309587fac788884c60b9e6a3098cc2c63ac86e54cc0e7fd3738f062216a6f0078b1ee18a8a4699aa1758ea8d1ffb77fb52d96f62e04a6def8b9ef3fb5"},
    {"832cbe19", "a69f0c700a4b1e5b8dd66c9fabe34596", "c369f0216144e77effd9a8fb34363ae2e0ebcc77", "bc3e21f61bdc9dd0975ae1a98ead6c60c61ae958fdeb94770ab5d44c487667eb", "249fc1a418b507e254af637b22bebb51f34ae3736568dae949149475b3fdd2bfa7c9ed713079b22909821d791a4fec24", "aa7ce71dc61dc7f58942b846ba5c2d66a786c5eca46bfff05af1c18cb3c64ff04609cd3b8b5307e14aae5ae0f4dcf45b44988b01d86b2e534656025e398bea60", "30efe517346c818828634cb8a3eb3538c14bc2f280132cf0261ed18a7dd85a8b", "c6395c46ab62779c67b7cd85e21ca20aae1b6c3c683cbbb4cfd536bf1b325e7423dff4b20f057eba3c458f935e8278cbceeb97af14e6194e067a335cf346a0be", "dcb330971cdeb2aad91c15960169f2b60f41b206a7cb3af6f239d9a34975cf55", "4521533daa8df4eafbbe8fa7cf362c45a69b3be40fc942346a864279cd6e4d531abb1df3f01e08714f569a9ec98a5c2db6f36140efec5b771a3403d37c7c0383"},
    {"82edc6c1", "589becb4d2fe32b2aa10f87877186362", "74652503a98876f93c0d65e899c392c7ed2b387b", "dbe0ce98f294f2575e21c8ad9cea3c4c2ee89d97600dc87c06a91f3912bad382", "5722c63c7af73cbba993c4202f1a238a79a494bf4d98d608439c1724373b53670d29f811a6a8fea242693e99cb00619f", "795b9b41c9befc4543b008b63d5af53d97108b1eb9808a93183e22d0dd040a8ff7db8e10a63095a987b8371a20970f472801024e117815817eb295a60cad0a89", "90f6c93fa5782818ad1f9638581a7ef2eacd1519d18f1c6f1caa0731cc82610d", "f8cbd4aaae865dc484625d922b79d89446c87585d97ef23ee14de87fefb3b7402387d5917511ad921936c0e79d0a4e7fc65278dbe3b800c0ceb546f69015d1aa", "09f995e558cf9659b5e913fb7a379bc9d2c01810c413ccfbeb280d072d3f44b3", "79fe76da8667a0e153f858d5d9e5fe95cf9d8eba787c26b9873984529a066651de59bf7d963549e2378c0fd6f44c4f65e8f157f37dfdfdbc59d5b991c6517fd1"},
    {"e235fda2", "1fc67f67bff29728cca0fc8ef1b21233", "e473e5e4c966c6b0bcdbb6b93cf76d1b0da383ac", "f7cd60bcd1d7117bf0110d9a5b938bbd505d15c38022928d4a21e2c469cfa30b", "c98d6e615d672bf55d963f76059f7aaa432c2da5f8a7808e406773e89cba2b36e3faf569f336e2a3168dff9fc23081dc", "5d5f0167f8ed345e3e92c8d21b2426a2f7aabac42b20675c1cd6cd1ec772c7ccdf0501e018d7d20ae3c8ea8bbb1b195ed800193620e8a9163d382a282c95f1c9", "7e8b80b812f0f04e0fa496af6ee58dffdf39d2ec1703b100ff54375840040505", "03585f36d49cbeb997c3745da9250decc6aa01d3139fc3e648e99dc45e8f29b5fb3f3943315b464325c34413d04231ddde05c44f7ec1b78e6c23670021a04ff8", "028c05ffd7afb68bf6db163462a11c9b667fb6e0bc80898e1ba7224b2b43a78d", "6fca3e4ff1410deecf38b53a6ef81619717240686eb2be45c7865db40fbfd530482cbb9235bfc692b51a35af5dc19cb0b6a89a43f05914ea9f550aa3bb07b4e6"},
    {"185a18fa", "7dc14b27183752528516423245d645e5", "a891e61bc0484dad70691836b3e1ff5483b8f22d", "c0574525e77cea4a3aff6b563c95eb83affcec6de295ffbc35598bf654bab09b", "dc732fbacf8bb903085b93a1b14390b94d3d9176d0c5fe6bc766ccca999dc036160d77a6c7a9e988687bfbe791ff4298", "859cf0736b0f7eac8c1aaf2cf17b43f6400f7462fc1608ebc9acc3f1911c600e4892d414ef35edb22b7ca14168ad045788506f11514c2b5cdf3f772255656e45", "f7d07cd3435d250189e6a27678191b73b8df6be1b9c68595e0a895f1b7d43576", "53daccd60a66f52b05b38fb61dc44b4bdf2ab90d8b404a58f46b0df84945baa1dd1cc21df9080770fdd4be9d1cae360cf1117c7f98640c472fbd2ce6b8993d31", "d90089be7e1134686e40195d0c3535908c44759c56f2d893a99e542e0ce0ceb3", "6fe5d2d95340be10d0c2d10394b2b064d00530e7f0e5c1049e4376f847a5388b16edb47222f904ebb9ed37561858bef8657774f49872d5067e51c638542b63b3"},
    {"fe79c9f4", "f9914cd8b67085ceb02d1eba3ff2dc0c", "588cf108f067a9951c1d42248e0c3a6e49afe002", "5e48d64839a8bff5e2471507af6a49f9990817df437857f4a2007df41266e1b5", "84bb0a5a3e514c5864330680d458efab2c5658db7fb13e0e795dd1f1e5900362d38aeda42330757fd16387f7840637ad", "e7b33b6b94b9b5e03e34a445c198d1a3a5620d999e75ad72064c804477a91745cf6bf8a53c18b37eee5697aca1d539fcd93a594cd004242f3fd26cf468762a38", "298588fab178b5941df80b0c340c0bd1132713540627aaf3aac3b2fbf1f9fabc", "99314a205050a6b2bf3d9e181171fff7f746c92541f043671e7f1252f2729d0c1b7718c9bcf69e6d3f018bdb56c6d6ef3acdaffb707c883d4c15b6af96299db6", "d973b530bd07c9ec9028ded65a3bd5b96db3055672df3c35d15d999943fdb04b", "fdd7e388d9e765df6434c004564158cdc2c87e1a058278a2c10bec83c23609e618a2b51eed058d16ec79fff21a60aa7556d78c2e08588080d6fdf908505131b6"},
};

}  // namespace

// ============================================================ 算法信息表 ====
const AlgoInfo* algoTable() {
    static const AlgoInfo kTable[kAlgoCount] = {
        {Algo::CRC32, "crc32", "CRC32", 4, false,
         "⚠ 仅完整性检查：32 位校验和，线性可伪造，绝不可用于安全用途"},
        {Algo::MD5, "md5", "MD5", 16, false,
         "⚠ 已被彻底攻破：碰撞秒级可造，绝不可用于防篡改/签名/口令"},
        {Algo::SHA1, "sha1", "SHA-1", 20, false,
         "⚠ 已被攻破：实用碰撞攻击（SHAttered），绝不可用于防篡改/签名"},
        {Algo::SHA256, "sha256", "SHA-256", 32, true, "✔ 推荐：SHA-2 家族，256 位，通用首选"},
        {Algo::SHA384, "sha384", "SHA-384", 48, true, "✔ 推荐：SHA-2 家族，384 位"},
        {Algo::SHA512, "sha512", "SHA-512", 64, true,
         "✔ 推荐：SHA-2 家族，512 位，64 位平台性能好"},
        {Algo::SHA3_256, "sha3-256", "SHA3-256", 32, true,
         "✔ 推荐：FIPS 202 Keccak 海绵结构，256 位"},
        {Algo::SHA3_512, "sha3-512", "SHA3-512", 64, true,
         "✔ 推荐：FIPS 202 Keccak 海绵结构，512 位"},
        {Algo::BLAKE2S_256, "blake2s", "BLAKE2s-256", 32, true,
         "✔ 推荐：RFC 7693，面向 32 位平台优化，速度极快"},
        {Algo::BLAKE2B_512, "blake2b", "BLAKE2b-512", 64, true,
         "✔ 推荐：RFC 7693，面向 64 位平台优化，速度极快"},
    };
    static_assert(sizeof(kTable) / sizeof(kTable[0]) == kAlgoCount, "算法表数量不匹配");
    return kTable;
}

bool hasInsecureAlgo(const std::vector<Algo>& algos) {
    for (Algo a : algos) {
        if (!algoInfo(a).secure) return true;
    }
    return false;
}

std::string insecureAlgoList(const std::vector<Algo>& algos) {
    std::string out;
    for (Algo a : algos) {
        if (algoInfo(a).secure) continue;
        if (!out.empty()) out += ", ";
        out += algoInfo(a).display;
    }
    return out;
}

const AlgoInfo& algoInfo(Algo a) {
    const int idx = static_cast<int>(a);
    if (idx < 0 || idx >= kAlgoCount) return algoTable()[0];
    return algoTable()[idx];
}

std::vector<Algo> allAlgos() {
    std::vector<Algo> v;
    v.reserve(kAlgoCount);
    for (int i = 0; i < kAlgoCount; ++i) v.push_back(static_cast<Algo>(i));
    return v;
}

namespace {
// 归一化：仅保留小写字母与数字，用于宽松匹配
std::string normalizeAlgoText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) out.push_back(static_cast<char>(std::tolower(uc)));
    }
    return out;
}
}  // namespace

bool parseAlgo(std::string_view text, Algo& out) {
    const std::string key = normalizeAlgoText(text);
    if (key.empty()) return false;
    if (key == "all" || key == "star" || key == "*") {
        out = Algo::CRC32;
        return true;
    }
    // 别名表
    struct Alias { const char* key; Algo algo; };
    static const Alias kAliases[] = {
        {"crc32", Algo::CRC32},        {"crc", Algo::CRC32},
        {"md5", Algo::MD5},            {"sha1", Algo::SHA1},
        {"sha256", Algo::SHA256},      {"sha2256", Algo::SHA256},
        {"sha384", Algo::SHA384},      {"sha2384", Algo::SHA384},
        {"sha512", Algo::SHA512},      {"sha2512", Algo::SHA512},
        {"sha3256", Algo::SHA3_256},   {"sha3", Algo::SHA3_256},
        {"sha3512", Algo::SHA3_512},   {"keccak256", Algo::SHA3_256},
        {"keccak512", Algo::SHA3_512}, {"blake2s", Algo::BLAKE2S_256},
        {"blake2s256", Algo::BLAKE2S_256},
        {"blake2b", Algo::BLAKE2B_512}, {"blake2b512", Algo::BLAKE2B_512},
    };
    for (const Alias& a : kAliases) {
        if (key == a.key) {
            out = a.algo;
            return true;
        }
    }
    return false;
}

std::vector<Algo> parseAlgoList(std::string_view text, std::string* bad) {
    std::vector<Algo> result;
    const std::string key = normalizeAlgoText(text);
    if (key == "all" || key == "*") return allAlgos();

    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        Algo a{};
        if (parseAlgo(token, a)) {
            result.push_back(a);
        } else if (bad) {
            if (!bad->empty()) *bad += ", ";
            *bad += token;
        }
        token.clear();
    };
    for (char c : text) {
        if (c == ',' || c == ';' || c == '|' || c == '+' || c == ' ' || c == '\t' || c == '\n' ||
            c == '\r') {
            flush();
        } else {
            token.push_back(c);
        }
    }
    flush();
    return normalizeAlgoList(result);
}

std::vector<Algo> normalizeAlgoList(const std::vector<Algo>& algos) {
    std::vector<Algo> out;
    for (int i = 0; i < kAlgoCount; ++i) {
        const Algo a = static_cast<Algo>(i);
        for (Algo x : algos) {
            if (x == a) {
                out.push_back(a);
                break;
            }
        }
    }
    if (out.empty()) out.push_back(Algo::SHA256);
    return out;
}

std::string algoIdList(const std::vector<Algo>& algos, const char* sep) {
    std::string out;
    for (size_t i = 0; i < algos.size(); ++i) {
        if (i) out += sep;
        out += algoInfo(algos[i]).id;
    }
    return out;
}

std::string algoDisplayList(const std::vector<Algo>& algos, const char* sep) {
    std::string out;
    for (size_t i = 0; i < algos.size(); ++i) {
        if (i) out += sep;
        out += algoInfo(algos[i]).display;
    }
    return out;
}

// ============================================================ 十六进制 ====
std::string toHex(const uint8_t* data, size_t len, bool upper) {
    static const char* kLower = "0123456789abcdef";
    static const char* kUpper = "0123456789ABCDEF";
    const char* tbl = upper ? kUpper : kLower;
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = tbl[data[i] >> 4];
        out[2 * i + 1] = tbl[data[i] & 0x0F];
    }
    return out;
}

bool fromHex(std::string_view hex, std::vector<uint8_t>& out) {
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string cleaned;
    cleaned.reserve(hex.size());
    for (char c : hex) {
        if (c == ' ' || c == '\t' || c == ':' || c == '-') continue;
        cleaned.push_back(c);
    }
    if (cleaned.size() % 2 != 0) return false;
    out.clear();
    out.reserve(cleaned.size() / 2);
    for (size_t i = 0; i < cleaned.size(); i += 2) {
        const int hi = value(cleaned[i]);
        const int lo = value(cleaned[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// ================================================================ Hasher ====
Hasher::Hasher() : algo_(Algo::SHA256), digestBytes_(32), totalIn_(0) { init(Algo::SHA256); }

Hasher::Hasher(Algo a) : algo_(a), digestBytes_(0), totalIn_(0) { init(a); }

void Hasher::init(Algo a) {
    std::memset(&st_, 0, sizeof(st_));
    algo_ = a;
    digestBytes_ = algoInfo(a).digestBytes;
    totalIn_ = 0;
    switch (a) {
        case Algo::CRC32: crc32Init(st_.crc32); break;
        case Algo::MD5: md5Init(st_.md5); break;
        case Algo::SHA1: sha1Init(st_.sha1); break;
        case Algo::SHA256: sha256Init(st_.sha256); break;
        case Algo::SHA384: sha512Init(st_.sha512, true); break;
        case Algo::SHA512: sha512Init(st_.sha512, false); break;
        case Algo::SHA3_256: sha3Init(st_.sha3, 32); break;
        case Algo::SHA3_512: sha3Init(st_.sha3, 64); break;
        case Algo::BLAKE2S_256: blake2sInit(st_.b2s, 32); break;
        case Algo::BLAKE2B_512: blake2bInit(st_.b2b, 64); break;
        case Algo::Count: break;
    }
}

void Hasher::reset() { init(algo_); }

void Hasher::update(const void* data, size_t len) {
    if (data == nullptr || len == 0) return;
    totalIn_ += len;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    switch (algo_) {
        case Algo::CRC32: crc32Update(st_.crc32, p, len); break;
        case Algo::MD5: md5Update(st_.md5, p, len); break;
        case Algo::SHA1: sha1Update(st_.sha1, p, len); break;
        case Algo::SHA256: sha256Update(st_.sha256, p, len); break;
        case Algo::SHA384:
        case Algo::SHA512: sha512Update(st_.sha512, p, len); break;
        case Algo::SHA3_256:
        case Algo::SHA3_512: sha3Update(st_.sha3, p, len); break;
        case Algo::BLAKE2S_256: blake2sUpdate(st_.b2s, p, len); break;
        case Algo::BLAKE2B_512: blake2bUpdate(st_.b2b, p, len); break;
        case Algo::Count: break;
    }
}

void Hasher::final(uint8_t* out) {
    switch (algo_) {
        case Algo::CRC32: crc32Final(st_.crc32, out); break;
        case Algo::MD5: md5Final(st_.md5, out); break;
        case Algo::SHA1: sha1Final(st_.sha1, out); break;
        case Algo::SHA256: sha256Final(st_.sha256, out); break;
        case Algo::SHA384: sha512Final(st_.sha512, out, 48); break;
        case Algo::SHA512: sha512Final(st_.sha512, out, 64); break;
        case Algo::SHA3_256: sha3Final(st_.sha3, out); break;
        case Algo::SHA3_512: sha3Final(st_.sha3, out); break;
        case Algo::BLAKE2S_256: blake2sFinal(st_.b2s, out, 32); break;
        case Algo::BLAKE2B_512: blake2bFinal(st_.b2b, out, 64); break;
        case Algo::Count: break;
    }
}

std::string Hasher::hexDigest(bool upper) {
    uint8_t buf[64];
    final(buf);
    const std::string s = toHex(buf, static_cast<size_t>(digestBytes_), upper);
    reset();
    return s;
}

std::string hashMemory(Algo a, const void* data, size_t len, bool upper) {
    Hasher h(a);
    h.update(data, len);
    return h.hexDigest(upper);
}

// ================================================================ 自检 ====
namespace {
std::string digestStreaming(Algo a, const std::string& data, size_t chunk) {
    Hasher h(a);
    size_t off = 0;
    size_t step = chunk ? chunk : 1;
    while (off < data.size()) {
        const size_t take = std::min(step, data.size() - off);
        h.update(data.data() + off, take);
        off += take;
    }
    if (data.empty()) h.update(nullptr, 0);  // 空输入路径
    return h.hexDigest();
}
}  // namespace

SelfTestReport runSelfTest() {
    SelfTestReport rep;
    auto add = [&rep](std::string name, std::string expected, std::string actual) {
        SelfTestItem it;
        it.pass = (expected == actual);
        it.name = std::move(name);
        it.expected = std::move(expected);
        it.actual = std::move(actual);
        if (it.pass) {
            rep.passed++;
        } else {
            rep.failed++;
        }
        rep.items.push_back(std::move(it));
    };

    std::string data;
    // 1) 标准测试向量（分块喂入，验证流式正确性）
    for (const VectorRow& row : kVectorRows) {
        makeNamedInput(row.input, data);
        for (const AlgoVector& av : row.v) {
            const std::string actual = digestStreaming(av.algo, data, 61);
            std::string name = algoInfo(av.algo).display;
            name += "(";
            name += namedInputName(row.input);
            name += ")";
            add(std::move(name), av.expected, actual);
        }
    }

    // 2) 跨块边界扫描：每行一种长度，逐算法比对
    for (int r = 0; r < kSweepRows; ++r) {
        const int n = kSweepSizes[r];
        data.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            data[static_cast<size_t>(i)] = static_cast<char>((i * 37 + 11) & 0xFF);
        for (int a = 0; a < kAlgoCount; ++a) {
            const Algo algo = static_cast<Algo>(a);
            // 三种分块方式：整块、1 字节、13 字节，结果必须一致
            const std::string whole = digestStreaming(algo, data, data.size() ? data.size() : 1);
            const std::string byteByByte = digestStreaming(algo, data, 1);
            const std::string mixed = digestStreaming(algo, data, 13);
            std::string name = algoInfo(algo).display;
            name += "(边界 ";
            name += std::to_string(n);
            name += " 字节)";
            const bool pass = (whole == kSweepExpected[r][a]) && (byteByByte == whole) &&
                              (mixed == whole);
            add(std::move(name), kSweepExpected[r][a],
                pass ? whole : (whole + " [1字节:" + byteByByte + " 13字节:" + mixed + "]"));
        }
    }

    // 3) 复位后复用同一实例，结果必须可重现
    {
        Hasher h(Algo::SHA256);
        const std::string first = [&] {
            h.update("abc", 3);
            return h.hexDigest();
        }();
        h.update("abc", 3);
        const std::string second = h.hexDigest();
        add("Hasher 复位重用 (SHA-256)", first, second);
    }
    return rep;
}

const char* versionString() {
#ifdef EHSC_VERSION
    return EHSC_VERSION;   // 由 CMake 注入（CMakeLists.txt 中的 EHSC_VERSION_STRING）
#else
    return "2.0.0bate";    // 直接用 cl.exe 编译时的回退值
#endif
}

const char* buildInfoString() {
#if defined(_MSC_VER)
    return "MSVC " __DATE__ " " __TIME__;
#elif defined(__clang__)
    return "Clang " __clang_version__ " / " __DATE__ " " __TIME__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__ " / " __DATE__ " " __TIME__;
#else
    return __DATE__ " " __TIME__;
#endif
}

}  // namespace ehsc
