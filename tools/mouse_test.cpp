// 鼠标点击测试器：启动 EHsc 的交互式菜单，读屏幕定位某个菜单项，注入真实鼠标左键点击，
// 再读一次屏幕作为证据。
//   用法: mouse_test <exe> <目标文本> [点击后等待毫秒] [截图输出路径]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Screen {
    int width = 0;
    int rows = 0;
    std::vector<std::wstring> lines;
};

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), need,
                        nullptr, nullptr);
    return out;
}

bool readScreen(Screen& screen) {
    HANDLE conout = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
    if (conout == INVALID_HANDLE_VALUE) return false;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    GetConsoleScreenBufferInfo(conout, &info);
    screen.width = info.dwSize.X;
    screen.rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    screen.lines.clear();
    for (int y = 0; y < screen.rows; ++y) {
        std::vector<wchar_t> buffer(static_cast<size_t>(screen.width) + 1, L'\0');
        DWORD read = 0;
        COORD pos{0, static_cast<SHORT>(y)};
        if (!ReadConsoleOutputCharacterW(conout, buffer.data(), static_cast<DWORD>(screen.width),
                                         pos, &read)) {
            screen.lines.emplace_back();
            continue;
        }
        std::wstring line(buffer.data(), read);
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\0')) line.pop_back();
        screen.lines.push_back(line);
    }
    CloseHandle(conout);
    return true;
}

// 把"字符下标"换算成"显示列"（CJK 全角占 2 列）——控制台报告的鼠标坐标是显示列
int displayWidth(const std::wstring& text) {
    int width = 0;
    for (wchar_t ch : text) {
        const bool wide = (ch >= 0x1100 && ch <= 0x115F) || ch == 0x2329 || ch == 0x232A ||
                          (ch >= 0x2E80 && ch <= 0xA4CF && ch != 0x303F) ||
                          (ch >= 0xAC00 && ch <= 0xD7A3) || (ch >= 0xF900 && ch <= 0xFAFF) ||
                          (ch >= 0xFE30 && ch <= 0xFE6F) || (ch >= 0xFF00 && ch <= 0xFF60) ||
                          (ch >= 0xFFE0 && ch <= 0xFFE6);
        width += wide ? 2 : 1;
    }
    return width;
}

int findRow(const Screen& screen, const std::wstring& needle, int& column) {
    for (int y = 0; y < static_cast<int>(screen.lines.size()); ++y) {
        const std::wstring& line = screen.lines[static_cast<size_t>(y)];
        const size_t at = line.find(needle);
        if (at != std::wstring::npos) {
            column = displayWidth(line.substr(0, at));   // 换算成显示列
            return y;
        }
    }
    return -1;
}

bool injectClick(SHORT column, SHORT row) {
    HANDLE conin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
    if (conin == INVALID_HANDLE_VALUE) return false;
    INPUT_RECORD records[2] = {};
    for (int i = 0; i < 2; ++i) {
        records[i].EventType = MOUSE_EVENT;
        records[i].Event.MouseEvent.dwMousePosition = COORD{column, row};
        records[i].Event.MouseEvent.dwButtonState =
            (i == 0) ? FROM_LEFT_1ST_BUTTON_PRESSED : 0;   // 先按下，再抬起
        records[i].Event.MouseEvent.dwEventFlags = 0;
    }
    DWORD written = 0;
    const BOOL ok = WriteConsoleInputW(conin, records, 2, &written);
    CloseHandle(conin);
    return ok != FALSE;
}

// 注入按键（用于对照：确认注入通道本身是通的）
bool injectKey(wchar_t ch) {
    HANDLE conin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
    if (conin == INVALID_HANDLE_VALUE) return false;
    INPUT_RECORD records[2] = {};
    for (int i = 0; i < 2; ++i) {
        records[i].EventType = KEY_EVENT;
        records[i].Event.KeyEvent.bKeyDown = (i == 0);
        records[i].Event.KeyEvent.wRepeatCount = 1;
        records[i].Event.KeyEvent.wVirtualKeyCode = (ch == L'\r') ? VK_RETURN : static_cast<WORD>(ch);
        records[i].Event.KeyEvent.uChar.UnicodeChar = ch;
    }
    DWORD written = 0;
    const BOOL ok = WriteConsoleInputW(conin, records, 2, &written);
    CloseHandle(conin);
    return ok != FALSE;
}

