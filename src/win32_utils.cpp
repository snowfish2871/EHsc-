// ============================================================================
//  EHsc · win32_utils.cpp
//  Windows 原生 API 封装实现。
// ============================================================================
#include "win32_utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <shobjidl.h>   // IFileOpenDialog / IFileSaveDialog
#include <shellapi.h>   // ShellExecuteExW（UAC 提权重启）
#include <conio.h>      // _kbhit / _getch
#include <fcntl.h>      // _O_BINARY
#include <io.h>         // _isatty / _setmode
#include <cstdio>       // _fileno / swprintf_s

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <iostream>
#include <mutex>

namespace ehsc {
namespace {

UINT g_originalOutputCP = 0;
UINT g_originalInputCP = 0;
DWORD g_originalOutputMode = 0;
bool g_consoleSaved = false;
bool g_consoleInputModeSaved = false;
DWORD g_originalInputMode = 0;

bool isConsoleHandle(HANDLE h) {
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && h != nullptr && GetConsoleMode(h, &mode) != 0;
}

int64_t fileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // Windows 纪元 1601-01-01，Unix 纪元 1970-01-01，相差 11644473600 秒（100ns 单位）
    const uint64_t ticks = u.QuadPart;
    const uint64_t kEpochDiff = 116444736000000000ULL;
    if (ticks < kEpochDiff) return 0;
    return static_cast<int64_t>((ticks - kEpochDiff) / 10000000ULL);
}

// 极简 COM 智能指针（避免引入任何第三方库）
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put() {
        reset();
        return &ptr_;
    }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

// 只初始化一次 COM（多线程套间模式失败时容忍 RPC_E_CHANGED_MODE）
bool ensureCom() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    });
    return ok;
}

const COMDLG_FILTERSPEC kAllFilesFilter[] = {{L"所有文件 (*.*)", L"*.*"}};

// 构造文件类型过滤器。注意：storage 必须在对话框 Show() 期间保持存活，
// 因此由调用方持有，绝不能在返回时按值搬移（短字符串的 SSO 会让指针失效）。
void buildFilter(const std::wstring& label, const std::wstring& spec,
                 std::vector<std::wstring>& storage, std::vector<COMDLG_FILTERSPEC>& out) {
    storage.clear();
    out.clear();
    if (label.empty() || spec.empty()) {
        storage.emplace_back(L"所有文件 (*.*)");
        storage.emplace_back(L"*.*");
    } else {
        storage.push_back(label);
        storage.push_back(spec);
    }
    out.push_back(COMDLG_FILTERSPEC{storage[0].c_str(), storage[1].c_str()});
}

std::wstring shellItemPath(IShellItem* item) {
    if (!item) return std::wstring();
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
        result.assign(raw);
        CoTaskMemFree(raw);
    }
    return result;
}

}  // namespace

// ======================================================== 控制台 UTF-8 ====
namespace {

// 标准句柄是否"可用"：文件/管道重定向算可用（要保留重定向），
// 字符设备必须是真正的控制台才算可用。
bool handleIsUsable(HANDLE h) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    SetLastError(0);
    const DWORD type = GetFileType(h);
    if (type == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) return false;
    if (type == FILE_TYPE_CHAR) {
        DWORD mode = 0;
        return GetConsoleMode(h, &mode) != 0;
    }
    return true;   // 磁盘文件 / 管道：属于正常重定向
}

HANDLE openConsoleDevice(const wchar_t* name) {
    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

}  // namespace

namespace {
bool g_streamsRebound = false;
}

bool consoleStreamsWereRebound() { return g_streamsRebound; }

