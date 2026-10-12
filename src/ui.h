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
// 配置文件的实际状态（供界面与 config 子命令显示）
bool configFileExists();
bool configFileIsHidden();
std::wstring configSearchDescription();
bool loadConfig(Config& cfg);
bool saveConfig(const Config& cfg);
// 首次运行时创建默认配置文件（写入隐藏属性）；已存在则直接返回 true
bool ensureConfigExists(const Config& cfg);
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
// 带超时的询问：超时（无人应答）时返回默认值并置 timedOut。
// 交互式控制台上用键盘轮询实现，因此脚本/无人值守场景不会被永久卡住。
bool askYesNoTimed(const std::string& prompt, bool defaultValue, int timeoutSeconds,
                   bool* timedOut);

// ------------------------------------------------- 可点击菜单（鼠标） ----
// 把"控制台缓冲区里的某一行（可再限定列范围）"登记为可点击区域，
// 之后 askMenuChoice 会同时接受键盘输入与鼠标点击。
void clickableClear();
int  clickableRow();       // 打印条目前调用：拿到这一行会落在哪个缓冲区行
int  clickableColumn();    // 同上，列号（用于同一行内的多个可点击选项）
// 登记整行为可点击；label 是该行的可见文本（用于点击时按内容核对，
// 这样即使控制台缓冲区滚动导致行号变化也不会点错条目）
void clickableAddRow(int row, int value, const std::string& label);
// 登记行内某个文字片段为可点击；token 必须与屏幕上显示的原文一致
void clickableAddSpan(int row, int colBegin, int colEnd, int value, const std::string& token);
bool clickableAvailable(); // 当前环境是否支持鼠标点击
void printClickHint();     // 打印"可以直接点击"的提示（不支持时什么都不打印）

// 读一个菜单选项：键盘输入数字 + 回车，或鼠标点击已登记的条目。
// 返回选中的值；按 Esc / 直接回车返回 defaultValue。
int askMenuChoice(const std::string& prompt, int low, int high, int defaultValue);

// 一行式选项菜单（例如"清单格式 1) 文本 2) CSV 3) JSON"），每个选项都可点击。
int askOptionRow(const std::string& title,
                 const std::vector<std::pair<int, std::string>>& options, int defaultValue);

// ------------------------------------------------- 框线风格界面（█ ═ ╭─╮）----
struct BoxMenuItem {
    int         value = 0;
    std::string label;            // 主标题
    std::string hint;             // 右侧说明（可空）
    bool        accent = false;   // 是否用高亮色
};

// 顶部标题框：╭───╮ │ ◈ EHsc … v3.0.1 │ ╰───╯
void printAppHeader(const std::string& subtitle);
// 进入功能页：清掉上一屏（只在真实终端里）并画顶部标题框，
// 这样上一个功能的界面不会残留在控制台里。
void beginFunctionScreen(const std::string& title);
// 带框内容面板：╭─ 标题 ────╮ 多行内容 ╰────────╯
void printPanel(const std::string& title, const std::vector<std::string>& lines);
// 复选 / 单选标记（用于开关项与单选组）
std::string checkMark(bool on);   // [■] / [ ]
std::string radioMark(bool on);   // (●) / (○)
// 复选列表：↑↓ 移动、空格/数字键切换、A 全选、N 全不选、ENTER 确认、Esc 取消。
// items 传入初值，返回后 checked 里是结果；返回 false 表示取消。
bool runCheckList(const std::string& title, const std::vector<std::string>& labels,
                  std::vector<bool>& checked, const std::string& hint = std::string());
// 按钮式二选一（是/否）：←→ 或鼠标选择，ENTER 确认
bool askButtonYesNo(const std::string& question, bool defaultValue);
// 状态行：`  标签       值`（标签按显示宽度对齐）
void printStatusRow(const std::string& label, const std::string& value);
// 可导航菜单：↑↓ 选择、ENTER 确认、数字键直选、Esc/Q 返回、鼠标可点。
// 返回选中的值；取消/返回时返回 defaultValue。
// 输入被重定向时自动退回"打印编号列表 + 读一行"，脚本行为不变。
int runBoxMenu(const std::string& title, const std::vector<BoxMenuItem>& items, int defaultValue);
// 横向框线选项（如 1) 文本 2) CSV 3) JSON），当前项用 ╔═╗ 高亮
int askBoxOptions(const std::string& title,
                  const std::vector<std::pair<int, std::string>>& options, int defaultValue);
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
    // 面板是否真的在运行（enabled 且已 start）。调用方据此决定要不要自己轮询键盘：
    // 面板没跑起来时（重定向、--no-progress、UAC 新控制台句柄异常等）必须由主循环接管按键。
    bool isActive() const { return enabled_ && running_.load(); }
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

// 文件访问监听：把 watch 引擎的结果实时显示出来（实现在 main.cpp）
enum class ElevateMode {
    Ask,     // 未提权时主动询问是否以管理员身份重启（默认）
    Force,   // 直接走 UAC 提权流程，不询问
    Never,   // 保持当前权限，绝不询问/提权
};

struct WatchRunOptions {
    double                    seconds = 30.0;
    bool                      recursive = true;
    bool                      pollHandles = true;
    bool                      notifyChanges = true;
    bool                      trackActivity = true;   // 记录“它被干了什么”
    bool                      digestContent = true;   // 内容变化时用摘要证明
    bool                      includeSelf = false;
    uint32_t                  onlyPid = 0;
    size_t                    maxRecords = 20000;
    std::wstring              output;      // 报告输出路径（空 = 不写文件）
    OutFormat                 format = OutFormat::Text;
    std::vector<std::wstring> filters;
    bool                      quiet = false;
    ElevateMode               elevate = ElevateMode::Ask;
    // 交互式流程（主菜单）里即使用户输入暂时不在缓冲区，也照常提问 ——
    // 菜单本来就在等用户输入，多问一句不会改变阻塞语义。
    bool                      interactivePrompt = false;
    // 由提权后的子进程使用：跑完后暂停，等用户看完结果再关闭窗口
    bool                      pauseAtEnd = false;
    // 由提权后的子进程使用：不再次询问提权
    bool                      elevatedChild = false;
};
int cmdWatch(const Config& cfg, const std::vector<std::wstring>& targets,
             const WatchRunOptions& options);

}  // namespace ehsc