void dumpScreen(const Screen& screen, const wchar_t* path) {
    std::string text;
    for (const std::wstring& line : screen.lines) text += toUtf8(line) + "\r\n";
    HANDLE out = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(out);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: mouse_test <exe> <needle> [waitMs] [screenshotPath]\n");
        return 2;
    }
    const std::wstring exe = argv[1];
    const std::wstring needle = argv[2];
    const int waitMs = (argc >= 4) ? _wtoi(argv[3]) : 1500;
    const std::wstring shot = (argc >= 5) ? argv[4] : L"mouse_screen.txt";

    std::wstring command = L"\"" + exe + L"\"";
    for (int i = 5; i < argc; ++i) {   // 多余参数原样传给被测程序
        command += L" ";
        std::wstring arg = argv[i];
        if (arg.find(L' ') != std::wstring::npos) arg = L"\"" + arg + L"\"";
        command += arg;
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    std::printf("命令行: %s\n", toUtf8(command).c_str());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        nullptr, &si, &pi)) {
        std::printf("CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("pid=%lu 等待菜单渲染 %d ms\n", pi.dwProcessId, waitMs);
    Sleep(static_cast<DWORD>(waitMs));

    FreeConsole();
    if (!AttachConsole(pi.dwProcessId)) {
        std::printf("AttachConsole failed: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    Screen screen;
    if (!readScreen(screen)) {
        std::printf("读取屏幕失败\n");
        FreeConsole();
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    // 对照模式：needle 形如 "KEY:0" 时改为注入按键，用来验证注入通道本身是否可用
    if (needle.rfind(L"KEY:", 0) == 0) {
        const std::wstring keys = needle.substr(4);
        bool allOk = true;
        for (wchar_t ch : keys) {
            if (!injectKey(ch)) allOk = false;
            Sleep(200);
        }
        std::printf("注入按键 '%s' -> %s\n", toUtf8(keys).c_str(), allOk ? "ok" : "failed");
        FreeConsole();
        Sleep(2500);
        FreeConsole();
        if (AttachConsole(pi.dwProcessId)) {
            Screen after;
            if (readScreen(after)) dumpScreen(after, shot.c_str());
            FreeConsole();
        }
        if (WaitForSingleObject(pi.hProcess, 300) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 0;
    }

    // 支持多步点击：needle 用 | 分隔，例如 "9) 设置|5) 进度条"
    std::vector<std::wstring> targets;
    {
        std::wstring rest = needle;
        for (;;) {
            const size_t bar = rest.find(L'|');
            if (bar == std::wstring::npos) {
                targets.push_back(rest);
                break;
            }
            targets.push_back(rest.substr(0, bar));
            rest = rest.substr(bar + 1);
        }
    }

    bool allClicked = true;
    for (size_t step = 0; step < targets.size(); ++step) {
        Screen current;
        if (!readScreen(current)) {
            std::printf("第 %zu 步读取屏幕失败\n", step + 1);
            allClicked = false;
            break;
        }
        int column = 0;
        const int row = findRow(current, targets[step], column);
        std::printf("第 %zu 步：'%s' -> 行 %d, 列 %d\n", step + 1, toUtf8(targets[step]).c_str(), row,
                    column);
        if (row < 0) {
            dumpScreen(current, shot.c_str());
            allClicked = false;
            break;
        }
        {
            HANDLE conin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                       nullptr);
            DWORD mode = 0;
            if (conin != INVALID_HANDLE_VALUE && GetConsoleMode(conin, &mode)) {
                std::printf("  console input mode = 0x%08lX [MOUSE=%d QUICK_EDIT=%d]\n", mode,
                            (mode & ENABLE_MOUSE_INPUT) ? 1 : 0,
                            (mode & ENABLE_QUICK_EDIT_MODE) ? 1 : 0);
            }
            if (conin != INVALID_HANDLE_VALUE) CloseHandle(conin);
        }
        const bool ok = injectClick(static_cast<SHORT>(column), static_cast<SHORT>(row));
        std::printf("  注入点击 (%d,%d) -> %s\n", column, row, ok ? "ok" : "failed");
        if (!ok) allClicked = false;
        Sleep(1500);   // 等程序响应并重绘
    }
    std::printf("全部点击完成: %s\n", allClicked ? "是" : "否（见上面输出）");
    FreeConsole();

    Sleep(2500);   // 给程序时间响应并刷新界面

    FreeConsole();
    if (AttachConsole(pi.dwProcessId)) {
        Screen after;
        if (readScreen(after)) {
            dumpScreen(after, shot.c_str());
            std::printf("点击后屏幕已保存到 %s\n", toUtf8(shot).c_str());
        }
        FreeConsole();
    }

    if (WaitForSingleObject(pi.hProcess, 300) == WAIT_TIMEOUT) {
        std::printf("（程序仍在运行，测试结束前终止它）\n");
        TerminateProcess(pi.hProcess, 0);
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        std::printf("程序已自行退出，退出码 %lu\n", code);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}