bool rebindConsoleStreams() {
    // 没有控制台窗口（例如输出被完全重定向到文件/管道）就什么都不做
    if (GetConsoleWindow() == nullptr) return false;

    bool rebound = false;
    // 三条流各自判断、各自重绑：某一条合法重定向（比如 stderr 写日志文件）时不能被牵连
    if (!handleIsUsable(GetStdHandle(STD_OUTPUT_HANDLE))) {
        HANDLE con = openConsoleDevice(L"CONOUT$");
        if (con != nullptr) {
            SetStdHandle(STD_OUTPUT_HANDLE, con);
            FILE* stream = nullptr;
            (void)freopen_s(&stream, "CONOUT$", "w", stdout);
            std::cout.clear();
            std::cout << std::unitbuf;   // 重绑后立即生效，便于实时看到日志
            rebound = true;
        }
    }
    if (!handleIsUsable(GetStdHandle(STD_ERROR_HANDLE))) {
        HANDLE con = openConsoleDevice(L"CONOUT$");
        if (con != nullptr) {
            SetStdHandle(STD_ERROR_HANDLE, con);
            FILE* stream = nullptr;
            (void)freopen_s(&stream, "CONOUT$", "w", stderr);
            std::cerr.clear();
            std::cerr << std::unitbuf;
            rebound = true;
        }
    }
    if (!handleIsUsable(GetStdHandle(STD_INPUT_HANDLE))) {
        HANDLE con = openConsoleDevice(L"CONIN$");
        if (con != nullptr) {
            SetStdHandle(STD_INPUT_HANDLE, con);
            FILE* stream = nullptr;
            (void)freopen_s(&stream, "CONIN$", "r", stdin);
            std::cin.clear();
            rebound = true;
        }
    }
    g_streamsRebound = rebound;
    return rebound;
}

void setupConsoleUtf8() {
    if (!g_consoleSaved) {
        g_originalOutputCP = GetConsoleOutputCP();
        g_originalInputCP = GetConsoleCP();
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!GetConsoleMode(hOut, &g_originalOutputMode)) g_originalOutputMode = 0;
        g_consoleSaved = true;
    }
    // 控制台代码页切换为 UTF-8（65001），保证中文正常显示与输入
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hOut, &mode)) {
        mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                ENABLE_WRAP_AT_EOL_OUTPUT;
        SetConsoleMode(hOut, mode);
    }
    // 关掉"快速编辑模式"：Windows 控制台默认开启它，用户一旦在窗口里点一下鼠标进入
    // 选择状态，任何后续输出都会阻塞（表现为程序"卡死"），而且鼠标事件也不会传给程序。
    // 程序运行期间先禁用（同时显式打开鼠标输入），退出时 restoreConsole() 会还原原始模式。
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD inMode = 0;
    if (GetConsoleMode(hIn, &inMode)) {
        if (!g_consoleInputModeSaved) {
            g_originalInputMode = inMode;
            g_consoleInputModeSaved = true;
        }
        DWORD updated = inMode | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT;
        updated &= ~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE);
        SetConsoleMode(hIn, updated);
    }
    // 让标准流以二进制方式工作，避免 CRT 再做一次编码转换
    (void)_setmode(_fileno(stdout), _O_BINARY);
    (void)_setmode(_fileno(stderr), _O_BINARY);
    SetConsoleTitleW(L"EHsc · 纯 C++ 文件哈希工具");
}

void restoreConsole() {
    if (!g_consoleSaved) return;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_originalOutputMode) SetConsoleMode(hOut, g_originalOutputMode);
    // 还原被我们临时关掉的"快速编辑模式"
    if (g_consoleInputModeSaved) {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_originalInputMode);
        g_consoleInputModeSaved = false;
    }
    if (g_originalOutputCP) SetConsoleOutputCP(g_originalOutputCP);
    if (g_originalInputCP) SetConsoleCP(g_originalInputCP);
    (void)_setmode(_fileno(stdout), _O_TEXT);
    (void)_setmode(_fileno(stderr), _O_TEXT);
    g_consoleSaved = false;
}

bool stdoutIsTerminal() {
    // 调试/自动化测试用逃生开关：设置 EHSC_FORCE_TTY=1 可强制按终端渲染
    static const bool forced = [] {
        char buffer[8] = {0};
        const DWORD len = GetEnvironmentVariableA("EHSC_FORCE_TTY", buffer,
                                                  static_cast<DWORD>(sizeof(buffer)));
        return len > 0 && buffer[0] == '1';
    }();
    if (forced) return true;
    if (_isatty(_fileno(stdout)) == 0) return false;
    return isConsoleHandle(GetStdHandle(STD_OUTPUT_HANDLE));
}

int consoleColumns() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(hOut, &info)) {
        const int cols = static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1);
        if (cols > 20) return cols;
    }
    return 100;
}

void clearScreen() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(hOut, &info)) return;
    const DWORD cells = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
    DWORD written = 0;
    COORD home{0, 0};
    FillConsoleOutputCharacterW(hOut, L' ', cells, home, &written);
    FillConsoleOutputAttribute(hOut, info.wAttributes, cells, home, &written);
    SetConsoleCursorPosition(hOut, home);
}

