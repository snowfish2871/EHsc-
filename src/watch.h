// ============================================================================
//  EHsc · watch.h
//  文件访问监听：在指定的监听时间内，记录所选文件/文件夹被"谁"读取或写入。
//
//  两条互补的探测通道（都不需要安装驱动、不需要第三方库）：
//    1) 句柄监视（默认，无需管理员）
//       周期性枚举系统句柄表（NtQuerySystemInformation），把目标文件上所有已打开的
//       句柄解析成"进程 + 访问权限（读/写/删除）+ 起止时间"。
//       优点：能说出是哪个程序；局限：短于采样间隔的瞬时打开可能被漏掉，
//             非管理员只能看到当前用户可访问的进程（系统/其他用户进程需要管理员）。
//    2) 目录变更通知（ReadDirectoryChangesW）
//       实时捕获创建/写入/删除/改名/属性变化，哪怕进程瞬间完成也能记录时间点；
//       局限：Windows 不提供"是谁做的"，因此进程一栏只能写"未知"。
//
//  两者结合：既能指出"谁一直占着这个文件（读还是写）"，也能指出"这个文件在何时被改动"。
// ============================================================================
#pragma once

#include "core.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ehsc {

// ------------------------------------------------------------------ 事件 --
enum class WatchOp {
    Read,        // 只读打开（含执行）
    Write,       // 只写/追加
    ReadWrite,   // 读写
    Delete,      // 删除权限
    MetaOnly,    // 仅查询属性/同步，不读也不写内容
};

enum class ChangeKind {
    Added,        // 新建
    Removed,      // 删除
    Modified,     // 内容被写入
    RenamedOld,   // 改名（原名）
    RenamedNew,   // 改名（新名）
    AttrChanged,  // 属性/时间戳变化
};

const char* watchOpName(WatchOp op);
const char* watchOpDisplay(WatchOp op);
const char* changeKindName(ChangeKind kind);
const char* changeKindDisplay(ChangeKind kind);

// 一次"谁打开着这个文件"的记录（同一进程对同一文件的持续占用会合并为一条）
struct AccessRecord {
    uint32_t     pid = 0;
    std::wstring processName;      // 可执行文件名，如 notepad.exe
    std::wstring processPath;      // 完整路径（能取到时）
    std::wstring filePath;         // 目标文件的完整路径
    WatchOp      op = WatchOp::MetaOnly;
    uint32_t     accessMask = 0;   // 原始 GrantedAccess
    int64_t      firstSeenMs = 0;  // 首次观察到（Unix 毫秒）
    int64_t      lastSeenMs = 0;   // 最近一次观察到
    uint64_t     samples = 0;      // 被采样到的次数（≈ 持续时长 / 采样间隔）
    bool         stillOpen = true; // 监听结束时是否仍持有句柄
};

// 一次目录变更事件
struct ChangeRecord {
    int64_t      timeMs = 0;
    ChangeKind   kind = ChangeKind::Modified;
    std::wstring path;
    std::wstring newPath;          // 仅改名为新名时使用
    uint64_t     bytes = 0;        // 写入事件里能拿到的字节数（多数情况为 0）
};

struct WatchOptions {
    bool                      recursive = true;
    std::vector<std::wstring> filters;          // 通配符过滤，空 = 全部
    uint32_t                  onlyPid = 0;      // 只看某个进程（0 = 全部）
    double                    seconds = 30.0;   // 监听时长，0 = 直到取消
    bool                      pollHandles = true;
    int                       pollIntervalMs = 400;
    bool                      notifyChanges = true;
    bool                      includeSelf = false;   // 是否包含 EHsc 自身产生的事件
    size_t                    maxRecords = 20000;
};

struct WatchStats {
    uint64_t     accessRecords = 0;   // 合并后的访问记录条数
    uint64_t     changeEvents = 0;    // 变更事件条数
    uint64_t     samples = 0;         // 句柄采样轮次
    uint64_t     processes = 0;       // 涉及的不同进程数
    uint64_t     files = 0;           // 涉及的不同文件数
    uint64_t     handleScanMs = 0;    // 最近一轮句柄扫描耗时
    uint64_t     handlesScanned = 0;  // 最近一轮扫描的句柄总数
    uint64_t     handlesMatched = 0;  // 最近一轮命中的目标句柄数
    bool         handleScanAvailable = true;
    bool         elevated = false;
    std::wstring note;                // 能力/权限说明
};

