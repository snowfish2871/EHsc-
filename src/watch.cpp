// ============================================================================
//  EHsc · watch.cpp
//  文件访问监听实现：系统句柄表采样 + 目录变更通知，两条通道互补。
//
//  ⚠ 能力边界（程序会在报告里如实说明，不夸大）：
//    * 句柄采样能回答"谁正打开着这个文件、以读还是写的方式、持续了多久"，
//      但采样间隔内完成并立即关闭的访问可能被漏掉；
//    * 非管理员只能打开当前用户可访问的进程，系统进程/其他用户/受保护进程的句柄
//      无法解析（枚举本身可以，复制句柄需要权限）；
//    * Windows 的目录变更通知不提供发起进程，因此这类事件只能记录"何时被改"。
// ============================================================================
#include "watch.h"
#include "core.h"
#include "hashes.h"
#include "win32_utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace ehsc {
namespace {

// ---------------------------------------------------------------- NT API --
constexpr ULONG kSystemExtendedHandleInformation = 64;

struct SystemHandleEntryEx {
    void*     object;
    ULONG_PTR uniqueProcessId;
    ULONG_PTR handleValue;
    ULONG     grantedAccess;
    USHORT    creatorBackTraceIndex;
    USHORT    objectTypeIndex;
    ULONG     handleAttributes;
    ULONG     reserved;
};

struct SystemHandleInformationEx {
    ULONG_PTR numberOfHandles;
    ULONG_PTR reserved;
    SystemHandleEntryEx handles[1];
};

// 注意：OBJECT_TYPE_INFORMATION 的第一个成员是 UNICODE_STRING TypeName，
// TypeIndex 在结构体靠后（x64 偏移 90）。拿开头两字节当索引是常见错误 ——
// 那其实是字符串长度，会误把 Process 句柄当成 File 句柄。
struct ObjectTypeInformationFull {
    UNICODE_STRING typeName;
    ULONG totalNumberOfObjects;
    ULONG totalNumberOfHandles;
    ULONG totalPagedPoolUsage;
    ULONG totalNonPagedPoolUsage;
    ULONG totalNamePoolUsage;
    ULONG totalHandleTableUsage;
    ULONG highWaterNumberOfObjects;
    ULONG highWaterNumberOfHandles;
    ULONG highWaterPagedPoolUsage;
    ULONG highWaterNonPagedPoolUsage;
    ULONG highWaterNamePoolUsage;
    ULONG highWaterHandleTableUsage;
    ULONG invalidAttributes;
    GENERIC_MAPPING genericMapping;
    ULONG validAccessMask;
    BOOLEAN securityRequired;
    BOOLEAN maintainHandleCount;
    UCHAR typeIndex;
    CHAR  reservedByte;
    ULONG poolType;
    ULONG defaultPagedPoolCharge;
    ULONG defaultNonPagedPoolCharge;
};

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryObjectFn = NTSTATUS(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

NtQuerySystemInformationFn ntQuerySystemInformation() {
    static NtQuerySystemInformationFn fn = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtQuerySystemInformationFn>(
                           GetProcAddress(ntdll, "NtQuerySystemInformation"))
                     : nullptr;
    }();
    return fn;
}

NtQueryObjectFn ntQueryObject() {
    static NtQueryObjectFn fn = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtQueryObjectFn>(GetProcAddress(ntdll, "NtQueryObject"))
                     : nullptr;
    }();
    return fn;
}

// ---------------------------------------------------------------- 小工具 --
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring toLower(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

std::wstring baseNameLower(const std::wstring& path) { return toLower(baseName(path)); }

// "C:" -> "\Device\HarddiskVolume3" 的映射，用于把设备路径还原成盘符路径
const std::vector<std::pair<std::wstring, std::wstring>>& dosDeviceMap() {
    static const std::vector<std::pair<std::wstring, std::wstring>> map = [] {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
            wchar_t drive[3] = {letter, L':', 0};
            wchar_t targets[2048] = {0};
            if (QueryDosDeviceW(drive, targets, 2048) == 0) continue;
            for (const wchar_t* p = targets; *p; p += wcslen(p) + 1) {
                out.emplace_back(std::wstring(p), std::wstring(drive));
            }
        }
        return out;
    }();
    return map;
}

std::wstring devicePathToDos(const std::wstring& devicePath) {
    for (const auto& kv : dosDeviceMap()) {
        if (devicePath.size() >= kv.first.size() &&
            _wcsnicmp(devicePath.c_str(), kv.first.c_str(), kv.first.size()) == 0) {
            return kv.second + devicePath.substr(kv.first.size());
        }
    }
    return devicePath;
}

std::wstring stripExtendedPrefix(std::wstring path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + path.substr(8);
    if (path.rfind(L"\\\\?\\", 0) == 0) return path.substr(4);
    return path;
}

std::string formatTimeMs(int64_t unixMs) {
    if (unixMs <= 0) return "-";
    const std::time_t seconds = static_cast<std::time_t>(unixMs / 1000);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) return "-";
    char buffer[40];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03d", buffer, static_cast<int>(unixMs % 1000));
    return out;
}

std::string formatTimeMsShort(int64_t unixMs) {
    if (unixMs <= 0) return "-";
    const std::time_t seconds = static_cast<std::time_t>(unixMs / 1000);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) return "-";
    char buffer[40];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03d", buffer, static_cast<int>(unixMs % 1000));
    return out;
}



WatchOp classifyAccess(uint32_t mask) {
    constexpr uint32_t kFileReadData = 0x0001;
    constexpr uint32_t kFileWriteData = 0x0002;
    constexpr uint32_t kFileAppendData = 0x0004;
    constexpr uint32_t kFileWriteEa = 0x0010;
    constexpr uint32_t kFileExecute = 0x0020;
    constexpr uint32_t kDelete = 0x00010000;
    constexpr uint32_t kGenericRead = 0x80000000;
    constexpr uint32_t kGenericWrite = 0x40000000;

    const bool read = (mask & (kFileReadData | kFileExecute | kGenericRead)) != 0;
    const bool write = (mask & (kFileWriteData | kFileAppendData | kFileWriteEa | kGenericWrite)) != 0;
    if (read && write) return WatchOp::ReadWrite;
    if (write) return WatchOp::Write;
    if (read) return WatchOp::Read;
    if ((mask & kDelete) != 0) return WatchOp::Delete;
    return WatchOp::MetaOnly;
}

int opStrength(WatchOp op) {
    switch (op) {
        case WatchOp::ReadWrite: return 4;
        case WatchOp::Write: return 3;
        case WatchOp::Read: return 2;
        case WatchOp::Delete: return 1;
        case WatchOp::MetaOnly: return 0;
    }
    return 0;
}

ChangeKind classifyChange(DWORD action) {
    switch (action) {
        case FILE_ACTION_ADDED: return ChangeKind::Added;
        case FILE_ACTION_REMOVED: return ChangeKind::Removed;
        case FILE_ACTION_MODIFIED: return ChangeKind::Modified;
        case FILE_ACTION_RENAMED_OLD_NAME: return ChangeKind::RenamedOld;
        case FILE_ACTION_RENAMED_NEW_NAME: return ChangeKind::RenamedNew;
        default: return ChangeKind::AttrChanged;
    }
}

}  // namespace