// ============================================================== 键盘 ====
bool stdinIsConsole() { return isConsoleHandle(GetStdHandle(STD_INPUT_HANDLE)); }

bool stdinHasPendingInput() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (isConsoleHandle(h)) return true;
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    // 管道：看缓冲区里是否已经有数据
    DWORD available = 0;
    if (PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) return available > 0;
    // 文件重定向：整个文件都是可读的，读一行不会阻塞
    if (GetFileType(h) == FILE_TYPE_DISK) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size)) return size.QuadPart > 0;
    }
    return false;
}

namespace {
HANDLE g_consoleInput = nullptr;
bool   g_consoleInputTried = false;
bool   g_leftButtonDown = false;   // 用于识别"左键按下"这一瞬间

// 直接拿一个可用的控制台输入句柄：标准输入可用就用它，否则自己打开 CONIN$。
// （不依赖 <conio.h> 的句柄缓存，提权后重绑控制台时也能正常工作。）
HANDLE consoleInputHandle() {
    if (g_consoleInputTried) return g_consoleInput;
    g_consoleInputTried = true;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        g_consoleInput = h;
        return g_consoleInput;
    }
    g_consoleInput = openConsoleDevice(L"CONIN$");
    return g_consoleInput;
}
}  // namespace

ConsoleEvent pollConsoleEvent() {
    ConsoleEvent result;
    HANDLE hIn = consoleInputHandle();
    if (hIn == nullptr) return result;

    DWORD pending = 0;
    if (!GetNumberOfConsoleInputEvents(hIn, &pending) || pending == 0) return result;

    // 逐条读取，跳过鼠标移动/窗口事件与按键抬起事件；用 ReadConsoleInputW 而不是 _getch，
    // 这样可以自己解析 ESC（虚拟键码）、同时拿到鼠标事件，也不会被 CRT 缓存句柄影响。
    for (DWORD i = 0; i < pending; ++i) {
        INPUT_RECORD record{};
        DWORD read = 0;
        if (!ReadConsoleInputW(hIn, &record, 1, &read) || read == 0) break;

        if (record.EventType == KEY_EVENT) {
            if (!record.Event.KeyEvent.bKeyDown) continue;
            const wchar_t ch = record.Event.KeyEvent.uChar.UnicodeChar;
            if (ch != 0) {
                if (ch < 128) {
                    result.kind = ConsoleEventKind::Key;
                    result.key = static_cast<int>(ch);
                    return result;
                }
                continue;   // 非 ASCII（中文输入等）忽略
            }
            const WORD vk = record.Event.KeyEvent.wVirtualKeyCode;
            if (vk == VK_ESCAPE) {
                result.kind = ConsoleEventKind::Key;
                result.key = 27;
                return result;
            }
            if (vk == VK_RETURN) {
                result.kind = ConsoleEventKind::Key;
                result.key = '\r';
                return result;
            }
            continue;   // 其它功能键忽略，继续看后面还有没有可用事件
        }

        if (record.EventType == MOUSE_EVENT) {
            const MOUSE_EVENT_RECORD& mouse = record.Event.MouseEvent;
            const bool leftDown = (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
            if (leftDown && !g_leftButtonDown) {
                g_leftButtonDown = true;
                result.kind = ConsoleEventKind::MouseClick;
                result.row = mouse.dwMousePosition.Y;
                result.column = mouse.dwMousePosition.X;
                return result;
            }
            if (!leftDown) g_leftButtonDown = false;
        }
    }
    return result;
}

int pollKey() {
    const ConsoleEvent event = pollConsoleEvent();
    return event.kind == ConsoleEventKind::Key ? event.key : -1;
}

int consoleCursorRow() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (hOut == nullptr || hOut == INVALID_HANDLE_VALUE) return -1;
    if (!GetConsoleScreenBufferInfo(hOut, &info)) return -1;
    return info.dwCursorPosition.Y;
}

int consoleCursorColumn() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (hOut == nullptr || hOut == INVALID_HANDLE_VALUE) return -1;
    if (!GetConsoleScreenBufferInfo(hOut, &info)) return -1;
    return info.dwCursorPosition.X;
}