struct WatchResult {
    std::vector<AccessRecord> accesses;
    std::vector<ChangeRecord> changes;
    WatchStats                stats;
    std::vector<std::wstring> targets;   // 规范化后的目标
    double                    elapsed = 0;
    bool                      cancelled = false;
};

// ------------------------------------------------------------------ 监视器 --
class FileActivityMonitor {
public:
    FileActivityMonitor(std::vector<std::wstring> targets, const WatchOptions& options);
    ~FileActivityMonitor();
    FileActivityMonitor(const FileActivityMonitor&) = delete;
    FileActivityMonitor& operator=(const FileActivityMonitor&) = delete;

    // 启动全部探测线程；失败时返回 false 并给出原因（例如目录不存在）
    bool start(std::wstring& err);
    void stop();
    bool running() const { return running_.load(); }

    // 取出自上次调用以来新增/更新的记录（供界面实时打印）
    void drain(std::vector<AccessRecord>& newAccesses, std::vector<ChangeRecord>& newChanges);

    // 生成合并后的完整结果（调用前会做一次最终扫描）
    WatchResult snapshot();

    const WatchStats& stats() const { return stats_; }
    const std::vector<std::wstring>& normalizedTargets() const { return normalized_; }

private:
    struct TargetInfo {
        std::wstring path;        // 规范化绝对路径（小写用于比较）
        bool         isDir = false;
        std::wstring baseName;    // 小写文件名（文件目标用）
    };

    void handleLoop();
    void notifyLoop(size_t index);
    void scanHandles(std::vector<AccessRecord>& fresh);
    void appendChanges(std::vector<ChangeRecord>&& items);
    void mergeAccess(const AccessRecord& record);
    bool matchesTarget(const std::wstring& lowerPath) const;
    bool matchesFilter(const std::wstring& lowerPath) const;
    std::wstring resolveProcess(uint32_t pid, std::wstring* fullPath);

    std::vector<TargetInfo>          targets_;
    std::vector<std::wstring>        normalized_;
    std::vector<std::wstring>        notifyDirs_;      // 需要挂目录变更通知的目录（去重后）
    WatchOptions                     options_;
    WatchStats                       stats_;

    std::atomic<bool>                running_{false};
    std::atomic<bool>                stopFlag_{false};
    std::atomic<bool>                scanRequested_{false};   // 变更事件触发立即补一次采样
    std::thread                      handleThread_;
    std::vector<std::thread>         notifyThreads_;
    void*                            cancelEvent_ = nullptr;   // HANDLE

    std::mutex                       mutex_;
    std::unordered_map<std::string, size_t> accessIndex_;     // key -> accesses_ 下标
    std::vector<AccessRecord>        accesses_;
    std::vector<ChangeRecord>        changes_;
    std::vector<AccessRecord>        pendingAccess_;
    std::vector<ChangeRecord>        pendingChange_;

    std::unordered_map<uint32_t, std::wstring> processNameCache_;
    std::unordered_map<uint32_t, std::wstring> processPathCache_;

    // 句柄解析缓存：key = pid:handle:object，避免每轮都重复复制/查询
    struct HandleCacheEntry {
        std::wstring path;
        uint32_t     access = 0;
        uint64_t     object = 0;
        int          idleScans = 0;
    };
    std::unordered_map<uint64_t, HandleCacheEntry> handleCache_;
    uint16_t fileTypeIndex_ = 0;
    uint64_t lastTickMs_ = 0;
};

// ------------------------------------------------------------------ 报告 --
std::string buildWatchReport(const WatchResult& result, OutFormat format);

// 目标路径是否被监视范围覆盖（供界面提示用）
bool watchPathMatches(const std::vector<std::wstring>& targets, bool recursive,
                      const std::wstring& lowerPath);

}  // namespace ehsc
