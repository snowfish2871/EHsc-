// ============================================================================
//  EHsc · win32_utils.h
//  Windows 原生能力封装：文件/文件夹选择器、控制台 UTF-8 与虚拟终端、
//  目录枚举、文件读取句柄、系统错误信息、键盘轮询。
//
//  本模块是整个项目里唯一直接包含 <windows.h> 的地方，其他模块保持平台无关，
//  便于日后移植，也避免 Windows 宏（min/max 等）污染全局命名空间。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ehsc {

// Windows 文件属性位（取值与 FILE_ATTRIBUTE_* 完全一致）。
// 这里显式定义是为了让不包含 <windows.h> 的模块也能使用这些标志。
enum FileAttribute : uint32_t {
    FileAttrReadOnly    = 0x00000001u,
    FileAttrHidden      = 0x00000002u,
    FileAttrSystem      = 0x00000004u,
    FileAttrDirectory   = 0x00000010u,
    FileAttrArchive     = 0x00000020u,
    FileAttrReparsePoint = 0x00000400u,
};

// ------------------------------------------------------------ 控制台相关 --
// 将控制台输入/输出代码页设为 UTF-8，并开启 ANSI 虚拟终端序列（颜色、光标移动）
void setupConsoleUtf8();
// 把标准流重新接到本进程自己的控制台上。
// 典型场景：UAC 提权后启动的新进程可能继承了父进程（中完整性级别）的控制台句柄，
// 这些句柄不可用 —— 表现为"什么都打印不出来、按键也读不到"。
// 只有在"进程确实有控制台窗口、但标准句柄既不是可用控制台也不是文件/管道重定向"时才动手，
// 因此不会破坏正常的输出重定向。
bool rebindConsoleStreams();
// 本次启动是否发生过标准流重绑。用于区分"本来就是交互式控制台"和
// "只是被重绑后才拿到控制台"—— 后者不应触发"是否提权"这类交互式提问，
// 否则会把输出被重定向的脚本卡在提问上。
bool consoleStreamsWereRebound();
// 恢复控制台原始代码页与模式（退出前调用）
void restoreConsole();
bool stdoutIsTerminal();
bool stdinIsConsole();      // 标准输入是否为控制台（决定键盘交互是否可用）
// 标准输入是否立刻可读（控制台，或管道/文件里已经有数据）。
// 用于"想问一句但绝不阻塞脚本"的场景：没有待读数据就不问。
bool stdinHasPendingInput();
int  consoleColumns();
// 清屏并把光标移到左上角
void clearScreen();

// ------------------------------------------------------------ 键盘轮询 --
// 返回等待中的按键：可打印字符返回其 ASCII 值，方向键等功能键返回 0，无按键返回 -1。
// 非交互式场景（重定向输入）始终返回 -1。
int pollKey();

// ------------------------------------------------------ 控制台鼠标支持 --
// 控制台输入事件：键盘按键、鼠标左键单击，或什么都没有。
enum class ConsoleEventKind { None, Key, MouseClick };
struct ConsoleEvent {
    ConsoleEventKind kind = ConsoleEventKind::None;
    int              key = 0;    // kind == Key：可打印字符的 ASCII（Esc=27，回车=13）
    int              row = 0;    // kind == MouseClick：点击位置（控制台缓冲区坐标）
    int              column = 0;
};
// 取一个输入事件（不阻塞）。鼠标只报告"左键按下"那一刻，忽略抬起与移动。
ConsoleEvent pollConsoleEvent();
// 当前光标位置（控制台缓冲区坐标，不是窗口坐标）
int consoleCursorRow();
int consoleCursorColumn();
// 读取控制台某一行当前显示的文本（已去掉尾部空白）。
// 用途：把鼠标点击的行对回"实际显示的内容"，这样即使缓冲区滚动导致行号变化也不会点错。
std::string consoleLineText(int row);
// 当前是否支持鼠标点击交互（有真实控制台且鼠标输入已开启）
bool consoleMouseAvailable();

// ------------------------------------------------------ UTF-8 / 宽字符 --
std::string  wideToUtf8(const std::wstring& w);
std::wstring utf8ToWide(const std::string& s);

// ------------------------------------------------------------ 系统错误 --
std::wstring formatWinError(unsigned long code);
std::wstring lastWinError();

// ------------------------------------------------------------ 提权相关 --
// 当前进程是否以管理员（高完整性级别）身份运行
bool isProcessElevated();

