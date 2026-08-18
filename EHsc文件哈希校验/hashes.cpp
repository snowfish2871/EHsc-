#include "hashes.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <system_error>
#include <vector>

namespace
{
    constexpr std::size_t FILE_BUFFER_SIZE = 4u * 1024u * 1024u;

    inline std::uint32_t RotateLeft32(std::uint32_t value, unsigned shift)
    {
        return (value << shift) | (value >> (32u - shift));
    }

    inline std::uint32_t RotateRight32(std::uint32_t value, unsigned shift)
    {
        return (value >> shift) | (value << (32u - shift));
    }

    inline std::uint64_t RotateRight64(std::uint64_t value, unsigned shift)
    {
        return (value >> shift) | (value << (64u - shift));
    }

    inline std::uint32_t LoadLE32(const std::uint8_t* p)
    {
        return
            static_cast<std::uint32_t>(p[0]) |
            (static_cast<std::uint32_t>(p[1]) << 8u) |
            (static_cast<std::uint32_t>(p[2]) << 16u) |
            (static_cast<std::uint32_t>(p[3]) << 24u);
    }

    inline std::uint32_t LoadBE32(const std::uint8_t* p)
    {
        return
            (static_cast<std::uint32_t>(p[0]) << 24u) |
            (static_cast<std::uint32_t>(p[1]) << 16u) |
            (static_cast<std::uint32_t>(p[2]) << 8u) |
            static_cast<std::uint32_t>(p[3]);
    }

    inline std::uint64_t LoadBE64(const std::uint8_t* p)
    {
        return
            (static_cast<std::uint64_t>(p[0]) << 56u) |
            (static_cast<std::uint64_t>(p[1]) << 48u) |
            (static_cast<std::uint64_t>(p[2]) << 40u) |
            (static_cast<std::uint64_t>(p[3]) << 32u) |
            (static_cast<std::uint64_t>(p[4]) << 24u) |
            (static_cast<std::uint64_t>(p[5]) << 16u) |
            (static_cast<std::uint64_t>(p[6]) << 8u) |
            static_cast<std::uint64_t>(p[7]);
    }

    inline void StoreLE32(std::uint8_t* p, std::uint32_t value)
    {
        p[0] = static_cast<std::uint8_t>(value);
        p[1] = static_cast<std::uint8_t>(value >> 8u);
        p[2] = static_cast<std::uint8_t>(value >> 16u);
        p[3] = static_cast<std::uint8_t>(value >> 24u);
    }

    inline void StoreLE64(std::uint8_t* p, std::uint64_t value)
    {
        for (unsigned i = 0; i < 8; ++i)
        {
            p[i] = static_cast<std::uint8_t>(value >> (i * 8u));
        }
    }

    inline void StoreBE32(std::uint8_t* p, std::uint32_t value)
    {
        p[0] = static_cast<std::uint8_t>(value >> 24u);
        p[1] = static_cast<std::uint8_t>(value >> 16u);
        p[2] = static_cast<std::uint8_t>(value >> 8u);
        p[3] = static_cast<std::uint8_t>(value);
    }

    inline void StoreBE64(std::uint8_t* p, std::uint64_t value)
    {
        p[0] = static_cast<std::uint8_t>(value >> 56u);
        p[1] = static_cast<std::uint8_t>(value >> 48u);
        p[2] = static_cast<std::uint8_t>(value >> 40u);
        p[3] = static_cast<std::uint8_t>(value >> 32u);
        p[4] = static_cast<std::uint8_t>(value >> 24u);
        p[5] = static_cast<std::uint8_t>(value >> 16u);
        p[6] = static_cast<std::uint8_t>(value >> 8u);
        p[7] = static_cast<std::uint8_t>(value);
    }

