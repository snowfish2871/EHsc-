// ============================================================================
//  EHsc · ui.h
//  控制台界面：UTF-8 彩色输出、实时进度条、暂停/取消交互、菜单、配置持久化。
// ============================================================================
#pragma once

#include "core.h"
#include "report.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ehsc {

// ============================================================== 全局配置 ====
struct Config {
    std::vector<Algo> algos{Algo::SHA256};
    int               threads = 0;             // 0 = 自动（CPU 核心数，最多 8）
    size_t            bufferMB = 4;            // 读取缓冲区大小（MiB）
    OutFormat         format = OutFormat::Text;
    bool              progress = true;         // 显示进度条
    bool              progressForce = false;   // 即使输出被重定向也输出进度（--progress）
    bool              asciiBar = false;        // 使用 ASCII 进度条（兼容旧终端）
    bool              overlap = true;          // 读取/计算流水线
    bool              recursive = true;        // 递归子目录
    bool              includeHidden = true;    // 包含隐藏/系统文件
    bool              verifyCheckSize = true;  // 校验时同时检查大小
    bool              bom = false;             // 输出文件写入 UTF-8 BOM
    bool              colors = true;           // 彩色输出
    std::wstring      filters;                 // 形如 *.exe;*.dll 的过滤串
    std::wstring      lastDirectory;           // 上次使用的目录（记忆用）
};

std::wstring configFilePath();
bool loadConfig(Config& cfg);
bool saveConfig(const Config& cfg);
std::vector<std::wstring> parseFilters(const std::wstring& text);
size_t bufferBytes(const Config& cfg);

// ============================================================ 控制台输出 ====
enum class Color { Default, Bold, Dim, Red, Green, Yellow, Blue, Magenta, Cyan, Gray };

void enableColors(bool enabled);
bool colorsEnabled();
void setColor(Color color);
void resetColor();
std::string paint(const std::string& text, Color color);
void showBanner();
void printSectionTitle(const std::string& title);
void printInfo(const std::string& text);
void printSuccess(const std::string& text);
void printWarning(const std::string& text);
void printError(const std::string& text);

// ============================================================ 交互输入 ====
bool readLineUtf8(const std::string& prompt, std::string& line);
int askInt(const std::string& prompt, int low, int high, int defaultValue);
bool askYesNo(const std::string& prompt, bool defaultValue);
void pauseForEnter(const std::string& hint = "按回车键返回主菜单…");
// 输入路径；允许分号分隔多个；输入为空时调用 Windows 原生选择器
std::vector<std::wstring> askTargets(const std::string& prompt, bool multiple, bool folderMode);
std::wstring askSavePath(const std::string& prompt, const std::wstring& defaultName,
                         const std::wstring& filterLabel, const std::wstring& filterSpec);

// ============================================================ 进度监视器 ====
// 独立线程渲染进度，同时轮询键盘：[空格] 暂停/继续，[Q]/[Esc] 取消。
//   * 标准输出是终端时：渲染多行实时面板（进度条 / 速度 / 平均速度 / ETA / 字节数）
//   * 标准输出被重定向且 force 为 true 时：每秒输出一行纯文本进度（适合写入日志）
class ProgressMonitor {
public:
    ProgressMonitor(JobControl& control, bool asciiBar, bool enabled, bool force = false);
    ~ProgressMonitor();
    ProgressMonitor(const ProgressMonitor&) = delete;
    ProgressMonitor& operator=(const ProgressMonitor&) = delete;

    void start(const std::string& title);
    void stop();
    // 在进度面板上方打印一行日志（线程安全）
    void log(const std::string& line);
    // 设置附加状态文本（显示在面板底部）
    void setHint(const std::string& hint);

private:
    void loop();
    void drawPanel(ProgressSnapshot& snapshot);
    void drawStatusLine(ProgressSnapshot& snapshot);
    void clearPanel();
    void flushLogs();

    JobControl&              control_;
    bool                     ascii_;
    bool                     enabled_;
    bool                     interactive_;
    std::atomic<bool>        running_{false};
    std::thread              thread_;
    std::mutex               mutex_;
    std::condition_variable  wake_;
    std::deque<std::string>  logs_;
    std::string              title_;
    std::string              hint_;
    int                      linesDrawn_ = 0;
};

// ================================================================ 菜单 ====
int runInteractiveMenu(Config& cfg);
void setActiveJob(JobControl* control);
JobControl* activeJob();

// ============================================ 命令实现（位于 main.cpp） ====
int cmdSelfTest(bool verbose);
int cmdListAlgos();
int cmdHash(const Config& cfg, const std::vector<std::wstring>& targets,
            const std::wstring& outputPath, OutFormat format, bool quiet);
int cmdVerify(const Config& cfg, const std::wstring& manifestPath, const std::wstring& baseDir,
              const std::vector<Algo>& filterAlgos, bool quiet, bool reportAll);
int cmdCompare(const Config& cfg, const std::wstring& pathA, const std::wstring& pathB, bool quick,
               bool withHash);
int cmdBench(const Config& cfg, const std::vector<std::wstring>& targets, double minSeconds);

}  // namespace ehsc