// ============================================================ 枚举名称 ====
const char* watchOpName(WatchOp op) {
    switch (op) {
        case WatchOp::Read: return "read";
        case WatchOp::Write: return "write";
        case WatchOp::ReadWrite: return "readwrite";
        case WatchOp::Delete: return "delete";
        case WatchOp::MetaOnly: return "meta";
    }
    return "unknown";
}

const char* watchOpDisplay(WatchOp op) {
    switch (op) {
        case WatchOp::Read: return "读取";
        case WatchOp::Write: return "写入";
        case WatchOp::ReadWrite: return "读写";
        case WatchOp::Delete: return "刪除";
        case WatchOp::MetaOnly: return "查询属性";
    }
    return "未知";
}

const char* changeKindName(ChangeKind kind) {
    switch (kind) {
        case ChangeKind::Added: return "added";
        case ChangeKind::Removed: return "removed";
        case ChangeKind::Modified: return "modified";
        case ChangeKind::RenamedOld: return "renamed_from";
        case ChangeKind::RenamedNew: return "renamed_to";
        case ChangeKind::AttrChanged: return "attributes";
    }
    return "unknown";
}

const char* changeKindDisplay(ChangeKind kind) {
    switch (kind) {
        case ChangeKind::Added: return "新建";
        case ChangeKind::Removed: return "删除";
        case ChangeKind::Modified: return "内容写入";
        case ChangeKind::RenamedOld: return "改名(原)";
        case ChangeKind::RenamedNew: return "改名(新)";
        case ChangeKind::AttrChanged: return "属性变化";
    }
    return "未知";
}

// ============================================================ 监视器 ====
FileActivityMonitor::FileActivityMonitor(std::vector<std::wstring> targets,
                                         const WatchOptions& options)
    : options_(options) {
    for (const std::wstring& raw : targets) {
        const std::wstring full = fullPath(stripQuotes(raw));
        if (full.empty()) continue;
        TargetInfo info;
        info.path = full;
        info.isDir = isDirectory(full);
        info.baseName = baseNameLower(full);
        targets_.push_back(std::move(info));
        normalized_.push_back(full);
    }
}

FileActivityMonitor::~FileActivityMonitor() { stop(); }

bool FileActivityMonitor::start(std::wstring& err) {
    if (targets_.empty()) {
        err = L"没有可监视的目标";
        return false;
    }
    cancelEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (cancelEvent_ == nullptr) {
        err = L"无法创建取消事件对象";
        return false;
    }

    stats_.elevated = isProcessElevated();   // 统一走 win32_utils 的实现
    stats_.handleScanAvailable = ntQuerySystemInformation() != nullptr;
    if (!stats_.handleScanAvailable) options_.pollHandles = false;
    stats_.note = stats_.elevated
                      ? L"管理员模式：可解析系统与其他用户进程的句柄，覆盖面最全"
                      : L"非管理员：只能解析当前用户可访问的进程；系统进程、其他用户的句柄"
                        L"无法归因（以管理员身份运行可获得完整覆盖面）";

    // 收集需要挂目录变更通知的目录（文件目标监听其所在目录），并去重
    if (options_.notifyChanges) {
        for (const TargetInfo& target : targets_) {
            const std::wstring dir = target.isDir ? target.path : parentDir(target.path);
            if (dir.empty() || !isDirectory(dir)) continue;
            bool duplicate = false;
            for (const std::wstring& existing : notifyDirs_) {
                if (toLower(existing) == toLower(dir)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) notifyDirs_.push_back(dir);
        }
    }

    stopFlag_.store(false);
    running_.store(true);

    // 活动追踪：先给每个已存在的目标文件拍一张"起点快照"，
    // 之后任何变化都能和它对比，回答"这个文件被干了什么"。
    if (options_.trackActivity) {
        for (const TargetInfo& target : targets_) {
            if (target.isDir) continue;
            FileSnapshot snap;
            if (takeSnapshot(target.path, options_.digestContent, snap)) {
                snapshots_[toLower(target.path)] = snap;
            }
            trackedPaths_.push_back(target.path);
        }
        if (options_.notifyChanges || true) {
            // 目录目标：把目录里已有的文件也纳入"定时快照对比"的范围。
            // 原因：ReadDirectoryChangesW 只报告"目录项变化"，对已经打开的句柄
            // 继续写入/截断往往不产生新通知（实测 3 次操作只来 1 条事件），
            // 所以必须自己按间隔去比对文件本身的大小/时间/摘要。
            for (const TargetInfo& target : targets_) {
                if (!target.isDir) continue;
                collectTrackedFiles(target.path, target.path.size() + 1);
            }
        }
        activityThread_ = std::thread([this] { activityLoop(); });
    }

    if (options_.pollHandles) {
        handleThread_ = std::thread([this] { handleLoop(); });
    }
    for (size_t i = 0; i < notifyDirs_.size(); ++i) {
        notifyThreads_.emplace_back([this, i] { notifyLoop(i); });
    }
    return true;
}

void FileActivityMonitor::stop() {
    if (!running_.exchange(false)) {
        if (cancelEvent_) {
            CloseHandle(static_cast<HANDLE>(cancelEvent_));
            cancelEvent_ = nullptr;
        }
        return;
    }
    stopFlag_.store(true);
    if (cancelEvent_) SetEvent(static_cast<HANDLE>(cancelEvent_));
    activityRequested_.store(true);
    if (activityThread_.joinable()) activityThread_.join();
    if (handleThread_.joinable()) handleThread_.join();
    for (std::thread& thread : notifyThreads_) {
        if (thread.joinable()) thread.join();
    }
    notifyThreads_.clear();
    if (cancelEvent_) {
        CloseHandle(static_cast<HANDLE>(cancelEvent_));
        cancelEvent_ = nullptr;
    }
}

std::wstring FileActivityMonitor::resolveProcess(uint32_t pid, std::wstring* fullPathOut) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = processNameCache_.find(pid);
        if (it != processNameCache_.end()) {
            if (fullPathOut) {
                auto pit = processPathCache_.find(pid);
                *fullPathOut = pit != processPathCache_.end() ? pit->second : std::wstring();
            }
            return it->second;
        }
    }
    std::wstring full;
    std::wstring name;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        wchar_t buffer[2048] = {0};
        DWORD size = 2048;
        if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
            full.assign(buffer, size);
            const std::wstring base = baseName(full);
            name = base.empty() ? full : base;
        }
        CloseHandle(process);
    }
    if (name.empty()) name = L"(无法访问)";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        processNameCache_[pid] = name;
        processPathCache_[pid] = full;
    }
    if (fullPathOut) *fullPathOut = full;
    return name;
}

bool FileActivityMonitor::matchesFilter(const std::wstring& lowerPath) const {
    if (options_.filters.empty()) return true;
    const std::wstring name = baseName(lowerPath);
    for (const std::wstring& pattern : options_.filters) {
        if (wildcardMatch(pattern, name)) return true;
    }
    return false;
}