    std::string BytesToHex(const std::uint8_t* data, std::size_t size)
    {
        static constexpr char digits[] = "0123456789abcdef";

        std::string result;
        result.resize(size * 2u);

        for (std::size_t i = 0; i < size; ++i)
        {
            result[i * 2u] = digits[data[i] >> 4u];
            result[i * 2u + 1u] = digits[data[i] & 0x0Fu];
        }

        return result;
    }

    // ============================================================
    // CRC32
    // ============================================================

    class CRC32
    {
    public:
        CRC32()
            : value_(0xFFFFFFFFu)
        {
        }

        void Update(const std::uint8_t* data, std::size_t size)
        {
            const auto& table = GetTable();

            for (std::size_t i = 0; i < size; ++i)
            {
                value_ = table[(value_ ^ data[i]) & 0xFFu] ^
                    (value_ >> 8u);
            }
        }

        std::string Final() const
        {
            const std::uint32_t value = value_ ^ 0xFFFFFFFFu;

            std::ostringstream stream;
            stream << std::hex
                << std::setfill('0')
                << std::setw(8)
                << value;

            return stream.str();
        }

    private:
        static const std::array<std::uint32_t, 256>& GetTable()
        {
            static const std::array<std::uint32_t, 256> table = []
                {
                    std::array<std::uint32_t, 256> result{};

                    for (std::uint32_t i = 0; i < 256u; ++i)
                    {
                        std::uint32_t value = i;

                        for (unsigned bit = 0; bit < 8; ++bit)
                        {
                            value = (value & 1u)
                                ? (0xEDB88320u ^ (value >> 1u))
                                : (value >> 1u);
                        }

                        result[i] = value;
                    }

                    return result;
                }();

            return table;
        }

        std::uint32_t value_;
    };

    // ============================================================
    // MD5
    // ============================================================

    class MD5
    {
    public:
        MD5()
            : state_{
                0x67452301u,
                0xEFCDAB89u,
                0x98BADCFEu,
                0x10325476u
            }
        {
        }

        void Update(const std::uint8_t* data, std::size_t size)
        {
            totalBytes_ += static_cast<std::uint64_t>(size);

            if (bufferSize_ != 0)
            {
                const std::size_t needed = 64u - bufferSize_;
                const std::size_t copied = size < needed ? size : needed;

                std::memcpy(buffer_.data() + bufferSize_, data, copied);

                bufferSize_ += copied;
                data += copied;
                size -= copied;

                if (bufferSize_ == 64u)
                {
                    Transform(buffer_.data());
                    bufferSize_ = 0;
                }
            }

            while (size >= 64u)
            {
                Transform(data);
                data += 64u;
                size -= 64u;
            }

            if (size != 0)
            {
                std::memcpy(buffer_.data(), data, size);
                bufferSize_ = size;
            }
        }

        std::string Final()
        {
            const std::uint64_t bitLength = totalBytes_ * 8u;

            std::uint8_t padding[64]{};
            padding[0] = 0x80u;

            const std::size_t paddingSize =
                bufferSize_ < 56u
                ? 56u - bufferSize_
                : 120u - bufferSize_;

            Update(padding, paddingSize);

            std::uint8_t lengthBytes[8];
            StoreLE64(lengthBytes, bitLength);
            Update(lengthBytes, sizeof(lengthBytes));

            std::uint8_t digest[16];

            for (std::size_t i = 0; i < 4; ++i)
            {
                StoreLE32(digest + i * 4u, state_[i]);
            }

            return BytesToHex(digest, sizeof(digest));
        }

