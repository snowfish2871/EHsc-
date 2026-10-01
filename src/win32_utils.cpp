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
#include <conio.h>      // _kbhit / _getch
#include <fcntl.h>      // _O_BINARY
#include <io.h>         // _isatty / _setmode
#include <cstdio>       // _fileno / swprintf_s

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <mutex>

namespace ehsc {
namespace {

UINT g_originalOutputCP = 0;
UINT g_originalInputCP = 0;
DWORD g_originalOutputMode = 0;
bool g_consoleSaved = false;

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
    // 让标准流以二进制方式工作，避免 CRT 再做一次编码转换
    (void)_setmode(_fileno(stdout), _O_BINARY);
    (void)_setmode(_fileno(stderr), _O_BINARY);
    SetConsoleTitleW(L"EHsc · 纯 C++ 文件哈希工具");
}

void restoreConsole() {
    if (!g_consoleSaved) return;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_originalOutputMode) SetConsoleMode(hOut, g_originalOutputMode);
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
int pollKey() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (!isConsoleHandle(hIn)) return -1;
    if (!_kbhit()) return -1;
    const int ch = _getch();
    if (ch == 0 || ch == 0xE0) {   // 功能键/方向键：吃掉后续字节
        if (_kbhit()) (void)_getch();
        return 0;
    }
    return ch;
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
    HANDLE handle = CreateFileW(extended.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        err = lastWinError() + L"（" + path + L"）";
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