bool FileActivityMonitor::matchesTarget(const std::wstring& lowerPath) const {
    for (const TargetInfo& target : targets_) {
        const std::wstring lowerTarget = toLower(target.path);
        if (!target.isDir) {
            if (lowerPath == lowerTarget) return matchesFilter(lowerPath);
            continue;
        }
        if (lowerPath.size() <= lowerTarget.size()) continue;
        if (_wcsnicmp(lowerPath.c_str(), lowerTarget.c_str(), lowerTarget.size()) != 0) continue;
        const wchar_t sep = lowerPath[lowerTarget.size()];
        if (sep != L'\\' && sep != L'/') continue;
        if (!options_.recursive) {
            // 非递归：只接受目录的直接子项
            if (lowerPath.find_first_of(L"\\/", lowerTarget.size() + 1) != std::wstring::npos)
                continue;
        }
        if (!matchesFilter(lowerPath)) continue;
        return true;
    }
    return false;
}

void FileActivityMonitor::mergeAccess(const AccessRecord& record) {
    // key: pid + 文件路径（小写）
    std::string key = std::to_string(record.pid) + "|";
    const std::wstring lowerPath = toLower(record.filePath);
    key.append(wideToUtf8(lowerPath));

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = accessIndex_.find(key);
    if (it == accessIndex_.end()) {
        if (accesses_.size() >= options_.maxRecords) return;
        accessIndex_.emplace(key, accesses_.size());
        accesses_.push_back(record);
        pendingAccess_.push_back(record);
        return;
    }
    AccessRecord& existing = accesses_[it->second];
    existing.lastSeenMs = record.lastSeenMs;
    existing.samples++;
    existing.stillOpen = true;
    existing.accessMask = record.accessMask;
    if (opStrength(record.op) > opStrength(existing.op)) existing.op = record.op;
    pendingAccess_.push_back(existing);
}

void FileActivityMonitor::appendChanges(std::vector<ChangeRecord>&& items) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (ChangeRecord& item : items) {
            if (changes_.size() >= options_.maxRecords) break;
            changes_.push_back(item);
            pendingChange_.push_back(item);
            if (options_.trackActivity) pendingChangeForActivity_.push_back(item);
        }
        if (items.empty()) return;
    }
    stats_.changeEvents = changes_.size();
    // 变更事件一到就让活动线程立刻补一次快照对比（"变了" → "改了什么"）
    if (options_.trackActivity) activityRequested_.store(true);
}

void FileActivityMonitor::drain(std::vector<AccessRecord>& newAccesses,
                                std::vector<ChangeRecord>& newChanges,
                                std::vector<ActivityRecord>& newActivities) {
    std::lock_guard<std::mutex> lock(mutex_);
    newAccesses.swap(pendingAccess_);
    newChanges.swap(pendingChange_);
    newActivities.swap(pendingActivity_);
    pendingAccess_.clear();
    pendingChange_.clear();
    pendingActivity_.clear();
}

// ============================================================ 活动追踪 ====
// 目标：把文件系统给的"变了"翻译成人能读的"被干了什么"。
// 做法：每次变化都对文件拍一张快照（大小 / 修改时间 / 属性 / 可选的内容摘要），
//       和上一次快照对比得出活动类型；再查一次句柄表，看当时是谁拿着它 —— 那就是"谁干的"。
bool FileActivityMonitor::takeSnapshot(const std::wstring& path, bool withDigest,
                                       FileSnapshot& out) const {
    out = FileSnapshot{};
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;   // 不存在或没有权限
    }
    out.exists = true;
    out.attributes = data.dwFileAttributes;
    out.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    ULARGE_INTEGER ticks{};
    ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
    ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
    out.mtime = static_cast<int64_t>(ticks.QuadPart / 10000000ull) - 11644473600ll;

    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        out.digestValid = false;
        return true;
    }

    if (!withDigest || out.size > options_.digestMaxBytes) return true;
    // 内容摘要：用 SHA-256 证明"内容确实变了"，而不是只凭大小/时间猜测
    std::wstring err;
    const SingleHashResult hashed = hashSingleFile(path, {Algo::SHA256}, 1u << 20, true, nullptr,
                                                   false, 1);
    if (hashed.ok && !hashed.digests.empty()) {
        out.digest = hashed.digests.front();
        out.digestValid = true;
    }
    (void)err;
    return true;
}

void FileActivityMonitor::emitActivity(const std::wstring& path, const FileSnapshot& before,
                                       const FileSnapshot& after, ActivityKind kind,
                                       const std::wstring& newPath, int64_t timeMs) {
    ActivityRecord record;
    record.timeMs = timeMs > 0 ? timeMs : nowMs();
    record.kind = kind;
    record.path = path;
    record.newPath = newPath;
    record.before = before;
    record.after = after;
    if (before.digestValid && after.digestValid) {
        record.digestComputed = true;
        record.digestChanged = before.digest != after.digest;
    }
    // 归因：查一次谁正持有这个文件
    attributeProcess(path, record.pid, record.processName, record.op);

    std::lock_guard<std::mutex> lock(mutex_);
    if (activities_.size() >= options_.maxRecords) return;
    activities_.push_back(record);
    pendingActivity_.push_back(record);
    stats_.activityRecords = activities_.size();
    if (record.pid != 0) ++stats_.activitiesWithProcess;
}

// 在当前句柄采样结果里找"谁持有这个文件"，取访问权限最强的那一个
bool FileActivityMonitor::attributeProcess(const std::wstring& path, uint32_t& pid,
                                           std::wstring& name, WatchOp& op) {
    const std::wstring lower = toLower(path);
    auto search = [&]() -> bool {
        std::lock_guard<std::mutex> lock(mutex_);
        bool found = false;
        for (const AccessRecord& access : accesses_) {
            if (toLower(access.filePath) != lower) continue;
            if (!found || opStrength(access.op) > opStrength(op)) {
                pid = access.pid;
                name = access.processName;
                op = access.op;
                found = true;
            }
        }
        return found;
    };
    if (search()) return true;

    // 兜底：活动是稀有事件，这里补一次句柄采样再匹配一次。
    // "刚创建/刚写完就关句柄"的程序往往正好躲过定时采样，补扫能显著提高归因率。
    if (options_.pollHandles && stats_.handleScanAvailable) {
        std::vector<AccessRecord> fresh;
        {
            std::lock_guard<std::mutex> scanLock(scanMutex_);
            scanHandles(fresh);
            for (const AccessRecord& record : fresh) mergeAccess(record);
        }
        if (search()) return true;
    }
    return false;
}