    private:
        void Transform(const std::uint8_t block[64])
        {
            static constexpr std::uint32_t constants[64] = {
                0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
                0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
                0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
                0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
                0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
                0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
                0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
                0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
                0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
                0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
                0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
                0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
                0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
                0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
                0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
                0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
            };

            static constexpr unsigned shifts[64] = {
                7, 12, 17, 22, 7, 12, 17, 22,
                7, 12, 17, 22, 7, 12, 17, 22,
                5, 9, 14, 20, 5, 9, 14, 20,
                5, 9, 14, 20, 5, 9, 14, 20,
                4, 11, 16, 23, 4, 11, 16, 23,
                4, 11, 16, 23, 4, 11, 16, 23,
                6, 10, 15, 21, 6, 10, 15, 21,
                6, 10, 15, 21, 6, 10, 15, 21
            };

            std::uint32_t words[16];

            for (std::size_t i = 0; i < 16; ++i)
            {
                words[i] = LoadLE32(block + i * 4u);
            }

            std::uint32_t a = state_[0];
            std::uint32_t b = state_[1];
            std::uint32_t c = state_[2];
            std::uint32_t d = state_[3];

            for (std::uint32_t i = 0; i < 64u; ++i)
            {
                std::uint32_t function;
                std::uint32_t index;

                if (i < 16u)
                {
                    function = (b & c) | (~b & d);
                    index = i;
                }
                else if (i < 32u)
                {
                    function = (d & b) | (~d & c);
                    index = (5u * i + 1u) & 15u;
                }
                else if (i < 48u)
                {
                    function = b ^ c ^ d;
                    index = (3u * i + 5u) & 15u;
                }
                else
                {
                    function = c ^ (b | ~d);
                    index = (7u * i) & 15u;
                }

                const std::uint32_t oldD = d;

                d = c;
                c = b;
                b += RotateLeft32(
                    a + function + constants[i] + words[index],
                    shifts[i]
                );
                a = oldD;
            }

            state_[0] += a;
            state_[1] += b;
            state_[2] += c;
            state_[3] += d;
        }

        std::array<std::uint32_t, 4> state_;
        std::array<std::uint8_t, 64> buffer_{};
        std::size_t bufferSize_ = 0;
        std::uint64_t totalBytes_ = 0;
    };

    // ============================================================
    // SHA-1
    // ============================================================

    class SHA1
    {
    public:
        SHA1()
            : state_{
                0x67452301u,
                0xEFCDAB89u,
                0x98BADCFEu,
                0x10325476u,
                0xC3D2E1F0u
            }
        {
        }

        void Update(const std::uint8_t* data, std::size_t size)
        {
            totalBytes_ += static_cast<std::uint64_t>(size);

            if (bufferSize_ != 0)
            {
                const std::size_t needed = 64u - bufferSize_;
                const std::size_t copied = size < needed ? size : needed;

                std::memcpy(buffer_.data() + bufferSize_, data, copied);

                bufferSize_ += copied;
                data += copied;
                size -= copied;

                if (bufferSize_ == 64u)
                {
                    Transform(buffer_.data());
                    bufferSize_ = 0;
                }
            }

            while (size >= 64u)
            {
                Transform(data);
                data += 64u;
                size -= 64u;
            }

            if (size != 0)
            {
                std::memcpy(buffer_.data(), data, size);
                bufferSize_ = size;
            }
        }

        std::string Final()
        {
            const std::uint64_t bitLength = totalBytes_ * 8u;

            std::uint8_t padding[64]{};
            padding[0] = 0x80u;

            const std::size_t paddingSize =
                bufferSize_ < 56u
                ? 56u - bufferSize_
                : 120u - bufferSize_;

            Update(padding, paddingSize);

            std::uint8_t lengthBytes[8];
            StoreBE64(lengthBytes, bitLength);
            Update(lengthBytes, sizeof(lengthBytes));

            std::uint8_t digest[20];

            for (std::size_t i = 0; i < 5; ++i)
            {
                StoreBE32(digest + i * 4u, state_[i]);
            }

            return BytesToHex(digest, sizeof(digest));
        }

