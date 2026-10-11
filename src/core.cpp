// ============================================================================
//  EHsc · core.cpp
//  引擎实现：作业控制、目录扫描、线程池调度、重叠 I/O 哈希、字节比较、基准测试。
// ============================================================================
#include "core.h"
#include "win32_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

namespace ehsc {
namespace {

using Clock = std::chrono::steady_clock;

double nowSeconds() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::wstring toLowerWide(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return out;
}

// 计算相对根目录的路径（大小写不敏感前缀匹配）
std::wstring relativeTo(const std::wstring& rootIn, const std::wstring& path) {
    // 根目录可能带尾部反斜杠（例如用户输入 "D:\data\" 或以盘根 "C:\" 为目标），
    // 必须先剥掉，否则前缀比较会错位，导致相对路径退化成裸文件名、目录结构全部丢失。
    std::wstring root = rootIn;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    if (root.empty() || path.size() <= root.size()) return baseName(path);
    bool prefix = true;
    for (size_t i = 0; i < root.size(); ++i) {
        wchar_t a = root[i];
        wchar_t b = path[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) {
            prefix = false;
            break;
        }
    }
    if (!prefix) return baseName(path);
    const wchar_t sep = path[root.size()];
    if (sep != L'\\' && sep != L'/') return baseName(path);
    return path.substr(root.size() + 1);
}

// ------------------------------------------------------- 重叠 I/O 读取器 --
// 一个读取线程 + 环形缓冲：把“磁盘读取”与“哈希计算”重叠起来，
// 使单文件哈希也能吃到多线程/异步 I/O 的收益。缓冲区严格 FIFO，保证字节顺序。
class OverlapReader {
public:
    OverlapReader(WinFile& file, size_t chunkSize, size_t slots)
        : file_(file), chunkSize_(chunkSize) {
        if (slots < 2) slots = 2;
        for (size_t i = 0; i < slots; ++i) {
            Chunk c;
            c.data.resize(chunkSize_);
            free_.push_back(std::move(c));
        }
    }

    ~OverlapReader() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cvFree_.notify_all();
        cvReady_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    OverlapReader(const OverlapReader&) = delete;
    OverlapReader& operator=(const OverlapReader&) = delete;

    void start() { thread_ = std::thread([this] { readerLoop(); }); }

    // 取下一块数据；返回 false 表示到达文件尾或出错（err 非空即出错）
    bool next(const uint8_t*& data, size_t& len, std::wstring& err) {
        data = nullptr;
        len = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        cvReady_.wait(lock, [this] { return stop_ || !ready_.empty() || state_ != 0; });
        if (!ready_.empty()) {
            current_ = std::move(ready_.front());
            ready_.pop_front();
            hasCurrent_ = true;
            data = current_.data.data();
            len = current_.len;
            return true;
        }
        if (state_ == 2) err = error_;
        return false;
    }

    // 消费完当前块后归还缓冲
    void recycle() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!hasCurrent_) return;
            current_.len = 0;
            free_.push_back(std::move(current_));
            hasCurrent_ = false;
        }
        cvFree_.notify_all();
    }

private:
    struct Chunk {
        std::vector<uint8_t> data;
        size_t               len = 0;
    };

    void readerLoop() {
        for (;;) {
            Chunk chunk;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cvFree_.wait(lock, [this] { return stop_ || !free_.empty(); });
                if (stop_) return;
                chunk = std::move(free_.front());
                free_.pop_front();
            }

            size_t got = 0;
            std::wstring err;
            const bool ok = file_.read(chunk.data.data(), chunkSize_, got, err);

            bool finished = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!ok) {
                    state_ = 2;
                    error_ = err;
                    finished = true;
                } else if (got == 0) {
                    state_ = 1;
                    finished = true;
                } else {
                    chunk.len = got;
                    ready_.push_back(std::move(chunk));
                }
            }
            cvReady_.notify_all();
            if (finished) return;
        }
    }

    WinFile&                 file_;
    size_t                   chunkSize_;
    std::deque<Chunk>        free_;
    std::deque<Chunk>        ready_;
    Chunk                    current_;
    bool                     hasCurrent_ = false;
    int                      state_ = 0;   // 0 运行 / 1 结束 / 2 出错
    std::wstring             error_;
    bool                     stop_ = false;
    std::thread              thread_;
    std::mutex               mutex_;
    std::condition_variable  cvFree_;
    std::condition_variable  cvReady_;
};

