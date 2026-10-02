// ============================================================================
//  EHsc · core.h
//  引擎层：作业控制（进度 / 暂停 / 取消 / 速度统计）、目录扫描、
//          多线程任务调度、文件哈希、字节级比较、算法基准测试、通用格式化。
// ============================================================================
#pragma once

#include "hashes.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ehsc {

// ============================================================ 输出格式 ====
enum class OutFormat { Text, Csv, Json };

bool parseFormat(std::string_view text, OutFormat& out);
const char* formatName(OutFormat f);        // "txt" / "csv" / "json"
const char* formatDisplay(OutFormat f);     // "文本" / "CSV" / "JSON"

// ======================================================== 进度与作业控制 ====
struct ProgressSnapshot {
    std::wstring unit;             // 阶段名称，如 "计算哈希"
    std::wstring currentFile;      // 当前正在处理的文件
    uint64_t bytesDone = 0;
    uint64_t bytesTotal = 0;
    uint64_t filesDone = 0;
    uint64_t filesTotal = 0;
    uint64_t filesFailed = 0;
    double   elapsed = 0;          // 有效运行秒数（不含暂停时间）
    double   speedNow = 0;         // 瞬时速度 字节/秒
    double   speedAvg = 0;         // 平均速度 字节/秒
    double   eta = -1;             // 预计剩余秒数，<0 表示未知
    bool     paused = false;
    bool     cancelled = false;
    bool     active = false;       // 是否处于运行状态
};

// 线程安全的作业控制器：工作线程通过 checkpoint() 响应暂停与取消
class JobControl {
public:
    void beginRun(const std::wstring& unit);
    void endRun();
    void setUnit(const std::wstring& unit);

    // 工作线程调用：若已取消返回 false；若处于暂停状态则阻塞直到继续或取消
    bool checkpoint();
    void requestCancel();
    void togglePause();
    void setPaused(bool paused);
    bool paused() const { return paused_.load(); }
    bool cancelled() const { return cancelled_.load(); }
    void reset();   // 复位所有计数与标志（复用同一控制器跑下一个任务）

    void setTotals(uint64_t files, uint64_t bytes);
    void setCurrentFile(const std::wstring& path, uint64_t size);
    void addBytes(uint64_t n);
    void fileFinished(bool ok);

    ProgressSnapshot snapshot();
    double elapsedSeconds() const;

private:
    std::atomic<uint64_t> bytesDone_{0};
    std::atomic<uint64_t> bytesTotal_{0};
    std::atomic<uint64_t> filesDone_{0};
    std::atomic<uint64_t> filesTotal_{0};
    std::atomic<uint64_t> filesFailed_{0};
    std::atomic<bool>     cancelled_{false};
    std::atomic<bool>     paused_{false};
    std::atomic<bool>     active_{false};
    std::atomic<double>   pausedSeconds_{0.0};

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::wstring            currentFile_;
    std::wstring            unit_;
    std::chrono::steady_clock::time_point startTime_{};
    std::chrono::steady_clock::time_point pauseStartTime_{};
    bool                    pauseStartValid_ = false;
    std::deque<std::pair<double, uint64_t>> samples_;   // (时刻, 累计字节) 速度采样窗口
};

// ================================================================ 扫描 ====
struct ScanOptions {
    bool                       recursive = true;
    std::vector<std::wstring>  filters;        // 通配符，如 *.exe；为空表示全部
    bool                       includeHidden = true;
    bool                       includeSystem = true;
    bool                       followReparse = false;
    size_t                     maxFiles = 0;   // 0 表示不限制
};

struct FileEntry {
    std::wstring path;        // 完整路径
    uint64_t     size = 0;
    int64_t      mtime = 0;   // Unix 秒（UTC）
    int          rootIndex = 0;
};

struct ScanResult {
    std::vector<FileEntry>    files;
    std::vector<std::wstring> roots;       // 用户给定的根路径（绝对化后）
    std::vector<std::wstring> errors;      // 无法访问的目录等
    uint64_t                  totalBytes = 0;
    uint64_t                  dirCount = 0;
    bool                      cancelled = false;
};

ScanResult scanTargets(const std::vector<std::wstring>& targets, const ScanOptions& options,
                       JobControl* control);

// ============================================================ 哈希任务 ====
struct HashRecord {
    std::wstring              path;      // 完整路径
    std::wstring              rel;       // 相对根目录的路径（用于清单）
    uint64_t                  size = 0;
    int64_t                   mtime = 0;
    std::vector<std::string>  digests;   // 与算法列表一一对应的小写十六进制
    bool                      ok = true;
    bool                      cancelled = false;   // 因用户取消而未处理完
    std::wstring              error;
    double                    seconds = 0;
};