    private:
        void Transform(const std::uint8_t block[64])
        {
            std::uint32_t words[80];

            for (std::size_t i = 0; i < 16; ++i)
            {
                words[i] = LoadBE32(block + i * 4u);
            }

            for (std::size_t i = 16; i < 80; ++i)
            {
                words[i] = RotateLeft32(
                    words[i - 3] ^
                    words[i - 8] ^
                    words[i - 14] ^
                    words[i - 16],
                    1
                );
            }

            std::uint32_t a = state_[0];
            std::uint32_t b = state_[1];
            std::uint32_t c = state_[2];
            std::uint32_t d = state_[3];
            std::uint32_t e = state_[4];

            for (std::uint32_t i = 0; i < 80u; ++i)
            {
                std::uint32_t function;
                std::uint32_t constant;

                if (i < 20u)
                {
                    function = (b & c) | (~b & d);
                    constant = 0x5A827999u;
                }
                else if (i < 40u)
                {
                    function = b ^ c ^ d;
                    constant = 0x6ED9EBA1u;
                }
                else if (i < 60u)
                {
                    function = (b & c) | (b & d) | (c & d);
                    constant = 0x8F1BBCDCu;
                }
                else
                {
                    function = b ^ c ^ d;
                    constant = 0xCA62C1D6u;
                }

                const std::uint32_t temp =
                    RotateLeft32(a, 5) +
                    function +
                    e +
                    constant +
                    words[i];

                e = d;
                d = c;
                c = RotateLeft32(b, 30);
                b = a;
                a = temp;
            }

            state_[0] += a;
            state_[1] += b;
            state_[2] += c;
            state_[3] += d;
            state_[4] += e;
        }

        std::array<std::uint32_t, 5> state_;
        std::array<std::uint8_t, 64> buffer_{};
        std::size_t bufferSize_ = 0;
        std::uint64_t totalBytes_ = 0;
    };

    // ============================================================
    // SHA-256
    // ============================================================

    class SHA256
    {
    public:
        SHA256()
            : state_{
                0x6A09E667u,
                0xBB67AE85u,
                0x3C6EF372u,
                0xA54FF53Au,
                0x510E527Fu,
                0x9B05688Cu,
                0x1F83D9ABu,
                0x5BE0CD19u
            }
        {
        }

        void Update(const std::uint8_t* data, std::size_t size)
        {
            totalBytes_ += static_cast<std::uint64_t>(size);

            if (bufferSize_ != 0)
            {
                const std::size_t needed = 64u - bufferSize_;
                const std::size_t copied = size < needed ? size : needed;

                std::memcpy(buffer_.data() + bufferSize_, data, copied);

                bufferSize_ += copied;
                data += copied;
                size -= copied;

                if (bufferSize_ == 64u)
                {
                    Transform(buffer_.data());
                    bufferSize_ = 0;
                }
            }

            while (size >= 64u)
            {
                Transform(data);
                data += 64u;
                size -= 64u;
            }

            if (size != 0)
            {
                std::memcpy(buffer_.data(), data, size);
                bufferSize_ = size;
            }
        }

        std::string Final()
        {
            const std::uint64_t bitLength = totalBytes_ * 8u;

            std::uint8_t padding[64]{};
            padding[0] = 0x80u;

            const std::size_t paddingSize =
                bufferSize_ < 56u
                ? 56u - bufferSize_
                : 120u - bufferSize_;

            Update(padding, paddingSize);

            std::uint8_t lengthBytes[8];
            StoreBE64(lengthBytes, bitLength);
            Update(lengthBytes, sizeof(lengthBytes));

            std::uint8_t digest[32];

            for (std::size_t i = 0; i < 8; ++i)
            {
                StoreBE32(digest + i * 4u, state_[i]);
            }

            return BytesToHex(digest, sizeof(digest));
        }