// ------------------------------------------------------------- 扫描实现 --
void scanDirectory(const std::wstring& dir, int rootIndex, int depth, const ScanOptions& options,
                   ScanResult& out, JobControl* control) {
    if (control && control->cancelled()) {
        out.cancelled = true;
        return;
    }
    if (control) control->setCurrentFile(L"扫描目录  " + dir, 0);
    if (depth > 128) {
        out.errors.push_back(L"目录层级过深，已跳过：" + dir);
        return;
    }
    std::vector<DirEntry> entries;
    std::wstring err;
    if (!listDirectory(dir, entries, err)) {
        out.errors.push_back(dir + L"（" + err + L"）");
        return;
    }
    std::sort(entries.begin(), entries.end(),
              [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });

    for (const DirEntry& entry : entries) {
        if (control && control->cancelled()) {
            out.cancelled = true;
            return;
        }
        if (entry.isDir) {
            out.dirCount++;
            if (!options.recursive) continue;
            if (!options.followReparse && (entry.attributes & FileAttrReparsePoint) != 0)
                continue;   // 跳过符号链接/联接点，避免循环
            scanDirectory(entry.fullPath, rootIndex, depth + 1, options, out, control);
            if (out.cancelled) return;
            continue;
        }
        if (!options.includeHidden && (entry.attributes & FileAttrHidden) != 0) continue;
        if (!options.includeSystem && (entry.attributes & FileAttrSystem) != 0) continue;
        if (!options.filters.empty()) {
            bool matched = false;
            for (const std::wstring& pattern : options.filters) {
                if (wildcardMatch(pattern, entry.name)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) continue;
        }
        if (options.maxFiles != 0 && out.files.size() >= options.maxFiles) return;
        FileEntry file;
        file.path = entry.fullPath;
        file.size = entry.size;
        file.mtime = entry.mtime;
        file.rootIndex = rootIndex;
        out.totalBytes += entry.size;
        out.files.push_back(std::move(file));
    }
}

}  // namespace

// ============================================================ 输出格式 ====
bool parseFormat(std::string_view text, OutFormat& out) {
    const std::string key = trimText(std::string(text));
    if (equalsIgnoreCaseAscii(key, "txt") || equalsIgnoreCaseAscii(key, "text") ||
        equalsIgnoreCaseAscii(key, "文本")) {
        out = OutFormat::Text;
        return true;
    }
    if (equalsIgnoreCaseAscii(key, "csv")) {
        out = OutFormat::Csv;
        return true;
    }
    if (equalsIgnoreCaseAscii(key, "json")) {
        out = OutFormat::Json;
        return true;
    }
    return false;
}

const char* formatName(OutFormat f) {
    switch (f) {
        case OutFormat::Text: return "txt";
        case OutFormat::Csv: return "csv";
        case OutFormat::Json: return "json";
    }
    return "txt";
}

const char* formatDisplay(OutFormat f) {
    switch (f) {
        case OutFormat::Text: return "文本";
        case OutFormat::Csv: return "CSV 表格";
        case OutFormat::Json: return "JSON";
    }
    return "文本";
}

// ============================================================ JobControl ====
void JobControl::reset() {
    bytesDone_.store(0);
    bytesTotal_.store(0);
    filesDone_.store(0);
    filesTotal_.store(0);
    filesFailed_.store(0);
    cancelled_.store(false);
    paused_.store(false);
    active_.store(false);
    pausedSeconds_.store(0.0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        currentFile_.clear();
        unit_.clear();
        samples_.clear();
        pauseStartValid_ = false;
    }
}

void JobControl::beginRun(const std::wstring& unit) {
    cancelled_.store(false);
    paused_.store(false);
    pausedSeconds_.store(0.0);
    bytesDone_.store(0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        unit_ = unit;
        samples_.clear();
        pauseStartValid_ = false;
        startTime_ = Clock::now();
    }
    active_.store(true);
}

void JobControl::endRun() {
    // 若结束时仍处于暂停，把最后一段暂停时间也计入
    if (paused_.load()) setPaused(false);
    active_.store(false);
}

void JobControl::setUnit(const std::wstring& unit) {
    std::lock_guard<std::mutex> lock(mutex_);
    unit_ = unit;
}

bool JobControl::checkpoint() {
    if (cancelled_.load()) return false;
    if (paused_.load()) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !paused_.load() || cancelled_.load(); });
    }
    return !cancelled_.load();
}

void JobControl::requestCancel() {
    cancelled_.store(true);
    paused_.store(false);
    cv_.notify_all();
}

void JobControl::setPaused(bool paused) {
    const bool previous = paused_.exchange(paused);
    if (previous == paused) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (paused) {
            pauseStartTime_ = Clock::now();
            pauseStartValid_ = true;
        } else if (pauseStartValid_) {
            const double delta =
                std::chrono::duration<double>(Clock::now() - pauseStartTime_).count();
            pausedSeconds_.store(pausedSeconds_.load() + delta);
            pauseStartValid_ = false;
        }
    }
    if (!paused) cv_.notify_all();
}

void JobControl::togglePause() { setPaused(!paused_.load()); }

void JobControl::setTotals(uint64_t files, uint64_t bytes) {
    filesTotal_.store(files);
    bytesTotal_.store(bytes);
}