std::string consoleLineText(int row) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (hOut == nullptr || hOut == INVALID_HANDLE_VALUE) return std::string();
    if (!GetConsoleScreenBufferInfo(hOut, &info)) return std::string();
    if (row < 0 || row >= info.dwSize.Y) return std::string();
    const int width = info.dwSize.X;
    if (width <= 0) return std::string();
    std::vector<wchar_t> buffer(static_cast<size_t>(width) + 1, L'\0');
    DWORD read = 0;
    COORD pos{0, static_cast<SHORT>(row)};
    if (!ReadConsoleOutputCharacterW(hOut, buffer.data(), static_cast<DWORD>(width), pos, &read)) {
        return std::string();
    }
    std::wstring line(buffer.data(), read);
    while (!line.empty() && (line.back() == L' ' || line.back() == L'\0')) line.pop_back();
    return wideToUtf8(line);
}

bool consoleMouseAvailable() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (hIn == nullptr || hIn == INVALID_HANDLE_VALUE) return false;
    if (!GetConsoleMode(hIn, &mode)) return false;
    return (mode & ENABLE_MOUSE_INPUT) != 0;
}

// ====================================================== UTF-8 转换 ====
std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) {
        // 回退：按本地 ANSI 代码页解释，尽量不丢数据
        const int need2 = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()),
                                              nullptr, 0);
        if (need2 <= 0) return std::wstring();
        std::wstring out2(static_cast<size_t>(need2), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), out2.data(), need2);
        return out2;
    }
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

// ======================================================== 系统错误 ====
std::wstring formatWinError(unsigned long code) {
    if (code == 0) return L"无错误";
    LPWSTR buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer),
        0, nullptr);
    std::wstring message;
    if (len && buffer) {
        message.assign(buffer, len);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
            message.pop_back();
    }
    if (buffer) LocalFree(buffer);
    if (message.empty()) {
        wchar_t fallback[64];
        swprintf_s(fallback, L"未知错误 (代码 %lu)", code);
        message = fallback;
    }
    wchar_t codeText[32];
    swprintf_s(codeText, L"%lu", code);
    return message + L" [Win32 " + codeText + L"]";
}

std::wstring lastWinError() { return formatWinError(GetLastError()); }

// ============================================================ 提权相关 ====
bool isProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size) != 0;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring quoteCommandLineArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;   // 无需引号
    }
    std::wstring out = L"\"";
    for (auto it = argument.begin();; ++it) {
        size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            out.append(backslashes * 2, L'\\');   // 结尾反斜杠要加倍，避免转义收尾引号
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += *it;
        }
    }
    out += L'"';
    return out;
}

namespace {
HANDLE g_elevatedChild = nullptr;   // 提权子进程句柄（等待后关闭）
}

ElevateStatus relaunchElevated(const std::wstring& parameters, unsigned long* lastError) {
    // 自动化测试用：直接模拟"提权被拒绝 / 提权失败"，避免依赖真实 UAC 或 shell 行为
    // （EHSC_ELEVATE_SIMULATE=cancel | fail）
    {
        wchar_t simulate[16] = {0};
        const DWORD len = GetEnvironmentVariableW(L"EHSC_ELEVATE_SIMULATE", simulate, 16);
        if (len > 0 && len < 16) {
            if (_wcsicmp(simulate, L"cancel") == 0) {
                if (lastError) *lastError = ERROR_CANCELLED;
                return ElevateStatus::Cancelled;
            }
            if (_wcsicmp(simulate, L"fail") == 0) {
                if (lastError) *lastError = ERROR_ACCESS_DENIED;
                return ElevateStatus::Failed;
            }
        }
    }

    const std::wstring exe = executablePath();
    if (exe.empty()) {
        if (lastError) *lastError = ERROR_FILE_NOT_FOUND;
        return ElevateStatus::Failed;
    }

    std::wstring verb = L"runas";   // 正规提权流程（会弹 UAC）
    // 调试/自动化测试用的动词覆盖：设成 "open" 即以普通权限重启，便于验证重启链路
    {
        wchar_t buffer[32] = {0};
        const DWORD len = GetEnvironmentVariableW(L"EHSC_ELEVATE_VERB", buffer, 32);
        if (len > 0 && len < 32) verb.assign(buffer, len);
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // SEE_MASK_NO_CONSOLE：让提权后的新进程拥有自己的控制台。
    // 若不加这个标志，子进程会尝试继承父进程（中完整性级别）的控制台，
    // 结果是标准句柄不可用 —— 表现为"新窗口里什么都不打印、按 Q 也停不下来"。
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE;
    info.hwnd = reinterpret_cast<HWND>(GetConsoleWindow());
    info.lpVerb = verb.c_str();
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.lpDirectory = nullptr;          // 继承当前工作目录
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        if (lastError) *lastError = code;
        // ERROR_CANCELLED：用户在 UAC 对话框上选择了"否"
        return code == ERROR_CANCELLED ? ElevateStatus::Cancelled : ElevateStatus::Failed;
    }
    if (g_elevatedChild) CloseHandle(g_elevatedChild);
    g_elevatedChild = info.hProcess;
    if (lastError) *lastError = 0;
    return ElevateStatus::Started;
}