// 目录变更事件 → 活动记录：立即补一次快照对比，让"改了"变成"改了什么"
void FileActivityMonitor::trackChanges(const std::vector<ChangeRecord>& source) {
    // Windows 的一次改名会发出两条记录（先是 OLD、再是 NEW），这里把它们配成一条
    // "改名为 xxx" 的活动；配不上对的 OLD 仍按改名处理（只是拿不到新名字）。
    std::wstring pendingRenameFrom;
    auto emitRename = [&](const std::wstring& from, const std::wstring& to, int64_t timeMs) {
        FileSnapshot before;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = snapshots_.find(toLower(from));
            if (it != snapshots_.end()) before = it->second;
        }
        FileSnapshot after;
        takeSnapshot(from, options_.digestContent, after);
        emitActivity(from, before, after, ActivityKind::Renamed, to, timeMs);
        std::lock_guard<std::mutex> lock(mutex_);
        snapshots_.erase(toLower(from));
        if (!to.empty()) {
            FileSnapshot moved;
            if (takeSnapshot(to, options_.digestContent, moved)) snapshots_[toLower(to)] = moved;
        }
    };

    for (const ChangeRecord& change : source) {
        if (change.kind == ChangeKind::RenamedNew) {
            if (!pendingRenameFrom.empty()) {
                emitRename(pendingRenameFrom, change.path, change.timeMs);
                pendingRenameFrom.clear();
            }
            continue;
        }
        if (change.kind == ChangeKind::RenamedOld) {
            if (!pendingRenameFrom.empty()) emitRename(pendingRenameFrom, std::wstring(), 0);
            pendingRenameFrom = change.path;
            continue;
        }
        if (!pendingRenameFrom.empty()) {
            emitRename(pendingRenameFrom, std::wstring(), 0);
            pendingRenameFrom.clear();
        }

        const std::wstring key = toLower(change.path);
        FileSnapshot before;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = snapshots_.find(key);
            if (it != snapshots_.end()) before = it->second;
        }

        FileSnapshot after;
        const bool exists = takeSnapshot(change.path, options_.digestContent, after);

        ActivityKind kind = ActivityKind::ContentWritten;
        switch (change.kind) {
            case ChangeKind::Added:
                kind = before.exists ? ActivityKind::ContentWritten : ActivityKind::Created;
                break;
            case ChangeKind::Removed:
                kind = ActivityKind::Deleted;
                break;
            case ChangeKind::RenamedOld:
                kind = ActivityKind::Renamed;
                break;
            case ChangeKind::AttrChanged:
                kind = (before.exists && after.exists && before.attributes != after.attributes)
                           ? ActivityKind::AttributesChanged
                           : ActivityKind::TimestampsChanged;
                break;
            case ChangeKind::Modified:
            default:
                if (before.exists && exists) {
                    if (after.size > before.size) {
                        kind = ActivityKind::ContentWritten;
                    } else if (after.size < before.size) {
                        kind = ActivityKind::Truncated;
                    } else if (before.digestValid && after.digestValid) {
                        // 大小没变：靠摘要区分"真的改写了"和"只是动了时间戳"
                        kind = before.digest != after.digest ? ActivityKind::Rewritten
                                                             : ActivityKind::TimestampsChanged;
                    } else if (before.mtime != after.mtime) {
                        kind = ActivityKind::Rewritten;   // 没算摘要时只能保守判定
                    } else {
                        kind = ActivityKind::TimestampsChanged;
                    }
                } else {
                    kind = exists ? ActivityKind::Created : ActivityKind::Deleted;
                }
                break;
        }

        // 内容摘要能给出更准确的判断
        FileSnapshot digestBefore = before;
        FileSnapshot digestAfter = after;
        if (before.exists && exists && options_.digestContent) {
            if (!digestBefore.digestValid) {
                takeSnapshot(change.path, true, digestBefore);   // 现算一次
            }
        }
        (void)digestAfter;

        emitActivity(change.path, before, after, kind, change.newPath, change.timeMs);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exists) {
                snapshots_[key] = after;
            } else {
                snapshots_.erase(key);
            }
            if (change.kind == ChangeKind::RenamedOld && !change.newPath.empty()) {
                FileSnapshot moved;
                if (takeSnapshot(change.newPath, options_.digestContent, moved)) {
                    snapshots_[toLower(change.newPath)] = moved;
                }
            }
        }
    }
}

void FileActivityMonitor::activityLoop() {
    const int interval = options_.activityIntervalMs > 0 ? options_.activityIntervalMs : 250;
    while (!stopFlag_.load()) {
        // 目录事件已经触发了对比就立即做，否则按固定间隔轮询（文件目标也能被发现变化）
        for (int waited = 0; waited < interval && !stopFlag_.load(); waited += 50) {
            if (activityRequested_.exchange(false)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (stopFlag_.load()) break;

        // ① 目录变更事件 → 活动
        std::vector<ChangeRecord> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(pendingChangeForActivity_);
        }
        if (!pending.empty()) trackChanges(pending);

        // ② 逐个比对"被跟踪的文件"：文件目标 + 监听目录里已登记的文件。
        //    目录通知对"已打开句柄继续写入/截断"经常不漏报，这一步是活动识别的主力。
        std::vector<std::wstring> batch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t total = trackedPaths_.size();
            if (total > 0) {
                const size_t perTick = total > 256 ? 256 : total;   // 超大目录分批轮询
                for (size_t i = 0; i < perTick; ++i) {
                    batch.push_back(trackedPaths_[(pollCursor_ + i) % total]);
                }
                pollCursor_ = (pollCursor_ + perTick) % total;
            }
        }
        for (const std::wstring& path : batch) {
            if (stopFlag_.load()) break;
            pollPath(path);
        }
    }
}

void FileActivityMonitor::refreshSnapshot(const std::wstring& path, bool digestHint) {
    FileSnapshot snap;
    if (takeSnapshot(path, digestHint, snap)) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshots_[toLower(path)] = snap;
    }
}

// 把目录里已有的文件纳入定时快照对比（带数量上限，避免超大目录拖慢启动）
void FileActivityMonitor::collectTrackedFiles(const std::wstring& dir, size_t prefixLength) {
    constexpr size_t kMaxTracked = 4096;
    std::vector<DirEntry> entries;
    std::wstring err;
    if (!listDirectory(dir, entries, err)) return;
    for (const DirEntry& entry : entries) {
        if (trackedPaths_.size() >= kMaxTracked) return;
        const std::wstring lower = toLower(entry.fullPath);
        if (entry.isDir) {
            // 递归进子目录，跳过符号链接/联接点避免循环
            if (options_.recursive && (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                collectTrackedFiles(entry.fullPath, prefixLength);
            }
            continue;
        }
        if (!options_.filters.empty() && !matchesFilter(lower)) continue;
        trackedPaths_.push_back(entry.fullPath);
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshots_.find(lower) != snapshots_.end()) continue;
        FileSnapshot snap;
        if (takeSnapshot(entry.fullPath, false, snap)) snapshots_[lower] = snap;   // 起点不摘要，快
    }
}