// 按 Windows 命令行规则给单个参数加引号（用于重新拼装命令行）
std::wstring quoteCommandLineArgument(const std::wstring& argument);
// 用给定参数重新启动本程序本身，动词默认 "runas"（弹出 UAC 提权）
// 环境变量 EHSC_ELEVATE_VERB 可覆盖动词（调试/自动化测试用，例如 open 表示不提权）。
enum class ElevateStatus {
    Started,     // 已成功启动（UAC 已通过）
    Cancelled,   // 用户在 UAC 对话框上点了"否"或被策略拒绝
    Failed,      // 其他失败（见 lastError）
};
ElevateStatus relaunchElevated(const std::wstring& parameters, unsigned long* lastError);
// 等待上面启动的提权子进程结束，返回其退出码（拿不到时返回 -1）
int waitForElevatedChild(unsigned long timeoutMs = 0);

// ------------------------------------------------------------ 路径工具 --
bool isAbsolutePath(const std::wstring& path);
std::wstring toExtendedPath(const std::wstring& path);   // 支持超过 MAX_PATH 的长路径
std::wstring fullPath(const std::wstring& path);         // 规范化绝对路径
std::wstring currentDirectory();
std::wstring executableDirectory();                      // 可执行文件所在目录
std::wstring executablePath();
std::wstring joinPath(const std::wstring& dir, const std::wstring& name);
std::wstring baseName(const std::wstring& path);
std::wstring parentDir(const std::wstring& path);
std::wstring fileExtension(const std::wstring& path);    // 含点，小写
bool pathExists(const std::wstring& path);
bool isDirectory(const std::wstring& path);
bool isFile(const std::wstring& path);
bool isReparsePoint(const std::wstring& path);
bool wildcardMatch(const std::wstring& pattern, const std::wstring& name);

// ------------------------------------------------------------ 文件信息 --
struct FileInfo {
    bool     exists = false;
    bool     isDir = false;
    uint64_t size = 0;
    int64_t  mtime = 0;        // Unix 时间戳（秒，UTC）
    uint32_t attributes = 0;
};
bool getFileInfo(const std::wstring& path, FileInfo& info, std::wstring& err);

struct DirEntry {
    std::wstring name;
    std::wstring fullPath;
    bool         isDir = false;
    uint64_t     size = 0;
    int64_t      mtime = 0;
    uint32_t     attributes = 0;
};
// 列出目录内容（不含 . 与 ..）；目录不存在时返回 false 并填充 err
bool listDirectory(const std::wstring& dir, std::vector<DirEntry>& out, std::wstring& err);

// ------------------------------------------------------------ 文件读取 --
// RAII 文件句柄，使用 CreateFileW / ReadFile 顺序读取
class WinFile {
public:
    WinFile() = default;
    ~WinFile();
    WinFile(const WinFile&) = delete;
    WinFile& operator=(const WinFile&) = delete;
    WinFile(WinFile&& other) noexcept;
    WinFile& operator=(WinFile&& other) noexcept;

    bool open(const std::wstring& path, std::wstring& err);
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    uint64_t size() const { return size_; }
    // 顺序读取，got 为实际读取字节数（0 表示已到文件尾）
    bool read(void* buffer, size_t capacity, size_t& got, std::wstring& err);

private:
    void*    handle_ = nullptr;
    uint64_t size_ = 0;
};

// --------------------------------------------------- 文本文件读写（UTF-8） --
bool writeFileUtf8(const std::wstring& path, const std::string& data, bool withBom,
                   std::wstring& err);
bool readFileUtf8(const std::wstring& path, std::string& data, std::wstring& err);

// ------------------------------------------------------- 目录与文件属性 --
// 递归创建目录（已存在视为成功）
bool createDirectories(const std::wstring& path);
// 设置文件的“隐藏”属性（保留原有属性位，用于隐藏配置文件）
bool setFileHidden(const std::wstring& path);
bool isFileHidden(const std::wstring& path);
// 目录是否可写（用带 DELETE_ON_CLOSE 的临时文件实测，不留下垃圾）
bool isDirectoryWritable(const std::wstring& dir);
// 读取环境变量（不存在时返回空串）
std::wstring environmentVariable(const wchar_t* name);
// 用户漫游配置目录 %APPDATA%（拿不到时回退到 %USERPROFILE%\AppData\Roaming）
std::wstring roamingAppDataDirectory();
// 临时目录 %TEMP%
std::wstring tempDirectory();

// --------------------------------------------------- 原生文件选择对话框 --
// 均返回用户选择的路径；用户取消时返回空。multiple 为 true 时 pickFiles 支持多选。
std::vector<std::wstring> pickFiles(const std::wstring& title, bool multiple);
std::wstring pickFolder(const std::wstring& title);
std::wstring pickSaveFile(const std::wstring& title, const std::wstring& defaultName,
                          const std::wstring& filterLabel, const std::wstring& filterSpec);

// 用于捕获 Ctrl+C / 关闭窗口，回调只做置位操作
void installConsoleCtrlHandler(bool (*handler)(void* ctx), void* ctx);

}  // namespace ehsc
