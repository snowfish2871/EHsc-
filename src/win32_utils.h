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
// 恢复控制台原始代码页与模式（退出前调用）
void restoreConsole();
bool stdoutIsTerminal();
int  consoleColumns();
// 清屏并把光标移到左上角
void clearScreen();

// ------------------------------------------------------------ 键盘轮询 --
// 返回等待中的按键：可打印字符返回其 ASCII 值，方向键等功能键返回 0，无按键返回 -1。
// 非交互式场景（重定向输入）始终返回 -1。
int pollKey();

// ------------------------------------------------------ UTF-8 / 宽字符 --
std::string  wideToUtf8(const std::wstring& w);
std::wstring utf8ToWide(const std::string& s);

// ------------------------------------------------------------ 系统错误 --
std::wstring formatWinError(unsigned long code);
std::wstring lastWinError();

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

// --------------------------------------------------- 原生文件选择对话框 --
// 均返回用户选择的路径；用户取消时返回空。multiple 为 true 时 pickFiles 支持多选。
std::vector<std::wstring> pickFiles(const std::wstring& title, bool multiple);
std::wstring pickFolder(const std::wstring& title);
std::wstring pickSaveFile(const std::wstring& title, const std::wstring& defaultName,
                          const std::wstring& filterLabel, const std::wstring& filterSpec);

// 用于捕获 Ctrl+C / 关闭窗口，回调只做置位操作
void installConsoleCtrlHandler(bool (*handler)(void* ctx), void* ctx);

}  // namespace ehsc