// 比对单个文件：发现变化就产出活动记录（这是"通知漏报"情况下的主要依靠）
void FileActivityMonitor::pollPath(const std::wstring& path) {
    const std::wstring key = toLower(path);
    FileSnapshot before;
    bool hasBefore = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = snapshots_.find(key);
        if (it != snapshots_.end()) {
            before = it->second;
            hasBefore = true;
        }
    }
    FileSnapshot after;
    const bool exists = takeSnapshot(path, options_.digestContent, after);
    if (!hasBefore) {
        if (exists) {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshots_[key] = after;
        }
        return;
    }
    const bool changed =
        before.exists != exists ||
        (exists && (before.size != after.size || before.mtime != after.mtime ||
                    before.attributes != after.attributes ||
                    (before.digestValid && after.digestValid && before.digest != after.digest)));
    if (!changed) return;

    ActivityKind kind = ActivityKind::ContentWritten;
    if (!exists) {
        kind = ActivityKind::Deleted;
    } else if (!before.exists) {
        kind = ActivityKind::Created;
    } else if (after.size > before.size) {
        kind = ActivityKind::ContentWritten;
    } else if (after.size < before.size) {
        kind = ActivityKind::Truncated;
    } else if (before.attributes != after.attributes) {
        kind = ActivityKind::AttributesChanged;
    } else if (before.digestValid && after.digestValid && before.digest != after.digest) {
        kind = ActivityKind::Rewritten;
    } else {
        kind = ActivityKind::TimestampsChanged;
    }
    emitActivity(path, before, after, kind, std::wstring(), 0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (exists) {
            snapshots_[key] = after;
        } else {
            snapshots_.erase(key);
        }
    }
}

// ------------------------------------------------------------ 句柄扫描 --
void FileActivityMonitor::scanHandles(std::vector<AccessRecord>& fresh) {
    auto query = ntQuerySystemInformation();
    auto queryObject = ntQueryObject();
    if (!query || !queryObject) return;

    const auto startTick = std::chrono::steady_clock::now();

    // 只解析一次 File 对象的类型索引
    if (fileTypeIndex_ == 0) {
        wchar_t self[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        HANDLE handle = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        std::vector<unsigned char> buffer(2048);
        ULONG returned = 0;
        if (NT_SUCCESS(queryObject(handle, 2 /*ObjectTypeInformation*/, buffer.data(),
                                   static_cast<ULONG>(buffer.size()), &returned))) {
            const auto* info = reinterpret_cast<const ObjectTypeInformationFull*>(buffer.data());
            if (info->typeName.Buffer != nullptr && info->typeName.Length >= 8 &&
                _wcsnicmp(info->typeName.Buffer, L"File", 4) == 0) {
                fileTypeIndex_ = info->typeIndex;
            }
        }
        CloseHandle(handle);
        if (fileTypeIndex_ == 0) return;
    }

    ULONG size = 4u << 20;
    std::vector<unsigned char> buffer;
    NTSTATUS status = 0;
    ULONG needed = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        buffer.resize(size);
        status = query(kSystemExtendedHandleInformation, buffer.data(), size, &needed);
        if (NT_SUCCESS(status)) break;
        size = needed + (2u << 20);
    }
    if (!NT_SUCCESS(status)) return;

    const auto* info = reinterpret_cast<const SystemHandleInformationEx*>(buffer.data());
    const DWORD selfPid = GetCurrentProcessId();

    // 目标里是否包含目录（决定是否需要为每个句柄解析完整路径）
    bool hasDirectoryTarget = false;
    for (const TargetInfo& target : targets_) {
        if (target.isDir) { hasDirectoryTarget = true; break; }
    }

    std::set<uint64_t> aliveKeys;
    uint64_t matched = 0;

    for (ULONG_PTR i = 0; i < info->numberOfHandles; ++i) {
        const SystemHandleEntryEx& entry = info->handles[i];
        if (entry.objectTypeIndex != fileTypeIndex_ || entry.uniqueProcessId == 0) continue;
        const uint32_t pid = static_cast<uint32_t>(entry.uniqueProcessId);
        if (pid == selfPid && !options_.includeSelf) continue;
        if (options_.onlyPid != 0 && pid != options_.onlyPid) continue;

        // 缓存键：进程 + 句柄值；用对象指针判断句柄是否被复用
        const uint64_t key = (static_cast<uint64_t>(pid) << 32) | static_cast<uint32_t>(entry.handleValue);
        std::wstring path;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = handleCache_.find(key);
            if (it != handleCache_.end() && it->second.object == reinterpret_cast<uint64_t>(entry.object)) {
                path = it->second.path;
                known = true;
                it->second.idleScans = 0;
            }
        }
        aliveKeys.insert(key);

        if (!known) {
            HANDLE process = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
                                         FALSE, pid);
            if (process == nullptr) continue;   // 非管理员看不到的进程直接跳过
            HANDLE duplicate = nullptr;
            if (DuplicateHandle(process, reinterpret_cast<HANDLE>(entry.handleValue),
                                GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                if (GetFileType(duplicate) == FILE_TYPE_DISK) {
                    // 先用便宜的方式拿卷内相对路径做预筛，需要时再解析完整路径
                    struct { DWORD length; wchar_t name[2048]; } nameInfo{};
                    if (GetFileInformationByHandleEx(duplicate, FileNameInfo, &nameInfo,
                                                     sizeof(nameInfo))) {
                        const std::wstring relative(nameInfo.name, nameInfo.length / 2);
                        const std::wstring relativeLower = toLower(relative);
                        bool needFull = hasDirectoryTarget;
                        if (!needFull) {
                            const std::wstring baseLower = baseName(relativeLower);
                            for (const TargetInfo& target : targets_) {
                                if (!target.isDir && target.baseName == baseLower) {
                                    needFull = true;
                                    break;
                                }
                            }
                        }
                        if (needFull) {
                            std::vector<wchar_t> full(32768);
                            const DWORD length = GetFinalPathNameByHandleW(
                                duplicate, full.data(), static_cast<DWORD>(full.size()),
                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                            if (length > 0 && length < full.size()) {
                                path = stripExtendedPrefix(std::wstring(full.data(), length));
                            } else {
                                path = devicePathToDos(L"\\Device" + relative);
                            }
                        }
                    }
                }
                CloseHandle(duplicate);
            }
            CloseHandle(process);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                HandleCacheEntry& cache = handleCache_[key];
                cache.path = path;
                cache.object = reinterpret_cast<uint64_t>(entry.object);
                cache.access = entry.grantedAccess;
                cache.idleScans = 0;
            }
        }

        if (path.empty()) continue;
        const std::wstring lowerPath = toLower(path);
        if (!matchesTarget(lowerPath)) continue;
        ++matched;

        AccessRecord record;
        record.pid = pid;
        record.processPath = resolveProcess(pid, nullptr);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = processNameCache_.find(pid);
            record.processName = it != processNameCache_.end() ? it->second : L"(未知)";
            // 取完整可执行路径（进程名缓存里同时保存了完整路径）
            auto pit = processPathCache_.find(pid);
            if (pit != processPathCache_.end() && !pit->second.empty()) {
                record.processPath = pit->second;
            }
        }
        record.filePath = path;
        record.accessMask = entry.grantedAccess;
        record.op = classifyAccess(entry.grantedAccess);
        record.firstSeenMs = nowMs();
        record.lastSeenMs = record.firstSeenMs;
        record.samples = 1;
        record.stillOpen = true;
        fresh.push_back(record);
    }

    // 清理长期未出现的缓存项，避免无限增长
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = handleCache_.begin(); it != handleCache_.end();) {
            if (aliveKeys.count(it->first) != 0) {
                ++it;
                continue;
            }
            if (++it->second.idleScans >= 2) {
                it = handleCache_.erase(it);
            } else {
                ++it;
            }
        }
    }

    const double elapsedMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - startTick).count();
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.handleScanMs = static_cast<uint64_t>(elapsedMs);
    stats_.handlesScanned = info->numberOfHandles;
    stats_.handlesMatched = matched;
}