void JobControl::setCurrentFile(const std::wstring& path, uint64_t /*size*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentFile_ = path;
}

void JobControl::addBytes(uint64_t n) { bytesDone_.fetch_add(n); }

void JobControl::fileFinished(bool ok) {
    filesDone_.fetch_add(1);
    if (!ok) filesFailed_.fetch_add(1);
}

double JobControl::elapsedSeconds() const {
    std::chrono::steady_clock::time_point start;
    bool pausedNow = false;
    std::chrono::steady_clock::time_point pauseStart;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        start = startTime_;
        pausedNow = pauseStartValid_;
        pauseStart = pauseStartTime_;
    }
    if (start.time_since_epoch().count() == 0) return 0.0;
    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    elapsed -= pausedSeconds_.load();
    if (pausedNow) elapsed -= std::chrono::duration<double>(Clock::now() - pauseStart).count();
    return elapsed > 0 ? elapsed : 0.0;
}

ProgressSnapshot JobControl::snapshot() {
    ProgressSnapshot snap;
    snap.bytesDone = bytesDone_.load();
    snap.bytesTotal = bytesTotal_.load();
    snap.filesDone = filesDone_.load();
    snap.filesTotal = filesTotal_.load();
    snap.filesFailed = filesFailed_.load();
    snap.paused = paused_.load();
    snap.cancelled = cancelled_.load();
    snap.active = active_.load();
    snap.elapsed = elapsedSeconds();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap.currentFile = currentFile_;
        snap.unit = unit_;
    }
    snap.speedAvg = snap.elapsed > 0.02 ? static_cast<double>(snap.bytesDone) / snap.elapsed : 0.0;

    // 瞬时速度：最近约 1.5 秒的滑动窗口
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const double t = nowSeconds();
        samples_.emplace_back(t, snap.bytesDone);
        while (samples_.size() > 2 && t - samples_.front().first > 1.5) samples_.pop_front();
        if (samples_.size() >= 2) {
            const double dt = samples_.back().first - samples_.front().first;
            const uint64_t db = samples_.back().second - samples_.front().second;
            if (dt > 0.1 && db > 0)
                snap.speedNow = static_cast<double>(db) / dt;
            else
                snap.speedNow = snap.speedAvg;
        } else {
            snap.speedNow = snap.speedAvg;
        }
    }

    if (snap.speedAvg > 1024.0 && snap.bytesTotal > snap.bytesDone) {
        snap.eta = static_cast<double>(snap.bytesTotal - snap.bytesDone) / snap.speedAvg;
    } else if (snap.bytesTotal > 0 && snap.bytesDone >= snap.bytesTotal) {
        snap.eta = 0.0;
    } else {
        snap.eta = -1.0;
    }
    return snap;
}

// ================================================================ 扫描 ====
ScanResult scanTargets(const std::vector<std::wstring>& targets, const ScanOptions& options,
                       JobControl* control) {
    ScanResult result;
    std::vector<std::wstring> seen;   // 去重（小写完整路径）

    auto alreadySeen = [&seen](const std::wstring& path) {
        const std::wstring key = toLowerWide(path);
        for (const std::wstring& s : seen) {
            if (s == key) return true;
        }
        seen.push_back(key);
        return false;
    };

    for (const std::wstring& raw : targets) {
        if (control && control->cancelled()) {
            result.cancelled = true;
            break;
        }
        const std::wstring target = fullPath(stripQuotes(raw));
        if (target.empty()) continue;

        FileInfo info;
        std::wstring err;
        if (!getFileInfo(target, info, err)) {
            result.errors.push_back(target + L"（" + err + L"）");
            continue;
        }

        const int rootIndex = static_cast<int>(result.roots.size());
        result.roots.push_back(info.isDir ? target : parentDir(target));

        if (info.isDir) {
            scanDirectory(target, rootIndex, 0, options, result, control);
        } else {
            bool matched = options.filters.empty();
            for (const std::wstring& pattern : options.filters) {
                if (wildcardMatch(pattern, baseName(target))) {
                    matched = true;
                    break;
                }
            }
            if (!matched) continue;
            if (alreadySeen(target)) continue;
            FileEntry file;
            file.path = target;
            file.size = info.size;
            file.mtime = info.mtime;
            file.rootIndex = rootIndex;
            result.totalBytes += info.size;
            result.files.push_back(std::move(file));
        }
    }
    return result;
}

// ========================================================== 线程池调度 ====
void parallelFor(size_t count, int threads, const std::function<void(size_t)>& body) {
    if (count == 0) return;
    const size_t workers =
        std::min<size_t>(count, static_cast<size_t>(threads > 0 ? threads : 1));
    if (workers <= 1) {
        for (size_t i = 0; i < count; ++i) body(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t t = 0; t < workers; ++t) {
        pool.emplace_back([&] {
            for (;;) {
                const size_t index = next.fetch_add(1);
                if (index >= count) break;
                try {
                    body(index);
                } catch (...) {
                    // 单个任务抛出异常不应终止整个进程
                }
            }
        });
    }
    for (std::thread& thread : pool) {
        if (thread.joinable()) thread.join();
    }
}

// --------------------------------------------------- 算法级并行（同文件） --
// 多个算法消费的是同一份字节流，彼此完全独立，因此可以并行：
// 把算法分成 G 组，每组一个线程，所有线程对同一块缓冲并行计算。
// 用栅栏保证"发布一块 → 全部算完 → 再读下一块"，所以每个算法看到的字节顺序
// 与单线程完全一致（摘要结果不变），只是把 CPU 时间摊到了多个核心上。
class Barrier {
public:
    explicit Barrier(size_t parties) : total_(parties) {}

    void arriveAndWait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const uint64_t generation = generation_;
        if (++arrived_ == total_) {
            arrived_ = 0;
            ++generation_;
            cv_.notify_all();
            return;
        }
        cv_.wait(lock, [this, generation] { return generation_ != generation; });
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    size_t                  total_;
    size_t                  arrived_ = 0;
    uint64_t                generation_ = 0;
};

// 各算法相对速度（MB/s，来自实际测量的粗略比例），用于把算法均衡地分到各组
double estimatedCostPerByte(Algo algo) {
    switch (algo) {
        case Algo::CRC32:      return 1.0 / 640.0;
        case Algo::MD5:        return 1.0 / 820.0;
        case Algo::SHA1:       return 1.0 / 630.0;
        case Algo::SHA256:     return 1.0 / 450.0;
        case Algo::SHA384:     return 1.0 / 670.0;
        case Algo::SHA512:     return 1.0 / 670.0;
        case Algo::SHA3_256:   return 1.0 / 344.0;
        case Algo::SHA3_512:   return 1.0 / 174.0;
        case Algo::BLAKE2S_256: return 1.0 / 460.0;
        case Algo::BLAKE2B_512: return 1.0 / 738.0;
        case Algo::Count:      break;
    }
    return 1.0 / 400.0;
}

// 把算法按下标分配到 groups 组：按"预计耗时"从大到小，每次放进当前最轻的一组（LPT 近似）
std::vector<std::vector<size_t>> partitionAlgorithms(const std::vector<Algo>& algos, int groups) {
    std::vector<std::vector<size_t>> buckets(static_cast<size_t>(groups));
    std::vector<double> loads(static_cast<size_t>(groups), 0.0);
    std::vector<size_t> order(algos.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return estimatedCostPerByte(algos[a]) > estimatedCostPerByte(algos[b]);
    });
    for (size_t index : order) {
        size_t best = 0;
        for (size_t g = 1; g < buckets.size(); ++g) {
            if (loads[g] < loads[best]) best = g;
        }
        buckets[best].push_back(index);
        loads[best] += estimatedCostPerByte(algos[index]);
    }
    return buckets;
}

// ============================================================ 单文件哈希 ====
SingleHashResult hashSingleFile(const std::wstring& path, const std::vector<Algo>& algos,
                               size_t bufferSize, bool overlap, JobControl* control,
                               bool reportBytes, int algorithmGroups) {
    SingleHashResult result;
    const auto start = Clock::now();
    if (bufferSize < 4096) bufferSize = 4096;

    WinFile file;
    std::wstring err;
    if (!file.open(path, err)) {
        result.error = err;
        return result;
    }

    std::vector<Hasher> hashers;
    hashers.reserve(algos.size());
    for (Algo a : algos) hashers.emplace_back(a);

    auto consume = [&](const uint8_t* data, size_t len) {
        for (Hasher& h : hashers) h.update(data, len);
        if (control && reportBytes) control->addBytes(len);
    };

    // ---------------------------------------------------- 并行消费同一块 --
    // 只有"算法数 ≥ 2、分了多组、且块足够大"时才值得并行（同步开销摊销得开）
    int groups = algorithmGroups;
    if (groups > static_cast<int>(hashers.size())) groups = static_cast<int>(hashers.size());
    if (groups < 1) groups = 1;
    const bool parallelGroups = groups > 1 && bufferSize >= (256u << 10);

    const std::vector<std::vector<size_t>> buckets =
        parallelGroups ? partitionAlgorithms(algos, groups) : std::vector<std::vector<size_t>>();

    const bool useOverlap = overlap && bufferSize >= (256u << 10);
    if (useOverlap) {
        // 缓冲区较大时减少流水线槽位，把「每线程峰值内存」限制在合理范围：
        //   4 MB  → 4 槽 (16 MB)
        //   16 MB → 2 槽 (32 MB)
        //   >32 MB → 关闭流水线，退化为单缓冲顺序读取
        const size_t perThreadBudget = 32u << 20;
        size_t slots = 4;
        while (slots > 2 && bufferSize * slots > perThreadBudget) --slots;
        if (bufferSize * slots > (64u << 20)) {
            std::vector<uint8_t> buffer(bufferSize);
            for (;;) {
                if (control && !control->checkpoint()) {
                    result.cancelled = true;
                    result.error = L"已取消";
                    return result;
                }
                size_t got = 0;
                if (!file.read(buffer.data(), buffer.size(), got, err)) {
                    result.error = err;
                    return result;
                }
                if (got == 0) break;
                consume(buffer.data(), got);
            }
        } else if (parallelGroups) {
            // 读线程（OverlapReader）+ G 个算法线程，用栅栏同步
            struct Shared {
                const uint8_t* data = nullptr;
                size_t         len = 0;
                bool           finished = false;
                bool           failed = false;
            } shared;

            Barrier barrier(static_cast<size_t>(groups) + 1);   // +1 = 主线程（发布/回收）
            std::vector<std::thread> workers;
            workers.reserve(static_cast<size_t>(groups));
            for (int g = 0; g < groups; ++g) {
                workers.emplace_back([&, g] {
                    for (;;) {
                        barrier.arriveAndWait();          // 阶段 1：等这一块发布
                        const bool done = shared.finished;
                        if (!done) {
                            try {
                                for (size_t index : buckets[static_cast<size_t>(g)]) {
                                    hashers[index].update(shared.data, shared.len);
                                }
                            } catch (...) {
                                shared.failed = true;
                            }
                        }
                        barrier.arriveAndWait();          // 阶段 2：报告算完
                        if (done) break;
                    }
                });
            }

            OverlapReader reader(file, bufferSize, slots);
            reader.start();
            bool stopped = false;
            for (;;) {
                if (control && !control->checkpoint()) {
                    result.cancelled = true;
                    result.error = L"已取消";
                    stopped = true;
                    break;
                }
                const uint8_t* data = nullptr;
                size_t len = 0;
                std::wstring readError;
                if (!reader.next(data, len, readError)) {
                    if (!readError.empty()) {
                        result.error = readError;
                        stopped = true;
                    }
                    break;
                }
                shared.data = data;
                shared.len = len;
                barrier.arriveAndWait();                  // 发布
                if (reportBytes && control) control->addBytes(len);
                barrier.arriveAndWait();                  // 等所有算法算完
                reader.recycle();
                if (shared.failed) {
                    result.error = L"哈希计算线程异常";
                    stopped = true;
                    break;
                }
            }
            // 收尾：让工作线程看到 finished 后按同样的两阶段退出，避免死锁
            shared.finished = true;
            shared.data = nullptr;
            shared.len = 0;
            barrier.arriveAndWait();
            barrier.arriveAndWait();
            for (std::thread& worker : workers) {
                if (worker.joinable()) worker.join();
            }
            if (stopped) return result;
        } else {
            OverlapReader reader(file, bufferSize, slots);
            reader.start();
            for (;;) {
                if (control && !control->checkpoint()) {
                    result.cancelled = true;
                    result.error = L"已取消";
                    return result;
                }
                const uint8_t* data = nullptr;
                size_t len = 0;
                std::wstring readError;
                if (!reader.next(data, len, readError)) {
                    if (!readError.empty()) {
                        result.error = readError;
                        return result;
                    }
                    break;
                }
                consume(data, len);
                reader.recycle();
            }
        }
    } else if (parallelGroups) {
        // 无流水线：直接读一块、并行算一块
        std::vector<uint8_t> buffer(bufferSize);
        struct Shared {
            const uint8_t* data = nullptr;
            size_t         len = 0;
            bool           finished = false;
            bool           failed = false;
        } shared;
        Barrier barrier(static_cast<size_t>(groups) + 1);
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(groups));
        for (int g = 0; g < groups; ++g) {
            workers.emplace_back([&, g] {
                for (;;) {
                    barrier.arriveAndWait();
                    const bool done = shared.finished;
                    if (!done) {
                        try {
                            for (size_t index : buckets[static_cast<size_t>(g)]) {
                                hashers[index].update(shared.data, shared.len);
                            }
                        } catch (...) {
                            shared.failed = true;
                        }
                    }
                    barrier.arriveAndWait();
                    if (done) break;
                }
            });
        }
        bool stopped = false;
        for (;;) {
            if (control && !control->checkpoint()) {
                result.cancelled = true;
                result.error = L"已取消";
                stopped = true;
                break;
            }
            size_t got = 0;
            if (!file.read(buffer.data(), buffer.size(), got, err)) {
                result.error = err;
                stopped = true;
                break;
            }
            if (got == 0) break;
            shared.data = buffer.data();
            shared.len = got;
            barrier.arriveAndWait();
            if (reportBytes && control) control->addBytes(got);
            barrier.arriveAndWait();
            if (shared.failed) {
                result.error = L"哈希计算线程异常";
                stopped = true;
                break;
            }
        }
        shared.finished = true;
        shared.data = nullptr;
        shared.len = 0;
        barrier.arriveAndWait();
        barrier.arriveAndWait();
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        if (stopped) return result;
    } else {
        std::vector<uint8_t> buffer(bufferSize);
        for (;;) {
            if (control && !control->checkpoint()) {
                result.cancelled = true;
                result.error = L"已取消";
                return result;
            }
            size_t got = 0;
            if (!file.read(buffer.data(), buffer.size(), got, err)) {
                result.error = err;
                return result;
            }
            if (got == 0) break;
            consume(buffer.data(), got);
        }
    }

    result.bytes = file.size();
    result.digests.reserve(hashers.size());
    for (Hasher& h : hashers) result.digests.push_back(h.hexDigest());
    result.ok = true;
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return result;
}

// ============================================================ 批量哈希 ====
HashJobResult runHashJob(const std::vector<std::wstring>& targets, const HashJobOptions& options,
                         JobControl& control) {
    HashJobResult result;

    // 先进入“扫描”阶段，让大规模目录树也有进度反馈
    control.beginRun(L"扫描文件");
    ScanResult scan = scanTargets(targets, options.scan, &control);
    result.roots = scan.roots;
    result.errors = scan.errors;
    result.totalBytes = scan.totalBytes;

    if (scan.cancelled) {
        control.endRun();
        result.cancelled = true;
        result.elapsed = control.elapsedSeconds();
        return result;
    }

    if (options.sortByName) {
        std::stable_sort(scan.files.begin(), scan.files.end(),
                         [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
    }
    result.records.resize(scan.files.size());

    const int threads = effectiveThreadCount(options.threads);
    control.setUnit(L"计算哈希");
    control.setTotals(scan.files.size(), scan.totalBytes);

    if (scan.files.empty()) {
        control.endRun();
        result.elapsed = control.elapsedSeconds();
        return result;
    }

    // 调度顺序与展示顺序解耦：展示仍按（名字排序后的）扫描顺序，
    // 但真正派发时按文件大小从大到小 —— 大文件先开工，尾巴上不会剩一个巨型文件
    // 让其它核心干等（经典的 LPT 调度），多文件场景下能明显缩短总墙钟时间。
    std::vector<size_t> dispatch(scan.files.size());
    for (size_t i = 0; i < dispatch.size(); ++i) dispatch[i] = i;
    if (!options.sortByName) {
        std::stable_sort(dispatch.begin(), dispatch.end(), [&](size_t a, size_t b) {
            return scan.files[a].size > scan.files[b].size;
        });
    }

    // 每个文件可用多少组算法线程：总核数要按"同时在跑的文件数"摊分，避免超订
    const size_t parallelFiles = std::min<size_t>(scan.files.size(), static_cast<size_t>(threads));
    int groupsPerFile = 1;
    if (options.algos.size() >= 2 && parallelFiles >= 1) {
        groupsPerFile = static_cast<int>(
            std::max<size_t>(1, static_cast<size_t>(threads) / std::max<size_t>(1, parallelFiles)));
        groupsPerFile = std::min<int>(groupsPerFile, static_cast<int>(options.algos.size()));
        // 单文件太小就没必要拆（同步开销比计算还大）
        if (!scan.files.empty()) {
            const uint64_t largest = scan.files[dispatch.front()].size;
            if (largest < (4u << 20)) groupsPerFile = 1;
        }
    }

    parallelFor(scan.files.size(), threads, [&](size_t jobIndex) {
        const size_t index = dispatch[jobIndex];
        const FileEntry& entry = scan.files[index];
        HashRecord& record = result.records[index];
        record.path = entry.path;
        record.rel = (entry.rootIndex >= 0 && entry.rootIndex < static_cast<int>(scan.roots.size()))
                         ? relativeTo(scan.roots[entry.rootIndex], entry.path)
                         : baseName(entry.path);
        record.size = entry.size;
        record.mtime = entry.mtime;

        control.setCurrentFile(entry.path, entry.size);
        // 小文件不拆分（拆分收益抵不过线程创建与同步开销）
        const int groups = entry.size >= (4u << 20) ? groupsPerFile : 1;
        const SingleHashResult single = hashSingleFile(entry.path, options.algos,
                                                       options.bufferSize, options.overlap,
                                                       &control, true, groups);
        record.digests = single.digests;
        record.seconds = single.seconds;
        record.ok = single.ok;
        record.cancelled = single.cancelled;
        if (!single.ok) record.error = single.error;
        // 因取消而未处理的文件不算“失败”，单独统计
        control.fileFinished(single.ok || single.cancelled);
    });

    control.endRun();
    result.elapsed = control.elapsedSeconds();
    result.cancelled = control.cancelled();
    for (const HashRecord& record : result.records) {
        if (record.ok) {
            result.okFiles++;
        } else if (record.cancelled) {
            result.cancelledFiles++;
        } else {
            result.failedFiles++;
        }
    }
    return result;
}

// ============================================================ 文件比较 ====
CompareResult compareFiles(const std::wstring& pathA, const std::wstring& pathB,
                           const CompareOptions& options, JobControl& control) {
    CompareResult result;
    const auto start = Clock::now();

    FileInfo infoA, infoB;
    std::wstring err;
    if (!getFileInfo(pathA, infoA, err)) {
        result.status = 2;
        result.error = L"无法读取「" + pathA + L"」：" + err;
        return result;
    }
    if (!getFileInfo(pathB, infoB, err)) {
        result.status = 2;
        result.error = L"无法读取「" + pathB + L"」：" + err;
        return result;
    }
    if (infoA.isDir || infoB.isDir) {
        result.status = 2;
        result.error = L"字节级比较仅支持文件，不支持文件夹";
        return result;
    }

    result.sizeA = infoA.size;
    result.sizeB = infoB.size;
    result.sizeEqual = (infoA.size == infoB.size);

    const uint64_t compareBytes = std::min(infoA.size, infoB.size);
    // 进度分母与实际累加的字节数保持同一量纲：
    //   摘要阶段每个文件读一遍（sizeA + sizeB），逐字节阶段两个文件各读 compareBytes
    uint64_t totalBytes = compareBytes * 2;
    if (options.withHash) totalBytes += infoA.size + infoB.size;

    control.beginRun(L"文件比较");
    control.setTotals(2, totalBytes);
    control.setCurrentFile(pathA + L"  ⟷  " + pathB, totalBytes);

    if (options.withHash) {
        const SingleHashResult hashA =
            hashSingleFile(pathA, options.algos, options.bufferSize, true, &control, true);
        if (!hashA.ok) {
            result.status = hashA.cancelled ? 3 : 2;
            result.error = hashA.error;
            control.endRun();
            return result;
        }
        const SingleHashResult hashB =
            hashSingleFile(pathB, options.algos, options.bufferSize, true, &control, true);
        if (!hashB.ok) {
            result.status = hashB.cancelled ? 3 : 2;
            result.error = hashB.error;
            control.endRun();
            return result;
        }
        result.digestsA = hashA.digests;
        result.digestsB = hashB.digests;
    }

    if (options.quick) {
        result.contentEqual = result.sizeEqual;
        result.compared = 0;
    } else {
        WinFile fileA, fileB;
        if (!fileA.open(pathA, err)) {
            result.status = 2;
            result.error = L"无法打开「" + pathA + L"」：" + err;
            control.endRun();
            return result;
        }
        if (!fileB.open(pathB, err)) {
            result.status = 2;
            result.error = L"无法打开「" + pathB + L"」：" + err;
            control.endRun();
            return result;
        }
        const size_t bufferSize = std::max<size_t>(options.bufferSize, 4096);
        std::vector<uint8_t> bufferA(bufferSize);
        std::vector<uint8_t> bufferB(bufferSize);
        uint64_t offset = 0;
        bool diffFound = false;
        for (;;) {
            if (!control.checkpoint()) {
                result.status = 3;
                result.error = L"已取消";
                control.endRun();
                return result;
            }
            size_t gotA = 0, gotB = 0;
            if (!fileA.read(bufferA.data(), bufferSize, gotA, err)) {
                result.status = 2;
                result.error = L"读取失败（" + pathA + L"）：" + err;
                control.endRun();
                return result;
            }
            if (!fileB.read(bufferB.data(), bufferSize, gotB, err)) {
                result.status = 2;
                result.error = L"读取失败（" + pathB + L"）：" + err;
                control.endRun();
                return result;
            }
            if (gotA == 0 && gotB == 0) break;
            const size_t common = std::min(gotA, gotB);
            if (common > 0 && std::memcmp(bufferA.data(), bufferB.data(), common) != 0) {
                size_t index = 0;
                while (index < common && bufferA[index] == bufferB[index]) ++index;
                result.firstDiff = offset + index;
                diffFound = true;
                const size_t window = std::max<size_t>(options.windowSize, 1);
                const size_t available = std::min(window, common - index);
                result.windowOffset = offset + index;
                result.windowA.assign(bufferA.begin() + static_cast<ptrdiff_t>(index),
                                      bufferA.begin() + static_cast<ptrdiff_t>(index + available));
                result.windowB.assign(bufferB.begin() + static_cast<ptrdiff_t>(index),
                                      bufferB.begin() + static_cast<ptrdiff_t>(index + available));
            }
            result.compared += common;
            offset += common;
            control.addBytes(static_cast<uint64_t>(gotA) + gotB);
            if (diffFound || gotA == 0 || gotB == 0) break;
        }
        result.contentEqual = !diffFound && result.sizeEqual;
    }

    control.endRun();
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    if (result.status == 0) result.status = result.contentEqual ? 0 : 1;
    return result;
}

// ============================================================ 基准测试 ====
std::vector<BenchRow> benchmarkMemory(const void* data, size_t size, const std::vector<Algo>& algos,
                                      double minSeconds, JobControl& control) {
    std::vector<BenchRow> rows;
    if (size == 0) return rows;
    if (minSeconds < 0.05) minSeconds = 0.05;

    control.beginRun(L"算法速度对比");
    control.setTotals(algos.size(), algos.size());

    for (Algo algo : algos) {
        if (!control.checkpoint()) break;
        control.setCurrentFile(utf8ToWide(algoInfo(algo).display), 1);

        Hasher hasher(algo);
        double total = 0;
        size_t iterations = 0;
        const auto start = Clock::now();
        do {
            hasher.reset();
            hasher.update(data, size);
            (void)hasher.hexDigest();
            ++iterations;
            total = std::chrono::duration<double>(Clock::now() - start).count();
            if (!control.checkpoint()) break;
        } while (total < minSeconds && iterations < 200000);

        BenchRow row;
        row.algo = algo;
        row.iterations = iterations;
        row.seconds = iterations > 0 ? total / static_cast<double>(iterations) : 0;
        row.mbPerSec = row.seconds > 0 ? (static_cast<double>(size) / (1024.0 * 1024.0)) / row.seconds
                                       : 0;
        rows.push_back(row);
        control.addBytes(1);
        control.fileFinished(true);
    }

    control.endRun();
    return rows;
}

// ============================================================ 工具函数 ====
int effectiveThreadCount(int requested) {
    if (requested > 0) return std::min(requested, 128);
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) hardware = 4;
    // 默认用满硬件线程（上限 32）：哈希是纯计算密集，多开线程能直接换来吞吐；
    // 机械硬盘上可以把 -t 调小（并发随机读会互相拖慢），SSD/NVMe 则越大越好。
    return static_cast<int>(std::min<unsigned>(hardware, 32));
}

std::string formatByteSize(uint64_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, kUnits[unit]);
    }
    return buffer;
}

std::string formatCount(uint64_t value) {
    const std::string digits = std::to_string(value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const size_t firstGroup = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (i - firstGroup) % 3 == 0 && i >= firstGroup) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

std::string formatDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    const uint64_t total = static_cast<uint64_t>(seconds);
    const uint64_t hours = total / 3600;
    const uint64_t minutes = (total % 3600) / 60;
    const uint64_t secs = total % 60;
    const double frac = seconds - static_cast<double>(total);
    char buffer[64];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu:%02llu",
                      static_cast<unsigned long long>(hours),
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(secs));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu.%01d",
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(secs), static_cast<int>(frac * 10));
    }
    return buffer;
}

std::string formatSpeed(double bytesPerSecond) {
    static const char* kUnits[] = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
    double value = bytesPerSecond;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f %s", value, kUnits[unit]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, kUnits[unit]);
    }
    return buffer;
}

std::string formatUnixTime(int64_t unixSeconds) {
    if (unixSeconds <= 0) return "-";
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
    if (localtime_s(&local, &t) != 0) return "-";
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return buffer;
}

std::string isoTimeUtc(int64_t unixSeconds) {
    if (unixSeconds <= 0) return std::string();
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm utc{};
    if (gmtime_s(&utc, &t) != 0) return std::string();
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string isoNowUtc() {
    const std::time_t t = std::time(nullptr);
    std::tm utc{};
    if (gmtime_s(&utc, &t) != 0) return std::string();
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string trimText(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

std::wstring stripQuotes(const std::wstring& text) {
    std::wstring out = text;
    while (!out.empty() && (out.front() == L' ' || out.front() == L'\t')) out.erase(out.begin());
    while (!out.empty() && (out.back() == L' ' || out.back() == L'\t')) out.pop_back();
    if (out.size() >= 2 && out.front() == L'"' && out.back() == L'"') {
        out = out.substr(1, out.size() - 2);
    }
    return out;
}

size_t textDisplayWidth(const std::string& utf8Text) {
    size_t width = 0;
    for (size_t i = 0; i < utf8Text.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8Text[i]);
        uint32_t code = 0;
        int length = 1;
        if (c < 0x80) {
            code = c;
        } else if ((c >> 5) == 0x6) {
            code = c & 0x1Fu;
            length = 2;
        } else if ((c >> 4) == 0xE) {
            code = c & 0x0Fu;
            length = 3;
        } else if ((c >> 3) == 0x1E) {
            code = c & 0x07u;
            length = 4;
        } else {
            ++i;
            continue;
        }
        for (int k = 1; k < length && i + static_cast<size_t>(k) < utf8Text.size(); ++k) {
            code = (code << 6) |
                   (static_cast<unsigned char>(utf8Text[i + static_cast<size_t>(k)]) & 0x3Fu);
        }
        i += static_cast<size_t>(length);
        const bool wide =
            (code >= 0x1100 && code <= 0x115F) || code == 0x2329 || code == 0x232A ||
            (code >= 0x2E80 && code <= 0xA4CF && code != 0x303F) ||
            (code >= 0xAC00 && code <= 0xD7A3) || (code >= 0xF900 && code <= 0xFAFF) ||
            (code >= 0xFE30 && code <= 0xFE6F) || (code >= 0xFF00 && code <= 0xFF60) ||
            (code >= 0xFFE0 && code <= 0xFFE6) || (code >= 0x1F300 && code <= 0x1FAFF) ||
            (code >= 0x20000 && code <= 0x3FFFD);
        width += wide ? 2 : 1;
    }
    return width;
}

std::string padToWidth(const std::string& utf8Text, size_t width) {
    std::string out = utf8Text;
    size_t current = textDisplayWidth(out);
    while (current < width) {
        out.push_back(' ');
        ++current;
    }
    return out;
}

}  // namespace ehsc