    private:
        void Transform(const std::uint8_t block[64])
        {
            static constexpr std::uint32_t constants[64] = {
                0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
                0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
                0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
                0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
                0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
                0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
                0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
                0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
                0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
            };

            std::uint32_t words[64];

            for (std::size_t i = 0; i < 16; ++i)
            {
                words[i] = LoadBE32(block + i * 4u);
            }

            for (std::size_t i = 16; i < 64; ++i)
            {
                const std::uint32_t s0 =
                    RotateRight32(words[i - 15], 7) ^
                    RotateRight32(words[i - 15], 18) ^
                    (words[i - 15] >> 3u);

                const std::uint32_t s1 =
                    RotateRight32(words[i - 2], 17) ^
                    RotateRight32(words[i - 2], 19) ^
                    (words[i - 2] >> 10u);

                words[i] =
                    words[i - 16] +
                    s0 +
                    words[i - 7] +
                    s1;
            }

            std::uint32_t a = state_[0];
            std::uint32_t b = state_[1];
            std::uint32_t c = state_[2];
            std::uint32_t d = state_[3];
            std::uint32_t e = state_[4];
            std::uint32_t f = state_[5];
            std::uint32_t g = state_[6];
            std::uint32_t h = state_[7];

            for (std::size_t i = 0; i < 64; ++i)
            {
                const std::uint32_t sigma1 =
                    RotateRight32(e, 6) ^
                    RotateRight32(e, 11) ^
                    RotateRight32(e, 25);

                const std::uint32_t choose =
                    (e & f) ^ (~e & g);

                const std::uint32_t temp1 =
                    h +
                    sigma1 +
                    choose +
                    constants[i] +
                    words[i];

                const std::uint32_t sigma0 =
                    RotateRight32(a, 2) ^
                    RotateRight32(a, 13) ^
                    RotateRight32(a, 22);

                const std::uint32_t majority =
                    (a & b) ^ (a & c) ^ (b & c);

                const std::uint32_t temp2 =
                    sigma0 + majority;

                h = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            state_[0] += a;
            state_[1] += b;
            state_[2] += c;
            state_[3] += d;
            state_[4] += e;
            state_[5] += f;
            state_[6] += g;
            state_[7] += h;
        }

        std::array<std::uint32_t, 8> state_;
        std::array<std::uint8_t, 64> buffer_{};
        std::size_t bufferSize_ = 0;
        std::uint64_t totalBytes_ = 0;
    };

    // ============================================================
    // SHA-384 / SHA-512
    // ============================================================

    class SHA512Base
    {
    public:
        enum class Type
        {
            SHA384,
            SHA512
        };

        explicit SHA512Base(Type type)
            : type_(type)
        {
            if (type_ == Type::SHA384)
            {
                state_ = {
                    0xcbbb9d5dc1059ed8ULL,
                    0x629a292a367cd507ULL,
                    0x9159015a3070dd17ULL,
                    0x152fecd8f70e5939ULL,
                    0x67332667ffc00b31ULL,
                    0x8eb44a8768581511ULL,
                    0xdb0c2e0d64f98fa7ULL,
                    0x47b5481dbefa4fa4ULL
                };
            }
            else
            {
                state_ = {
                    0x6a09e667f3bcc908ULL,
                    0xbb67ae8584caa73bULL,
                    0x3c6ef372fe94f82bULL,
                    0xa54ff53a5f1d36f1ULL,
                    0x510e527fade682d1ULL,
                    0x9b05688c2b3e6c1fULL,
                    0x1f83d9abfb41bd6bULL,
                    0x5be0cd19137e2179ULL
                };
            }
        }

        void Update(const std::uint8_t* data, std::size_t size)
        {
            const std::uint64_t oldLow = totalBytesLow_;
            totalBytesLow_ += static_cast<std::uint64_t>(size);

            if (totalBytesLow_ < oldLow)
            {
                ++totalBytesHigh_;
            }

            if (bufferSize_ != 0)
            {
                const std::size_t needed = 128u - bufferSize_;
                const std::size_t copied = size < needed ? size : needed;

                std::memcpy(buffer_.data() + bufferSize_, data, copied);

                bufferSize_ += copied;
                data += copied;
                size -= copied;

                if (bufferSize_ == 128u)
                {
                    Transform(buffer_.data());
                    bufferSize_ = 0;
                }
            }

            while (size >= 128u)
            {
                Transform(data);
                data += 128u;
                size -= 128u;
            }

            if (size != 0)
            {
                std::memcpy(buffer_.data(), data, size);
                bufferSize_ = size;
            }
        }