struct HashJobOptions {
    std::vector<Algo> algos{Algo::SHA256};
    int               threads = 0;          // 0 = 自动
    size_t            bufferSize = 4u << 20;  // 单次读取大小（4 MiB）
    bool              overlap = true;       // 读取/计算流水线（单文件也能并行 I/O）
    ScanOptions       scan;
    bool              sortByName = true;
};

struct HashJobResult {
    std::vector<HashRecord>   records;
    std::vector<std::wstring> roots;
    std::vector<std::wstring> errors;       // 扫描阶段错误
    uint64_t                  totalBytes = 0;
    uint64_t                  okFiles = 0;
    uint64_t                  failedFiles = 0;
    uint64_t                  cancelledFiles = 0;
    double                    elapsed = 0;
    bool                      cancelled = false;
};

struct SingleHashResult {
    bool                      ok = false;
    bool                      cancelled = false;
    std::wstring              error;
    std::vector<std::string>  digests;
    uint64_t                  bytes = 0;
    double                    seconds = 0;
};

// 通用多线程调度：把 count 个下标分发给 threads 个工作线程
void parallelFor(size_t count, int threads, const std::function<void(size_t)>& body);

// 计算单个文件的多算法摘要（bufferSize 为每次读取字节数）
SingleHashResult hashSingleFile(const std::wstring& path, const std::vector<Algo>& algos,
                                size_t bufferSize, bool overlap, JobControl* control,
                                bool reportBytes);

// 批量哈希任务（自动扫描 + 多线程 + 进度统计）
HashJobResult runHashJob(const std::vector<std::wstring>& targets, const HashJobOptions& options,
                         JobControl& control);

// ============================================================ 文件比较 ====
struct CompareOptions {
    size_t            bufferSize = 4u << 20;
    bool              quick = false;      // 只比较大小
    bool              withHash = false;   // 额外计算并比较哈希
    std::vector<Algo> algos{Algo::SHA256};
    size_t            windowSize = 48;    // 首个差异处保留的字节数
};

struct CompareResult {
    int                       status = 0;       // 0 相同 / 1 不同 / 2 出错 / 3 取消
    bool                      sizeEqual = false;
    bool                      contentEqual = false;
    uint64_t                  sizeA = 0;
    uint64_t                  sizeB = 0;
    uint64_t                  compared = 0;     // 实际逐字节比较的字节数
    uint64_t                  firstDiff = UINT64_MAX;   // 首个不同字节的偏移
    std::vector<uint8_t>      windowA;          // 首个差异处的字节窗口
    std::vector<uint8_t>      windowB;
    uint64_t                  windowOffset = 0;
    std::vector<std::string>  digestsA;
    std::vector<std::string>  digestsB;
    std::wstring              error;
    double                    seconds = 0;
};

CompareResult compareFiles(const std::wstring& pathA, const std::wstring& pathB,
                           const CompareOptions& options, JobControl& control);

// ========================================================== 基准测试 ====
struct BenchRow {
    Algo   algo = Algo::SHA256;
    double seconds = 0;     // 单次遍历用时
    double mbPerSec = 0;
    double cyclesPerByte = 0;
    size_t iterations = 0;
};

std::vector<BenchRow> benchmarkMemory(const void* data, size_t size, const std::vector<Algo>& algos,
                                      double minSeconds, JobControl& control);

// ============================================================ 工具函数 ====
int         effectiveThreadCount(int requested);
std::string formatByteSize(uint64_t bytes);          // 1.23 GB
std::string formatCount(uint64_t value);             // 1,234,567
std::string formatDuration(double seconds);          // 00:01:23.4
std::string formatSpeed(double bytesPerSecond);      // 312.5 MB/s
std::string formatUnixTime(int64_t unixSeconds);     // 2024-05-01 12:00:00（本地时区）
std::string isoTimeUtc(int64_t unixSeconds);         // 2024-05-01T04:00:00Z
std::string isoNowUtc();                             // 当前 UTC 时间 ISO8601
std::string trimText(const std::string& text);
bool        equalsIgnoreCaseAscii(std::string_view a, std::string_view b);
std::wstring stripQuotes(const std::wstring& text);

// UTF-8 文本的显示宽度（中日韩全角字符按 2 列计）与按显示宽度右补空格
size_t      textDisplayWidth(const std::string& utf8Text);
std::string padToWidth(const std::string& utf8Text, size_t width);

}  // namespace ehsc