int waitForElevatedChild(unsigned long timeoutMs) {
    if (g_elevatedChild == nullptr) return -1;
    const DWORD wait = WaitForSingleObject(g_elevatedChild,
                                           timeoutMs == 0 ? INFINITE : timeoutMs);
    int code = -1;
    if (wait == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(g_elevatedChild, &exitCode)) code = static_cast<int>(exitCode);
    }
    CloseHandle(g_elevatedChild);
    g_elevatedChild = nullptr;
    return code;
}

// ========================================================== 路径工具 ====
bool isAbsolutePath(const std::wstring& path) {
    if (path.size() >= 3 && (path[1] == L':') &&
        (path[2] == L'\\' || path[2] == L'/') &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
        return true;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    if (path.size() >= 2 && path[0] == L'/' && path[1] == L'/') return true;
    return false;
}

std::wstring toExtendedPath(const std::wstring& path) {
    if (path.size() >= 4 && path.compare(0, 4, L"\\\\?\\") == 0) return path;
    std::wstring p = path;
    if (!isAbsolutePath(p)) return p;
    for (wchar_t& c : p) {
        if (c == L'/') c = L'\\';
    }
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
        return L"\\\\?\\UNC\\" + p.substr(2);
    }
    return L"\\\\?\\" + p;
}

std::wstring fullPath(const std::wstring& path) {
    if (path.empty()) return path;
    const std::wstring extended = toExtendedPath(path);
    std::vector<wchar_t> buffer(32768);
    const DWORD len = GetFullPathNameW(extended.c_str(), static_cast<DWORD>(buffer.size()),
                                       buffer.data(), nullptr);
    if (len == 0 || len >= buffer.size()) return path;
    std::wstring result(buffer.data(), len);
    // 去掉 \\?\ 前缀，保持路径对用户友好
    if (result.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\\\" + result.substr(8);
    if (result.compare(0, 4, L"\\\\?\\") == 0) return result.substr(4);
    return result;
}

std::wstring currentDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD len = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (len == 0 || len >= buffer.size()) return L".";
    return std::wstring(buffer.data(), len);
}

std::wstring executablePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) return std::wstring();
    return std::wstring(buffer.data(), len);
}

std::wstring executableDirectory() {
    const std::wstring path = executablePath();
    if (path.empty()) return currentDirectory();
    const std::wstring dir = parentDir(path);
    return dir.empty() ? currentDirectory() : dir;
}

std::wstring joinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    std::wstring out = dir;
    const wchar_t last = out.back();
    if (last != L'\\' && last != L'/') out.push_back(L'\\');
    out += name;
    return out;
}

std::wstring baseName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

std::wstring parentDir(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return std::wstring();
    if (pos == 2 && p[1] == L':') return p.substr(0, 3);   // "C:\"
    return p.substr(0, pos);
}

