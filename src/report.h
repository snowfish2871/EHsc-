// ============================================================================
//  EHsc · report.h
//  结果输出（TXT / CSV / JSON）、清单文件读写、清单校验。
// ============================================================================
#pragma once

#include "core.h"
#include "win32_utils.h"

#include <string>
#include <vector>

namespace ehsc {

// ============================================================ 报告输出 ====
struct ReportMeta {
    std::string               tool = "EHsc";
    std::string               version;
    std::string               generated;      // ISO8601 UTC
    std::string               host;           // 计算机名
    std::vector<Algo>         algos;
    std::vector<std::wstring> roots;
};

// 生成报告文本（不含进度信息）
std::string buildTextReport(const HashJobResult& result, const ReportMeta& meta);
std::string buildCsvReport(const HashJobResult& result, const ReportMeta& meta);
std::string buildJsonReport(const HashJobResult& result, const ReportMeta& meta);
std::string buildReport(const HashJobResult& result, const ReportMeta& meta, OutFormat format);

// ============================================================ 清单解析 ====
enum class ManifestKind { Text, Csv, Json };

struct ManifestDigest {
    Algo        algo = Algo::SHA256;
    std::string hex;
};

struct ManifestEntry {
    std::wstring                path;      // 清单中记录的（通常是相对）路径
    uint64_t                    size = 0;
    bool                        hasSize = false;
    int64_t                     mtime = 0;
    bool                        hasMtime = false;
    std::wstring                error;     // 生成清单时的错误说明（该条目无摘要）
    std::vector<ManifestDigest> digests;
};

struct Manifest {
    bool                       ok = false;
    std::wstring               error;
    ManifestKind               kind = ManifestKind::Text;
    std::vector<Algo>          algos;      // 清单中出现的全部算法
    std::vector<std::wstring>  roots;      // 清单记录的基准目录（用于解析相对路径）
    std::vector<ManifestEntry> entries;
};

Manifest parseManifestText(const std::string& text, const std::wstring& fileNameHint);
Manifest parseManifestFile(const std::wstring& path);

// ============================================================ 清单校验 ====
enum class VerifyStatus { Passed, Mismatch, Missing, SizeMismatch, ReadError, Skipped };

struct VerifyItem {
    std::wstring              path;        // 实际校验的绝对路径
    std::wstring              rel;         // 清单中的路径
    VerifyStatus              status = VerifyStatus::Passed;
    std::wstring              detail;      // 人类可读的说明
    std::wstring              manifestNote; // 清单里记录的生成期错误
    std::vector<std::string>  expected;    // 与 algos 对应
    std::vector<std::string>  actual;
    std::vector<Algo>         algos;
    uint64_t                  size = 0;
    double                    seconds = 0;
};

struct VerifyOptions {
    std::vector<Algo> filterAlgos;          // 空 = 使用清单里的全部算法
    size_t            bufferSize = 4u << 20;
    int               threads = 0;
    bool              checkSize = true;
    bool              reportAll = false;    // 通过项是否也放进 items
    // 相对路径解析：true 表示用户显式指定了基准目录（只按该目录解析）；
    // false 表示优先使用清单自身记录的根目录（清单可放在任意位置）。
    bool              baseDirIsExplicit = false;
};

struct VerifyResult {
    uint64_t                 total = 0;
    uint64_t                 passed = 0;
    uint64_t                 failed = 0;    // 摘要不一致
    uint64_t                 missing = 0;
    uint64_t                 errors = 0;    // 读取失败
    uint64_t                 skipped = 0;
    uint64_t                 bytesChecked = 0;
    double                   elapsed = 0;
    bool                     cancelled = false;
    std::wstring             resolvedBase;  // 实际用于解析相对路径的基准目录
    std::vector<VerifyItem>  items;
};

VerifyResult verifyManifest(const Manifest& manifest, const std::wstring& baseDir,
                            const VerifyOptions& options, JobControl& control);

}  // namespace ehsc