        std::string Final()
        {
            const std::uint64_t bitLengthHigh =
                (totalBytesHigh_ << 3u) |
                (totalBytesLow_ >> 61u);

            const std::uint64_t bitLengthLow =
                totalBytesLow_ << 3u;

            std::uint8_t padding[128]{};
            padding[0] = 0x80u;

            const std::size_t paddingSize =
                bufferSize_ < 112u
                ? 112u - bufferSize_
                : 240u - bufferSize_;

            Update(padding, paddingSize);

            std::uint8_t lengthBytes[16];
            StoreBE64(lengthBytes, bitLengthHigh);
            StoreBE64(lengthBytes + 8u, bitLengthLow);
            Update(lengthBytes, sizeof(lengthBytes));

            const std::size_t outputWords =
                type_ == Type::SHA384 ? 6u : 8u;

            std::uint8_t digest[64]{};

            for (std::size_t i = 0; i < outputWords; ++i)
            {
                StoreBE64(digest + i * 8u, state_[i]);
            }

            return BytesToHex(digest, outputWords * 8u);
        }

    private:
        void Transform(const std::uint8_t block[128])
        {
            static constexpr std::uint64_t constants[80] = {
                0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
                0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
                0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
                0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
                0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
                0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
                0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
                0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
                0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
                0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
                0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
                0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
                0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
                0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
                0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
                0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
                0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
                0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
                0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
                0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
                0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
                0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
                0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
                0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
                0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
                0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
                0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
                0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
                0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
                0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
                0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
                0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
                0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
                0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
                0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
                0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
                0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
                0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
                0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
                0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
            };

            std::uint64_t words[80];

            for (std::size_t i = 0; i < 16; ++i)
            {
                words[i] = LoadBE64(block + i * 8u);
            }

            for (std::size_t i = 16; i < 80; ++i)
            {
                const std::uint64_t s0 =
                    RotateRight64(words[i - 15], 1) ^
                    RotateRight64(words[i - 15], 8) ^
                    (words[i - 15] >> 7u);

                const std::uint64_t s1 =
                    RotateRight64(words[i - 2], 19) ^
                    RotateRight64(words[i - 2], 61) ^
                    (words[i - 2] >> 6u);

                words[i] =
                    words[i - 16] +
                    s0 +
                    words[i - 7] +
                    s1;
            }

            std::uint64_t a = state_[0];
            std::uint64_t b = state_[1];
            std::uint64_t c = state_[2];
            std::uint64_t d = state_[3];
            std::uint64_t e = state_[4];
            std::uint64_t f = state_[5];
            std::uint64_t g = state_[6];
            std::uint64_t h = state_[7];

            for (std::size_t i = 0; i < 80; ++i)
            {
                const std::uint64_t sigma1 =
                    RotateRight64(e, 14) ^
                    RotateRight64(e, 18) ^
                    RotateRight64(e, 41);

                const std::uint64_t choose =
                    (e & f) ^ (~e & g);

                const std::uint64_t temp1 =
                    h +
                    sigma1 +
                    choose +
                    constants[i] +
                    words[i];

                const std::uint64_t sigma0 =
                    RotateRight64(a, 28) ^
                    RotateRight64(a, 34) ^
                    RotateRight64(a, 39);

                const std::uint64_t majority =
                    (a & b) ^ (a & c) ^ (b & c);

                const std::uint64_t temp2 =
                    sigma0 + majority;

                h = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            state_[0] += a;
            state_[1] += b;
            state_[2] += c;
            state_[3] += d;
            state_[4] += e;
            state_[5] += f;
            state_[6] += g;
            state_[7] += h;
        }

        Type type_;
        std::array<std::uint64_t, 8> state_{};
        std::array<std::uint8_t, 128> buffer_{};
        std::size_t bufferSize_ = 0;

        std::uint64_t totalBytesLow_ = 0;
        std::uint64_t totalBytesHigh_ = 0;
    };
}

bool CalculateFileHashes(
    const std::filesystem::path& filePath,
    HashResult& result,
    std::string& errorMessage)
{
    errorMessage.clear();
    result = {};

    std::ifstream file(filePath, std::ios::binary);

    if (!file)
    {
        errorMessage = "无法打开文件。";
        return false;
    }

    CRC32 crc32;
    MD5 md5;
    SHA1 sha1;
    SHA256 sha256;
    SHA512Base sha384(SHA512Base::Type::SHA384);
    SHA512Base sha512(SHA512Base::Type::SHA512);

    std::vector<std::uint8_t> buffer(FILE_BUFFER_SIZE);

    while (file)
    {
        file.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size())
        );