void FileActivityMonitor::handleLoop() {
    // 首轮扫描稍等片刻，让通知通道先就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int interval = std::max(100, options_.pollIntervalMs);
    while (!stopFlag_.load()) {
        scanRequested_.store(false);
        std::vector<AccessRecord> fresh;
        {
            // 整轮扫描会改写句柄/进程缓存，和"活动归因时的补扫"互斥
            std::lock_guard<std::mutex> scanLock(scanMutex_);
            scanHandles(fresh);
            for (const AccessRecord& record : fresh) mergeAccess(record);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.samples++;
        }
        // 平时按采样间隔轮询；一旦收到文件变更事件就立刻补一次采样，
        // 这样"写完就把句柄关掉"的程序也有机会被抓到。
        for (int i = 0; i < interval / 50 && !stopFlag_.load(); ++i) {
            if (scanRequested_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

// ------------------------------------------------------- 目录变更通知 --
void FileActivityMonitor::notifyLoop(size_t index) {
    if (index >= notifyDirs_.size()) return;
    const std::wstring dir = notifyDirs_[index];

    HANDLE directory = CreateFileW(toExtendedPath(dir).c_str(), FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return;

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ready == nullptr) {
        CloseHandle(directory);
        return;
    }

    std::vector<unsigned char> buffer(64 * 1024);
    while (!stopFlag_.load()) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = ready;
        ResetEvent(ready);
        const BOOL issued = ReadDirectoryChangesW(
            directory, buffer.data(), static_cast<DWORD>(buffer.size()),
            options_.recursive ? TRUE : FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
            nullptr, &overlapped, nullptr);
        if (!issued) break;

        HANDLE waits[2] = {static_cast<HANDLE>(cancelEvent_), ready};
        const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0) {   // 收到取消信号
            CancelIo(directory);
            break;
        }
        if (waitResult != WAIT_OBJECT_0 + 1) break;

        DWORD bytes = 0;
        if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE)) {
            if (stopFlag_.load()) break;
            continue;
        }
        if (bytes == 0) continue;

        std::vector<ChangeRecord> items;
        const int64_t stamp = nowMs();
        const unsigned char* cursor = buffer.data();
        for (;;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor);
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            const std::wstring full = joinPath(dir, name);
            const std::wstring lower = toLower(full);
            if (matchesTarget(lower)) {
                ChangeRecord record;
                record.timeMs = stamp;
                record.kind = classifyChange(info->Action);
                record.path = full;
                if (info->NextEntryOffset == 0) {
                    items.push_back(record);
                    break;
                }
                items.push_back(record);
            }
            if (info->NextEntryOffset == 0) break;
            cursor += info->NextEntryOffset;
        }

        // 把「改名(原) → 改名(新)」配对，方便阅读
        for (size_t i = 0; i + 1 < items.size(); ++i) {
            if (items[i].kind == ChangeKind::RenamedOld &&
                items[i + 1].kind == ChangeKind::RenamedNew) {
                items[i].newPath = items[i + 1].path;
            }
        }
        if (!items.empty()) {
            appendChanges(std::move(items));
            // 立刻补一次句柄采样：写入方此刻往往还持有句柄
            scanRequested_.store(true);
        }
    }

    CloseHandle(ready);
    CloseHandle(directory);
}

// ------------------------------------------------------------ 结果快照 --
WatchResult FileActivityMonitor::snapshot() {
    // 最后一次采样：把"监听结束时已经不再持有句柄"的记录标记出来。
    // 注意这里不能依赖 running_（调用方通常已经 stop()），scanHandles() 本身与线程无关。
    std::vector<AccessRecord> fresh;
    if (options_.pollHandles && stats_.handleScanAvailable) {
        scanHandles(fresh);
        for (const AccessRecord& record : fresh) mergeAccess(record);
        std::lock_guard<std::mutex> lock(mutex_);
        for (AccessRecord& record : accesses_) record.stillOpen = false;
        for (const AccessRecord& record : fresh) {
            std::string key = std::to_string(record.pid) + "|" + wideToUtf8(toLower(record.filePath));
            auto it = accessIndex_.find(key);
            if (it != accessIndex_.end()) accesses_[it->second].stillOpen = true;
        }
    }

    WatchResult result;
    result.targets = normalized_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.accesses = accesses_;
        result.changes = changes_;
        result.activities = activities_;
        result.stats = stats_;
        std::set<uint32_t> pids;
        std::set<std::wstring> files;
        for (const AccessRecord& record : accesses_) {
            pids.insert(record.pid);
            files.insert(toLower(record.filePath));
        }
        for (const ChangeRecord& record : changes_) files.insert(toLower(record.path));
        result.stats.accessRecords = accesses_.size();
        result.stats.changeEvents = changes_.size();
        result.stats.processes = pids.size();
        result.stats.files = files.size();
        result.stats.handleScanAvailable = stats_.handleScanAvailable;
        result.stats.elevated = stats_.elevated;
    }
    std::sort(result.accesses.begin(), result.accesses.end(),
              [](const AccessRecord& a, const AccessRecord& b) {
                  if (a.firstSeenMs != b.firstSeenMs) return a.firstSeenMs < b.firstSeenMs;
                  if (a.pid != b.pid) return a.pid < b.pid;
                  return a.filePath < b.filePath;
              });
    std::sort(result.changes.begin(), result.changes.end(),
              [](const ChangeRecord& a, const ChangeRecord& b) { return a.timeMs < b.timeMs; });
    return result;
}

// ============================================================ 报告 ====
namespace {
std::string csvField(const std::wstring& text) {
    const std::string utf8 = wideToUtf8(text);
    if (utf8.find_first_of(",\"\r\n") == std::string::npos) return utf8;
    std::string out = "\"";
    for (char c : utf8) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
}

std::string jsonField(const std::wstring& text) {
    const std::string utf8 = wideToUtf8(text);
    std::string out;
    for (unsigned char c : utf8) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}
}  // namespace

