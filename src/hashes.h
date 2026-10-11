// ============================================================================
//  EHsc · hashes.h
//  纯 C++ 实现的哈希算法库（无 OpenSSL / Crypto++ / Boost 等任何第三方依赖）
//
//  支持算法：
//    CRC32(32)  MD5(128)  SHA-1(160)  SHA-256  SHA-384  SHA-512
//    SHA3-256   SHA3-512  BLAKE2s-256  BLAKE2b-512
//
//  设计要点：
//    * 所有算法均为流式（update/final）状态机，只用标准库基础类型，
//      状态结构全部是“平凡可复制”的 POD，便于放进联合体与容器中。
//    * Hasher 通过内部联合体实现多态，无虚函数、无堆分配，可安全被多线程
//      各自持有（每个线程一个实例即可）。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ehsc {

// 源文件必须按 UTF-8 编码编译（MSVC 需要 /utf-8，CMakeLists 已自动添加）。
// 若此断言失败，说明编译器把中文字面量当成本地代码页解释，界面会乱码。
static_assert(sizeof(u8"中") == 4, "本项目要求以 UTF-8 编码编译源文件（MSVC: /utf-8）");

// ---------------------------------------------------------------- 算法枚举 --
enum class Algo : int {
    CRC32 = 0,
    MD5,
    SHA1,
    SHA256,
    SHA384,
    SHA512,
    SHA3_256,
    SHA3_512,
    BLAKE2S_256,
    BLAKE2B_512,
    Count
};

constexpr int kAlgoCount = static_cast<int>(Algo::Count);

struct AlgoInfo {
    Algo        algo;         // 枚举值
    const char* id;           // 机器标识（命令行 / CSV / JSON 使用），如 "sha3-256"
    const char* display;      // 显示名，如 "SHA3-256"
    int         digestBytes;  // 摘要长度（字节）
    bool        secure;       // 是否可用于安全用途：
                              //   CRC32 / MD5 / SHA-1 为 false —— 它们已完全不适合
                              //   防篡改、数字签名、口令存储等场景，只能做完整性检查
    const char* note;         // 中文备注
};

const AlgoInfo& algoInfo(Algo a);
const AlgoInfo* algoTable();  // 返回 kAlgoCount 个元素

// 列表中是否包含不适合安全用途的算法（CRC32 / MD5 / SHA-1）
bool hasInsecureAlgo(const std::vector<Algo>& algos);
// 返回不适合安全用途的算法显示名列表，如 "CRC32, MD5, SHA-1"
std::string insecureAlgoList(const std::vector<Algo>& algos);

// 宽松解析："SHA-256" / "sha256" / "sha_256" 均可识别；"all"/"*" 表示全部。
bool parseAlgo(std::string_view text, Algo& out);

// 解析逗号/分号/空格分隔的算法列表；无法识别的词条会写入 bad（若非空）。
std::vector<Algo> parseAlgoList(std::string_view text, std::string* bad = nullptr);

std::string algoIdList(const std::vector<Algo>& algos, const char* sep = ",");
std::string algoDisplayList(const std::vector<Algo>& algos, const char* sep = ", ");

// 去重并保持枚举顺序；空列表时返回默认算法。
std::vector<Algo> normalizeAlgoList(const std::vector<Algo>& algos);
std::vector<Algo> allAlgos();

// ------------------------------------------------------------ 十六进制工具 --
std::string toHex(const uint8_t* data, size_t len, bool upper = false);
bool fromHex(std::string_view hex, std::vector<uint8_t>& out);

// ------------------------------------------------------------ 内部状态定义 --
namespace detail {

struct Crc32State {                       // CRC-32/ISO-HDLC（反射多项式 0xEDB88320）
    uint32_t crc;
};

struct Md5State {
    uint32_t h[4];
    uint64_t total;                       // 已处理字节数
    uint8_t  buf[64];
    size_t   buflen;
};

struct Sha1State {
    uint32_t h[5];
    uint64_t total;
    uint8_t  buf[64];
    size_t   buflen;
};

struct Sha256State {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    size_t   buflen;
};

struct Sha512State {
    uint64_t h[8];
    uint64_t totalLo;                     // 128 位消息长度（低 64 位）
    uint64_t totalHi;                     // 高 64 位（本工具不会用到，仅保证形式正确）
    uint8_t  buf[128];
    size_t   buflen;
};

struct Sha3State {                        // Keccak-f[1600] 海绵结构
    uint64_t s[25];                       // 1600 位状态（x + 5y）
    uint8_t  buf[168];                    // 最大 rate = 200 - 2*16
    size_t   rate;                        // 字节速率：SHA3-256=136, SHA3-512=72
    size_t   pt;                          // 当前块内偏移
    int      outBytes;
};

struct Blake2sState {
    uint32_t h[8];
    uint32_t t[2];                        // 64 位计数器
    uint8_t  buf[64];
    size_t   buflen;
};

struct Blake2bState {
    uint64_t h[8];
    uint64_t t[2];                        // 128 位计数器
    uint8_t  buf[128];
    size_t   buflen;
};

union State {
    Crc32State   crc32;
    Md5State     md5;
    Sha1State    sha1;
    Sha256State  sha256;
    Sha512State  sha512;
    Sha3State    sha3;
    Blake2sState b2s;
    Blake2bState b2b;
};

}  // namespace detail

// ------------------------------------------------------------------ Hasher --
class Hasher {
public:
    Hasher();
    explicit Hasher(Algo a);

    void init(Algo a);                    // 复位并选择算法
    void reset();                         // 用同一算法复位
    void update(const void* data, size_t len);
    void final(uint8_t* out);             // 输出 digestBytes() 字节摘要

    Algo algo() const { return algo_; }
    const AlgoInfo& info() const { return algoInfo(algo_); }
    int  digestBytes() const { return digestBytes_; }
    size_t totalBytes() const { return totalIn_; }

    // final + 转十六进制 + 自动复位（可复用同一实例计算下一个文件）
    std::string hexDigest(bool upper = false);

private:
    Algo              algo_;
    int               digestBytes_;
    uint64_t          totalIn_;
    detail::State     st_;
};

// 一次性计算内存中的哈希（便于自检与单元测试）
std::string hashMemory(Algo a, const void* data, size_t len, bool upper = false);

// ---------------------------------------------------------------- 内置自检 --
struct SelfTestItem {
    std::string name;      // 用例名，如 "SHA-256(\"abc\")"
    std::string expected;
    std::string actual;
    bool        pass = false;
};

struct SelfTestReport {
    std::vector<SelfTestItem> items;
    int passed = 0;
    int failed = 0;
    bool ok() const { return failed == 0; }
};

// 使用标准测试向量（NIST / RFC 1321 / RFC 3174 / RFC 7693 / FIPS 202）验证实现。
// 同时包含跨块边界与百万字节的流式用例。
SelfTestReport runSelfTest();

const char* versionString();
const char* buildInfoString();

}  // namespace ehsc