std::wstring fileExtension(const std::wstring& path) {
    const std::wstring name = baseName(path);
    const size_t pos = name.find_last_of(L'.');
    if (pos == std::wstring::npos) return std::wstring();
    std::wstring ext = name.substr(pos);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

bool pathExists(const std::wstring& path) {
    const std::wstring p = toExtendedPath(path);
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

bool isDirectory(const std::wstring& path) {
    const std::wstring p = toExtendedPath(path);
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool isFile(const std::wstring& path) {
    const std::wstring p = toExtendedPath(path);
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool isReparsePoint(const std::wstring& path) {
    const std::wstring p = toExtendedPath(path);
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool wildcardMatch(const std::wstring& pattern, const std::wstring& name) {
    // 迭代式通配符匹配，支持 * 与 ?，大小写不敏感
    size_t p = 0, n = 0;
    size_t starP = std::wstring::npos, starN = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' ||
                                   towlower(pattern[p]) == towlower(name[n]))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            starP = p++;
            starN = n;
        } else if (starP != std::wstring::npos) {
            p = starP + 1;
            n = ++starN;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}

// ========================================================== 文件信息 ====
bool getFileInfo(const std::wstring& path, FileInfo& info, std::wstring& err) {
    info = FileInfo{};
    const std::wstring p = toExtendedPath(path);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) {
        err = lastWinError();
        return false;
    }
    info.exists = true;
    info.attributes = data.dwFileAttributes;
    info.isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.mtime = fileTimeToUnix(data.ftLastWriteTime);
    if (info.isDir) info.size = 0;
    return true;
}

bool listDirectory(const std::wstring& dir, std::vector<DirEntry>& out, std::wstring& err) {
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern.push_back(L'\\');
    pattern += L'*';
    const std::wstring search = toExtendedPath(pattern);

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        // 只有“目录存在但没有匹配项”才算空目录；路径不存在/不可访问必须报错，
        // 否则目录在扫描过程中消失会被静默当成空目录。
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_NO_MORE_FILES) return true;
        err = formatWinError(code);
        return false;
    }
    const std::wstring base = pattern.substr(0, pattern.size() - 1);
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        DirEntry entry;
        entry.name = fd.cFileName;
        entry.fullPath = base + fd.cFileName;
        entry.attributes = fd.dwFileAttributes;
        entry.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.size = entry.isDir
                         ? 0
                         : ((static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow);
        entry.mtime = fileTimeToUnix(fd.ftLastWriteTime);
        out.push_back(std::move(entry));
    } while (FindNextFileW(h, &fd));

    const DWORD code = GetLastError();
    FindClose(h);
    if (code != ERROR_NO_MORE_FILES) {
        err = formatWinError(code);
        return false;
    }
    return true;
}

// ========================================================== 文件读取 ====
WinFile::~WinFile() { close(); }

WinFile::WinFile(WinFile&& other) noexcept : handle_(other.handle_), size_(other.size_) {
    other.handle_ = nullptr;
    other.size_ = 0;
}

WinFile& WinFile::operator=(WinFile&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        size_ = other.size_;
        other.handle_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

bool WinFile::open(const std::wstring& path, std::wstring& err) {
    close();
    const std::wstring p = toExtendedPath(path);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = lastWinError() + L"（路径：" + path + L"）";
        return false;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size)) {
        size_ = static_cast<uint64_t>(size.QuadPart);
    } else {
        size_ = 0;
    }
    handle_ = h;
    return true;
}

void WinFile::close() {
    if (handle_) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    size_ = 0;
}

bool WinFile::read(void* buffer, size_t capacity, size_t& got, std::wstring& err) {
    got = 0;
    if (!handle_) {
        err = L"文件未打开";
        return false;
    }
    if (capacity == 0) return true;
    const DWORD want = static_cast<DWORD>(std::min<size_t>(capacity, 0x40000000u));
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), buffer, want, &read, nullptr)) {
        const DWORD code = GetLastError();
        if (code == ERROR_HANDLE_EOF) {
            got = 0;
            return true;
        }
        err = formatWinError(code);
        return false;
    }
    got = read;
    return true;
}