        const std::streamsize readSize = file.gcount();

        if (readSize > 0)
        {
            const auto size = static_cast<std::size_t>(readSize);

            crc32.Update(buffer.data(), size);
            md5.Update(buffer.data(), size);
            sha1.Update(buffer.data(), size);
            sha256.Update(buffer.data(), size);
            sha384.Update(buffer.data(), size);
            sha512.Update(buffer.data(), size);
        }
    }

    if (file.bad())
    {
        errorMessage = "读取文件时发生错误。";
        return false;
    }

    result.crc32 = crc32.Final();
    result.md5 = md5.Final();
    result.sha1 = sha1.Final();
    result.sha256 = sha256.Final();
    result.sha384 = sha384.Final();
    result.sha512 = sha512.Final();

    return true;
}

bool FilesExactlyEqual(
    const std::filesystem::path& first,
    const std::filesystem::path& second,
    bool& equal,
    std::string& errorMessage)
{
    equal = false;
    errorMessage.clear();

    std::error_code firstError;
    std::error_code secondError;

    const auto firstSize =
        std::filesystem::file_size(first, firstError);

    const auto secondSize =
        std::filesystem::file_size(second, secondError);

    if (firstError || secondError)
    {
        errorMessage = "无法读取一个或两个文件的大小。";
        return false;
    }

    if (firstSize != secondSize)
    {
        equal = false;
        return true;
    }

    std::ifstream firstFile(first, std::ios::binary);
    std::ifstream secondFile(second, std::ios::binary);

    if (!firstFile || !secondFile)
    {
        errorMessage = "无法打开一个或两个待比较文件。";
        return false;
    }

    std::vector<char> firstBuffer(FILE_BUFFER_SIZE);
    std::vector<char> secondBuffer(FILE_BUFFER_SIZE);

    while (true)
    {
        firstFile.read(
            firstBuffer.data(),
            static_cast<std::streamsize>(firstBuffer.size())
        );

        secondFile.read(
            secondBuffer.data(),
            static_cast<std::streamsize>(secondBuffer.size())
        );

        const std::streamsize firstRead = firstFile.gcount();
        const std::streamsize secondRead = secondFile.gcount();

        if (firstRead != secondRead)
        {
            equal = false;
            return true;
        }

        if (firstRead == 0)
        {
            break;
        }

        if (std::memcmp(
            firstBuffer.data(),
            secondBuffer.data(),
            static_cast<std::size_t>(firstRead)) != 0)
        {
            equal = false;
            return true;
        }
    }

    if (firstFile.bad() || secondFile.bad())
    {
        errorMessage = "比较过程中读取文件失败。";
        return false;
    }

    equal = true;
    return true;
}