namespace {
// 把文件属性位翻译成简短中文，用于"属性变更"活动
std::string attributeText(uint32_t attributes) {
    if (attributes == 0) return "无";
    std::string text;
    auto add = [&text](const char* name) {
        if (!text.empty()) text += "+";
        text += name;
    };
    if (attributes & FILE_ATTRIBUTE_READONLY)  add("只读");
    if (attributes & FILE_ATTRIBUTE_HIDDEN)    add("隐藏");
    if (attributes & FILE_ATTRIBUTE_SYSTEM)    add("系统");
    if (attributes & FILE_ATTRIBUTE_ARCHIVE)   add("存档");
    if (attributes & FILE_ATTRIBUTE_TEMPORARY) add("临时");
    if (attributes & FILE_ATTRIBUTE_SPARSE_FILE) add("稀疏");
    if (attributes & FILE_ATTRIBUTE_COMPRESSED) add("压缩");
    if (attributes & FILE_ATTRIBUTE_ENCRYPTED) add("加密");
    return text.empty() ? "其它" : text;
}
}  // namespace
const char* activityKindName(ActivityKind kind) {
    switch (kind) {
        case ActivityKind::Created:           return "created";
        case ActivityKind::ContentWritten:    return "written";
        case ActivityKind::Truncated:         return "truncated";
        case ActivityKind::Rewritten:         return "rewritten";
        case ActivityKind::Deleted:           return "deleted";
        case ActivityKind::Renamed:           return "renamed";
        case ActivityKind::AttributesChanged: return "attributes";
        case ActivityKind::TimestampsChanged: return "timestamps";
        case ActivityKind::Lost:              return "lost";
    }
    return "unknown";
}
const char* activityKindDisplay(ActivityKind kind) {
    switch (kind) {
        case ActivityKind::Created:           return "新建文件";
        case ActivityKind::ContentWritten:    return "写入内容";
        case ActivityKind::Truncated:         return "截断内容";
        case ActivityKind::Rewritten:         return "原地改写";
        case ActivityKind::Deleted:           return "删除文件";
        case ActivityKind::Renamed:           return "改名";
        case ActivityKind::AttributesChanged: return "属性变更";
        case ActivityKind::TimestampsChanged: return "时间戳变更";
        case ActivityKind::Lost:              return "失去访问";
    }
    return "未知活动";
}