// ==================================================== 文本文件读写 ====
bool writeFileUtf8(const std::wstring& path, const std::string& data, bool withBom,
                   std::wstring& err) {
    const std::wstring extended = toExtendedPath(path);

    // 重要：CreateFileW 的 CREATE_ALWAYS 无法覆盖带「隐藏」属性的已存在文件
    // （会直接失败并返回 ERROR_ACCESS_DENIED）。隐藏的配置文件正属于这种情况，
    // 因此这里先临时摘掉隐藏属性，写完后原样恢复。
    const DWORD originalAttrs = GetFileAttributesW(extended.c_str());
    const bool existed = originalAttrs != INVALID_FILE_ATTRIBUTES;
    const bool wasHidden = existed && (originalAttrs & FILE_ATTRIBUTE_HIDDEN) != 0;
    if (wasHidden) {
        SetFileAttributesW(extended.c_str(), originalAttrs & ~FILE_ATTRIBUTE_HIDDEN);
    }

    HANDLE handle = CreateFileW(extended.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        err = lastWinError() + L"（" + path + L"）";
        if (wasHidden) SetFileAttributesW(extended.c_str(), originalAttrs);
        return false;
    }
    bool ok = true;
    DWORD written = 0;
    if (withBom) {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        if (!WriteFile(handle, bom, 3, &written, nullptr) || written != 3) {
            err = lastWinError();
            ok = false;
        }
    }
    if (ok && !data.empty()) {
        size_t offset = 0;
        while (offset < data.size()) {
            const DWORD chunk =
                static_cast<DWORD>(std::min<size_t>(data.size() - offset, 16u << 20));
            if (!WriteFile(handle, data.data() + offset, chunk, &written, nullptr)) {
                err = lastWinError();
                ok = false;
                break;
            }
            if (written == 0) {
                // 磁盘写满等情况：WriteFile 可能成功返回却写出 0 字节，
                // 必须视为失败，否则会报告“已写入”但文件是截断的。
                err = L"写入磁盘时返回 0 字节（磁盘空间不足？）";
                ok = false;
                break;
            }
            offset += written;
        }
    }
    CloseHandle(handle);
    if (wasHidden) {
        // 恢复隐藏属性（成功与否都恢复，避免留下“写着写着就可见了”的文件）
        SetFileAttributesW(extended.c_str(), GetFileAttributesW(extended.c_str()) |
                                                 FILE_ATTRIBUTE_HIDDEN);
    }
    return ok;
}

bool readFileUtf8(const std::wstring& path, std::string& data, std::wstring& err) {
    WinFile file;
    if (!file.open(path, err)) return false;
    data.clear();
    if (file.size() > 0) {
        const uint64_t reserve = std::min<uint64_t>(file.size() + 1, 256u << 20);
        data.reserve(static_cast<size_t>(reserve));
    }
    std::vector<uint8_t> buffer(1 << 20);
    for (;;) {
        size_t got = 0;
        if (!file.read(buffer.data(), buffer.size(), got, err)) return false;
        if (got == 0) break;
        data.append(reinterpret_cast<const char*>(buffer.data()), got);
    }
    // 去掉 UTF-8 BOM
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }
    return true;
}

// ==================================================== 目录与文件属性 ====
bool createDirectories(const std::wstring& path) {
    if (path.empty()) return false;
    if (isDirectory(path)) return true;
    std::wstring built;
    size_t i = 0;
    // 保留盘符（C:）或 UNC 前缀（\\server\share）
    if (path.size() >= 2 && path[1] == L':') {
        built = path.substr(0, 2);
        i = 2;
    } else if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        const size_t sep = path.find(L'\\', 2);
        if (sep == std::wstring::npos) return false;
        built = path.substr(0, sep);
        i = sep;
    }
    for (; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\' || path[i] == L'/') {
            if (!built.empty() && !isDirectory(built)) {
                if (!CreateDirectoryW(toExtendedPath(built).c_str(), nullptr)) {
                    const DWORD code = GetLastError();
                    if (code != ERROR_ALREADY_EXISTS) return false;
                }
            }
        }
        if (i < path.size()) built.push_back(path[i]);
    }
    return isDirectory(path);
}

bool setFileHidden(const std::wstring& path) {
    const std::wstring p = toExtendedPath(path);
    const DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return SetFileAttributesW(p.c_str(), attrs | FILE_ATTRIBUTE_HIDDEN) != 0;
}

bool isFileHidden(const std::wstring& path) {
    const std::wstring p = toExtendedPath(path);
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
}

bool isDirectoryWritable(const std::wstring& dir) {
    if (dir.empty() || !isDirectory(dir)) return false;
    wchar_t name[MAX_PATH] = {0};
    if (GetTempFileNameW(toExtendedPath(dir).c_str(), L"ewt", 0, name) == 0) return false;
    // GetTempFileNameW 会真正创建一个文件：确认能删除即视为可写
    return DeleteFileW(name) != 0;
}

std::wstring environmentVariable(const wchar_t* name) {
    if (name == nullptr) return std::wstring();
    const DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
    if (len == 0) return std::wstring();
    std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, buffer.data(), len + 1);
    if (written == 0 || written > len) return std::wstring();
    return std::wstring(buffer.data(), written);
}

std::wstring roamingAppDataDirectory() {
    std::wstring dir = environmentVariable(L"APPDATA");
    if (!dir.empty()) return dir;
    const std::wstring profile = environmentVariable(L"USERPROFILE");
    if (!profile.empty()) return joinPath(joinPath(profile, L"AppData"), L"Roaming");
    return std::wstring();
}