// 一行说清"干了什么"：大小变化 + 摘要是否变化 + 是谁干的
std::string describeActivityBrief(const ActivityRecord& record) {
    std::string text;
    const FileSnapshot& before = record.before;
    const FileSnapshot& after = record.after;

    switch (record.kind) {
        case ActivityKind::Created:
            text = "新建，初始 " + formatByteSize(after.size);
            break;
        case ActivityKind::Deleted: {
            text = "删除（原大小 " + formatByteSize(before.size) + "）";
            break;
        }
        case ActivityKind::Renamed: {
            text = "改名为 " + wideToUtf8(baseName(record.newPath));
            break;
        }
        case ActivityKind::AttributesChanged: {
            text = "属性 " + attributeText(before.attributes) + " → " + attributeText(after.attributes);
            break;
        }
        case ActivityKind::Truncated: {
            text = "大小 " + formatByteSize(before.size) + " → " + formatByteSize(after.size) + "（-" +
                   formatByteSize(before.size - after.size) + "）";
            break;
        }
        case ActivityKind::ContentWritten: {
            const uint64_t grew = after.size > before.size ? after.size - before.size : 0;
            text = "大小 " + formatByteSize(before.size) + " → " + formatByteSize(after.size);
            if (grew > 0) text += "（+" + formatByteSize(grew) + "）";
            break;
        }
        case ActivityKind::Rewritten: {
            text = "大小不变（" + formatByteSize(after.size) + "）";
            break;
        }
        case ActivityKind::TimestampsChanged: {
            text = "只有修改时间变化（大小仍为 " + formatByteSize(after.size) + "）";
            break;
        }
        case ActivityKind::Lost: {
            text = "监听期间失去访问权";
            break;
        }
    }
    if (record.digestComputed) {
        text += record.digestChanged ? "；内容摘要已变化" : "；内容摘要未变";
    }
    if (!record.processName.empty()) {
        text += "；当时持有者 " + wideToUtf8(record.processName) + " (pid " +
                std::to_string(record.pid) + "，" + watchOpDisplay(record.op) + ")";
    } else if (record.pid == 0 && record.kind != ActivityKind::Deleted) {
        text += "；未捕捉到持有进程（瞬时操作或权限不足）";
    }
    return text;
}
std::string buildWatchReport(const WatchResult& result, OutFormat format) {
    std::string out;
    const WatchStats& stats = result.stats;

    if (format == OutFormat::Json) {
        out += "{\n";
        out += "  \"tool\": \"EHsc\",\n";
        out += "  \"version\": \"" + std::string(versionString()) + "\",\n";
        out += "  \"generated\": \"" + isoNowUtc() + "\",\n";
        out += "  \"kind\": \"watch\",\n";
        out += "  \"targets\": [";
        for (size_t i = 0; i < result.targets.size(); ++i) {
            if (i) out += ", ";
            out += "\"" + jsonField(result.targets[i]) + "\"";
        }
        out += "],\n";
        char number[64];
        std::snprintf(number, sizeof(number), "%.2f", result.elapsed);
        out += "  \"durationSeconds\": " + std::string(number) + ",\n";
        out += "  \"elevated\": " + std::string(stats.elevated ? "true" : "false") + ",\n";
        out += "  \"note\": \"" + jsonField(stats.note) + "\",\n";
        out += "  \"stats\": {\"accessRecords\": " + std::to_string(stats.accessRecords) +
               ", \"changeEvents\": " + std::to_string(stats.changeEvents) +
               ", \"processes\": " + std::to_string(stats.processes) +
               ", \"files\": " + std::to_string(stats.files) +
               ", \"samples\": " + std::to_string(stats.samples) +
               ", \"activityRecords\": " + std::to_string(stats.activityRecords) +
               ", \"activitiesWithProcess\": " + std::to_string(stats.activitiesWithProcess) +
               "},\n";
        out += "  \"accesses\": [\n";
        for (size_t i = 0; i < result.accesses.size(); ++i) {
            const AccessRecord& record = result.accesses[i];
            std::snprintf(number, sizeof(number), "%lld",
                          static_cast<long long>(record.lastSeenMs - record.firstSeenMs));
            out += "    {\"pid\": " + std::to_string(record.pid) + ", \"process\": \"" +
                   jsonField(record.processName) + "\", \"processPath\": \"" +
                   jsonField(record.processPath) + "\", \"path\": \"" + jsonField(record.filePath) +
                   "\", \"operation\": \"" + watchOpName(record.op) + "\", \"accessMask\": " +
                   std::to_string(record.accessMask) + ", \"firstSeen\": \"" +
                   formatTimeMs(record.firstSeenMs) + "\", \"lastSeen\": \"" +
                   formatTimeMs(record.lastSeenMs) + "\", \"durationMs\": " + number +
                   ", \"samples\": " + std::to_string(record.samples) + ", \"stillOpen\": " +
                   (record.stillOpen ? "true" : "false") + "}";
            if (i + 1 < result.accesses.size()) out += ",";
            out += "\n";
        }
        out += "  ],\n  \"changes\": [\n";
        for (size_t i = 0; i < result.changes.size(); ++i) {
            const ChangeRecord& record = result.changes[i];
            out += "    {\"time\": \"" + formatTimeMs(record.timeMs) + "\", \"kind\": \"" +
                   changeKindName(record.kind) + "\", \"path\": \"" + jsonField(record.path) + "\"";
            if (!record.newPath.empty()) out += ", \"newPath\": \"" + jsonField(record.newPath) + "\"";
            out += "}";
            if (i + 1 < result.changes.size()) out += ",";
            out += "\n";
        }
        out += "  ],\n  \"activities\": [\n";
        for (size_t i = 0; i < result.activities.size(); ++i) {
            const ActivityRecord& record = result.activities[i];
            out += "    {\"time\": \"" + formatTimeMs(record.timeMs) + "\", \"activity\": \"" +
                   activityKindName(record.kind) + "\", \"description\": \"" +
                   jsonField(utf8ToWide(describeActivityBrief(record))) + "\", \"path\": \"" +
                   jsonField(record.path) + "\"";
            if (!record.newPath.empty()) out += ", \"newPath\": \"" + jsonField(record.newPath) + "\"";
            out += ", \"sizeBefore\": " + std::to_string(record.before.size) +
                   ", \"sizeAfter\": " + std::to_string(record.after.size) +
                   ", \"existedBefore\": " + (record.before.exists ? "true" : "false") +
                   ", \"existsAfter\": " + (record.after.exists ? "true" : "false");
            if (record.digestComputed) {
                out += ", \"digestBefore\": \"" + jsonField(utf8ToWide(record.before.digest)) + "\"";
                out += ", \"digestAfter\": \"" + jsonField(utf8ToWide(record.after.digest)) + "\"";
                out += ", \"digestChanged\": " +
                       std::string(record.digestChanged ? "true" : "false");
            }
            out += ", \"pid\": " + std::to_string(record.pid) + ", \"process\": \"" +
                   jsonField(record.processName) + "\", \"operation\": \"" + watchOpName(record.op) +
                   "\"}";
            if (i + 1 < result.activities.size()) out += ",";
            out += "\n";
        }
        out += "  ]\n}\n";
        return out;
    }

    if (format == OutFormat::Csv) {
        out += "# EHsc 文件访问监听报告 v" + std::string(versionString()) + " 生成于 " +
               isoNowUtc() + "\n";
        out += "# 权限: " + wideToUtf8(stats.note) + "\n";
        out += "Type,Time,EndTime,PID,Process,Operation,Path,NewPath,DurationMs,Samples,AccessMask,"
               "SizeBefore,SizeAfter,DigestBefore,DigestAfter,Note\n";
        for (const AccessRecord& record : result.accesses) {
            out += "Access," + formatTimeMs(record.firstSeenMs) + "," + formatTimeMs(record.lastSeenMs) +
                   "," + std::to_string(record.pid) + "," + csvField(record.processName) + "," +
                   watchOpName(record.op) + "," + csvField(record.filePath) + ",," +
                   std::to_string(record.lastSeenMs - record.firstSeenMs) + "," +
                   std::to_string(record.samples) + "," + std::to_string(record.accessMask) +
                   ",,,,,\n";
        }
        for (const ChangeRecord& record : result.changes) {
            out += "Change," + formatTimeMs(record.timeMs) + ",,0,,," + csvField(record.path) + "," +
                   csvField(record.newPath) + ",,,,,,,,\n";
        }
        // 活动行：Type=Activity，附带大小变化、摘要变化与归因进程
        for (const ActivityRecord& record : result.activities) {
            out += "Activity," + formatTimeMs(record.timeMs) + ",," + std::to_string(record.pid) + "," +
                   csvField(record.processName) + "," + watchOpName(record.op) + "," +
                   csvField(record.path) + "," + csvField(record.newPath) + ",,," + "," +
                   std::to_string(record.before.size) + "," + std::to_string(record.after.size) + "," +
                   csvField(utf8ToWide(record.before.digest)) + "," +
                   csvField(utf8ToWide(record.after.digest)) + "," +
                   csvField(utf8ToWide(std::string(activityKindDisplay(record.kind)) + "；" +
                                       describeActivityBrief(record))) +
                   "\n";
        }
        return out;
    }

    // 文本报告
    out += "# EHsc 文件访问监听报告 v" + std::string(versionString()) + "\n";
    out += "# 生成时间: " + isoNowUtc() + "\n";
    out += "# 监听目标: ";
    for (size_t i = 0; i < result.targets.size(); ++i) {
        if (i) out += " | ";
        out += wideToUtf8(result.targets[i]);
    }
    out += "\n";
    char line[128];
    std::snprintf(line, sizeof(line), "# 监听时长: %.1f 秒    句柄采样: %llu 轮（最近一轮 %llu ms）\n",
                  result.elapsed, static_cast<unsigned long long>(stats.samples),
                  static_cast<unsigned long long>(stats.handleScanMs));
    out += line;
    out += "# 权限说明: " + wideToUtf8(stats.note) + "\n";
    out += "#\n";
    out += "# === 进程访问（谁打开着目标文件）: " + std::to_string(result.accesses.size()) +
           " 条 ===\n";
    if (result.accesses.empty()) {
        out += "#   （监听期间未观察到任何进程打开目标文件）\n";
    }
    for (const AccessRecord& record : result.accesses) {
        out += "[" + formatTimeMsShort(record.firstSeenMs) + " ~ " +
               formatTimeMsShort(record.lastSeenMs) + "]  " +
               padToWidth(watchOpDisplay(record.op), 10) + "  " +
               padToWidth(wideToUtf8(record.processName) + " (pid " + std::to_string(record.pid) + ")",
                          34) +
               wideToUtf8(record.filePath) +
               (record.stillOpen ? "   [监听结束时仍打开]" : "") + "\n";
    }
    out += "#\n# === 文件变更事件（Windows 不提供发起进程）: " + std::to_string(result.changes.size()) +
           " 条 ===\n";
    if (result.changes.empty()) {
        out += "#   （监听期间目标文件没有发生变化）\n";
    }
    for (const ChangeRecord& record : result.changes) {
        out += "[" + formatTimeMs(record.timeMs) + "]  " +
               padToWidth(changeKindDisplay(record.kind), 10) + wideToUtf8(record.path);
        if (!record.newPath.empty()) out += "  ->  " + wideToUtf8(record.newPath);
        out += "\n";
    }
    out += "#\n# === 文件活动时间线（这个文件被干了什么）: " +
           std::to_string(result.activities.size()) + " 条 ===\n";
    if (result.activities.empty()) {
        out += "#   （监听期间目标文件没有发生可识别的变化）\n";
    }
    for (const ActivityRecord& record : result.activities) {
        out += "[" + formatTimeMs(record.timeMs) + "]  " +
               padToWidth(activityKindDisplay(record.kind), 12) + wideToUtf8(record.path) + "\n";
        out += "         " + describeActivityBrief(record) + "\n";
    }
    return out;
}

bool watchPathMatches(const std::vector<std::wstring>& targets, bool recursive,
                      const std::wstring& lowerPath) {
    for (const std::wstring& raw : targets) {
        const std::wstring target = toLower(fullPath(stripQuotes(raw)));
        if (target.empty()) continue;
        if (isDirectory(target)) {
            if (lowerPath.size() <= target.size()) continue;
            if (_wcsnicmp(lowerPath.c_str(), target.c_str(), target.size()) != 0) continue;
            const wchar_t sep = lowerPath[target.size()];
            if (sep != L'\\' && sep != L'/') continue;
            if (!recursive &&
                lowerPath.find_first_of(L"\\/", target.size() + 1) != std::wstring::npos)
                continue;
            return true;
        }
        if (lowerPath == target) return true;
    }
    return false;
}

}  // namespace ehsc