std::wstring tempDirectory() {
    std::wstring dir = environmentVariable(L"TEMP");
    if (!dir.empty()) return dir;
    dir = environmentVariable(L"TMP");
    if (!dir.empty()) return dir;
    std::vector<wchar_t> buffer(MAX_PATH + 1, L'\0');
    const DWORD len = GetTempPathW(MAX_PATH, buffer.data());
    if (len > 0 && len <= MAX_PATH) return std::wstring(buffer.data(), len);
    return std::wstring();
}

// ==================================================== 原生文件选择器 ====
std::vector<std::wstring> pickFiles(const std::wstring& title, bool multiple) {
    std::vector<std::wstring> result;
    if (!ensureCom()) return result;

    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dialog.put()));
    if (FAILED(hr)) return result;

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST;
        if (multiple) options |= FOS_ALLOWMULTISELECT;
        dialog->SetOptions(options);
    }
    if (!title.empty()) dialog->SetTitle(title.c_str());
    dialog->SetFileTypes(1, kAllFilesFilter);
    dialog->SetFileTypeIndex(1);

    hr = dialog->Show(reinterpret_cast<HWND>(GetConsoleWindow()));
    if (FAILED(hr)) return result;   // 包含用户取消 (ERROR_CANCELLED)

    if (multiple) {
        ComPtr<IShellItemArray> items;
        if (SUCCEEDED(dialog->GetResults(items.put())) && items) {
            DWORD count = 0;
            if (SUCCEEDED(items->GetCount(&count))) {
                for (DWORD i = 0; i < count; ++i) {
                    ComPtr<IShellItem> item;
                    if (SUCCEEDED(items->GetItemAt(i, item.put()))) {
                        const std::wstring path = shellItemPath(item.get());
                        if (!path.empty()) result.push_back(path);
                    }
                }
            }
        }
    } else {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(dialog->GetResult(item.put()))) {
            const std::wstring path = shellItemPath(item.get());
            if (!path.empty()) result.push_back(path);
        }
    }
    return result;
}

std::wstring pickFolder(const std::wstring& title) {
    std::wstring result;
    if (!ensureCom()) return result;

    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dialog.put()));
    if (FAILED(hr)) return result;

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_PICKFOLDERS;
        dialog->SetOptions(options);
    }
    if (!title.empty()) dialog->SetTitle(title.c_str());

    hr = dialog->Show(reinterpret_cast<HWND>(GetConsoleWindow()));
    if (FAILED(hr)) return result;

    ComPtr<IShellItem> item;
    if (SUCCEEDED(dialog->GetResult(item.put()))) result = shellItemPath(item.get());
    return result;
}

std::wstring pickSaveFile(const std::wstring& title, const std::wstring& defaultName,
                          const std::wstring& filterLabel, const std::wstring& filterSpec) {
    std::wstring result;
    if (!ensureCom()) return result;

    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dialog.put()));
    if (FAILED(hr)) return result;

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST;
        dialog->SetOptions(options);
    }
    if (!title.empty()) dialog->SetTitle(title.c_str());
    if (!defaultName.empty()) dialog->SetFileName(defaultName.c_str());

    std::vector<COMDLG_FILTERSPEC> specs;
    std::vector<std::wstring> storage;
    buildFilter(filterLabel, filterSpec, storage, specs);
    if (!specs.empty()) {
        dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        dialog->SetFileTypeIndex(1);
    }

    hr = dialog->Show(reinterpret_cast<HWND>(GetConsoleWindow()));
    if (FAILED(hr)) return result;

    ComPtr<IShellItem> item;
    if (SUCCEEDED(dialog->GetResult(item.put()))) result = shellItemPath(item.get());
    return result;
}

// ==================================================== Ctrl+C 处理 ====
namespace {
bool (*g_ctrlHandler)(void*) = nullptr;
void* g_ctrlContext = nullptr;

BOOL WINAPI consoleCtrlThunk(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_ctrlHandler && g_ctrlHandler(g_ctrlContext)) return TRUE;
            return FALSE;
        default:
            return FALSE;
    }
}
}  // namespace

void installConsoleCtrlHandler(bool (*handler)(void* ctx), void* ctx) {
    g_ctrlHandler = handler;
    g_ctrlContext = ctx;
    SetConsoleCtrlHandler(consoleCtrlThunk, TRUE);
}

}  // namespace ehsc
