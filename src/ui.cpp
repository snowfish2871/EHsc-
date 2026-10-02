// ============================================================================
//  EHsc · ui.cpp
//  控制台界面实现：颜色、输入、实时进度面板（进度条 / 速度 / 平均速度 / ETA /
//  暂停 / 取消）、配置持久化、交互式主菜单。
// ============================================================================
#include "ui.h"
#include "win32_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <iostream>
#include <sstream>

namespace ehsc {
namespace {

std::atomic<bool> g_colors{true};
std::atomic<JobControl*> g_activeJob{nullptr};

// UTF-8 字符显示宽度（中日韩字符按 2 列计算，ANSI 转义序列计 0 列）
size_t utf8DisplayWidth(const std::string& text) {
    size_t width = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0x1B) {   // 跳过 CSI 转义序列
            size_t j = i + 1;
            if (j < text.size() && text[j] == '[') {
                ++j;
                while (j < text.size() && !(text[j] >= '@' && text[j] <= '~')) ++j;
                if (j < text.size()) ++j;
            }
            i = j;
            continue;
        }
        uint32_t code = 0;
        int len = 1;
        if (c < 0x80) {
            code = c;
        } else if ((c >> 5) == 0x6) {
            code = c & 0x1Fu;
            len = 2;
        } else if ((c >> 4) == 0xE) {
            code = c & 0x0Fu;
            len = 3;
        } else if ((c >> 3) == 0x1E) {
            code = c & 0x07u;
            len = 4;
        } else {
            ++i;
            continue;
        }
        for (int k = 1; k < len && i + static_cast<size_t>(k) < text.size(); ++k)
            code = (code << 6) | (static_cast<unsigned char>(text[i + static_cast<size_t>(k)]) & 0x3Fu);
        i += static_cast<size_t>(len);
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

// 超宽时保留尾部（路径尾部信息量更大）
std::string truncateMiddle(const std::string& text, size_t maxWidth) {
    if (maxWidth == 0) return std::string();
    if (utf8DisplayWidth(text) <= maxWidth) return text;
    std::string suffix;
    size_t width = 0;
    for (size_t i = text.size(); i > 0;) {
        // 从后往前找一个 UTF-8 字符边界
        size_t start = i - 1;
        while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
        const std::string piece = text.substr(start, i - start);
        const size_t pieceWidth = utf8DisplayWidth(piece);
        if (width + pieceWidth > maxWidth - 1) break;
        suffix = piece + suffix;
        width += pieceWidth;
        i = start;
    }
    return "…" + suffix;
}

// 按显示宽度安全截断（保留完整 UTF-8 字符，末尾补颜色复位）
std::string truncateToWidth(const std::string& text, size_t maxWidth) {
    if (utf8DisplayWidth(text) <= maxWidth) return text;
    std::string out;
    size_t width = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0x1B) {
            size_t j = i + 1;
            if (j < text.size() && text[j] == '[') {
                ++j;
                while (j < text.size() && !(text[j] >= '@' && text[j] <= '~')) ++j;
                if (j < text.size()) ++j;
            }
            out.append(text, i, j - i);
            i = j;
            continue;
        }
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (i + len > text.size()) len = text.size() - i;
        const std::string piece = text.substr(i, len);
        const size_t pieceWidth = utf8DisplayWidth(piece);
        if (width + pieceWidth > maxWidth) break;
        out += piece;
        width += pieceWidth;
        i += len;
    }
    out += "\x1b[0m";
    return out;
}

std::string renderBar(double fraction, int width, bool ascii) {
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    int filled = static_cast<int>(std::lround(fraction * static_cast<double>(width)));
    if (filled > width) filled = width;
    std::string bar = "[";
    if (ascii) {
        bar.append(static_cast<size_t>(filled), '=');
        if (filled < width) {
            bar.push_back('>');
            bar.append(static_cast<size_t>(width - filled - 1), ' ');
        }
    } else {
        for (int i = 0; i < filled; ++i) bar += "█";
        for (int i = filled; i < width; ++i) bar += "░";
    }
    bar += "]";
    return bar;
}

std::string percentText(double fraction) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%6.2f%%", fraction * 100.0);
    return buffer;
}

}  // namespace

// ============================================================ 控制台输出 ====
void enableColors(bool enabled) { g_colors.store(enabled); }
bool colorsEnabled() { return g_colors.load() && stdoutIsTerminal(); }

void setColor(Color color) {
    if (!colorsEnabled()) return;
    const char* code = "\x1b[0m";
    switch (color) {
        case Color::Default: code = "\x1b[0m"; break;
        case Color::Bold: code = "\x1b[1m"; break;
        case Color::Dim: code = "\x1b[2m"; break;
        case Color::Red: code = "\x1b[31m"; break;
        case Color::Green: code = "\x1b[32m"; break;
        case Color::Yellow: code = "\x1b[33m"; break;
        case Color::Blue: code = "\x1b[34m"; break;
        case Color::Magenta: code = "\x1b[35m"; break;
        case Color::Cyan: code = "\x1b[36m"; break;
        case Color::Gray: code = "\x1b[90m"; break;
    }
    std::cout << code;
}

void resetColor() {
    if (!colorsEnabled()) return;
    std::cout << "\x1b[0m";
}

std::string paint(const std::string& text, Color color) {
    if (!colorsEnabled()) return text;
    std::string out;
    switch (color) {
        case Color::Default: out = "\x1b[0m"; break;
        case Color::Bold: out = "\x1b[1m"; break;
        case Color::Dim: out = "\x1b[2m"; break;
        case Color::Red: out = "\x1b[31m"; break;
        case Color::Green: out = "\x1b[32m"; break;
        case Color::Yellow: out = "\x1b[33m"; break;
        case Color::Blue: out = "\x1b[34m"; break;
        case Color::Magenta: out = "\x1b[35m"; break;
        case Color::Cyan: out = "\x1b[36m"; break;
        case Color::Gray: out = "\x1b[90m"; break;
    }
    return out + text + "\x1b[0m";
}

void showBanner() {
    const std::string line(76, '=');
    std::cout << "\n" << paint(line, Color::Cyan) << "\n";
    std::cout << "  " << paint("EHsc", Color::Bold)
              << " · 纯 C++ 文件哈希与校验工具   版本 " << versionString() << "\n";
    std::cout << "  " << paint("零第三方依赖", Color::Green)
              << "：CRC32 / MD5 / SHA-1 / SHA-256 / SHA-384 / SHA-512 /\n";
    std::cout << "              SHA3-256 / SHA3-512 / BLAKE2s-256 / BLAKE2b-512\n";
    std::cout << paint(line, Color::Cyan) << "\n";
}

void printSectionTitle(const std::string& title) {
    std::cout << "\n" << paint("── " + title + " ", Color::Cyan)
              << paint(std::string(60 > title.size() ? 60 - title.size() : 4, '-'), Color::Gray)
              << "\n";
}

void printInfo(const std::string& text) { std::cout << paint("· ", Color::Gray) << text << "\n"; }
void printSuccess(const std::string& text) {
    std::cout << paint("✔ ", Color::Green) << text << "\n";
}
void printWarning(const std::string& text) {
    std::cout << paint("! ", Color::Yellow) << text << "\n";
}
void printError(const std::string& text) { std::cout << paint("✘ ", Color::Red) << text << "\n"; }

// ============================================================ 交互输入 ====
bool readLineUtf8(const std::string& prompt, std::string& line) {
    std::cout << prompt << std::flush;
    if (!std::getline(std::cin, line)) {
        line.clear();
        return false;
    }
    // 容忍从文件/管道重定向输入时可能出现的 UTF-8 BOM
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
    line = trimText(line);
    return true;
}

int askInt(const std::string& prompt, int low, int high, int defaultValue) {
    std::string line;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!readLineUtf8(prompt, line)) return defaultValue;
        if (line.empty()) return defaultValue;
        char* end = nullptr;
        const long value = std::strtol(line.c_str(), &end, 10);
        if (end != line.c_str() && value >= low && value <= high) return static_cast<int>(value);
        printWarning("输入无效，请输入 " + std::to_string(low) + " ~ " + std::to_string(high) +
                     " 之间的整数");
    }
    return defaultValue;
}

namespace {
// 把用户输入解析为"是/否"，无法识别时返回默认值
bool interpretYesNo(const std::string& text, bool defaultValue) {
    std::string lower = text;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    while (!lower.empty() && (lower.front() == ' ' || lower.front() == '\t')) lower.erase(lower.begin());
    while (!lower.empty() &&
           (lower.back() == ' ' || lower.back() == '\r' || lower.back() == '\n' || lower.back() == '\t')) {
        lower.pop_back();
    }
    if (lower.empty()) return defaultValue;
    if (lower == "y" || lower == "yes" || lower == "1" || lower == "是" || lower == "好") return true;
    if (lower == "n" || lower == "no" || lower == "0" || lower == "否" || lower == "不") return false;
    return defaultValue;
}

namespace {
// 调试用：把点击相关的诊断写到文件（不能用 stderr，否则调试输出本身会占用控制台行、
// 让行号发生偏移，反而干扰诊断）
void clickDebugLog(const std::string& text) {
    const char* path = std::getenv("EHSC_DEBUG_CLICK");
    if (path == nullptr || *path == '\0') return;
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    out << text << "\n";
}
}  // namespace
// ------------------------------------------------------- 可点击区域 ----
struct ClickRegion {
    int row = 0;
    int colBegin = -1;   // < 0 表示整行可点
    int colEnd = -1;
    int value = 0;
    std::string label;   // 整行登记 = 该行可见文本；span 登记 = 屏幕上的原文片段
};

std::mutex g_clickMutex;
std::vector<ClickRegion> g_clickRegions;
constexpr int kNoClickValue = -1000000;

// 去掉左侧空白，便于按内容匹配
std::string trimLeft(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    return text.substr(begin);
}

// 取"条目关键字"：标题部分（第一个连续两个空格之前的内容）。
// 这样条目后面的当前值变了（例如 进度条 开 → 关）仍然匹配得上。
std::string clickLabelKey(const std::string& label) {
    const std::string text = trimLeft(label);
    const size_t gap = text.find("  ");
    return gap == std::string::npos ? text : text.substr(0, gap);
}

int lookupClick(int row, int column) {
    // 先读出被点击那一行"实际显示的内容"：控制台缓冲区可能已经滚动，
    // 只靠登记时的行号会点错条目（行号会全部挤在缓冲区最后一行），所以以屏幕文字为准。
    const std::string line = consoleLineText(row);
    const std::string trimmed = trimLeft(line);
    clickDebugLog("[dbg] lookup row=" + std::to_string(row) + " col=" + std::to_string(column) +
                  " line=[" + trimmed + "]");

    std::lock_guard<std::mutex> lock(g_clickMutex);
    if (!trimmed.empty()) {
        // ① 整行条目：该行以条目关键字开头
        for (const ClickRegion& region : g_clickRegions) {
            if (region.colBegin >= 0 || region.label.empty()) continue;
            const std::string key = clickLabelKey(region.label);
            if (!key.empty() && trimmed.rfind(key, 0) == 0) {
                clickDebugLog("[dbg]   matched row-region value=" + std::to_string(region.value));
                return region.value;
            }
        }
        // ② 行内片段：点击列落在这个片段上
        for (const ClickRegion& region : g_clickRegions) {
            if (region.colBegin < 0 || region.label.empty()) continue;
            const size_t at = line.find(region.label);
            if (at == std::string::npos) continue;
            const int begin = static_cast<int>(textDisplayWidth(line.substr(0, at)));
            const int end = begin + static_cast<int>(textDisplayWidth(region.label)) - 1;
            if (column >= begin && column <= end) return region.value;
        }
    }
    // ③ 兜底：按登记时的行号/列范围（屏幕文字读不到时）
    for (const ClickRegion& region : g_clickRegions) {
        if (region.row != row || region.colBegin < 0) continue;
        if (column >= region.colBegin && column <= region.colEnd) return region.value;
    }
    for (const ClickRegion& region : g_clickRegions) {
        if (region.row == row && region.colBegin < 0) return region.value;
    }
    return kNoClickValue;
}

// 在提示文本里找到 token 所在的列范围（用于把 "Y" / "N" 变成可点击）
bool findTokenColumns(const std::string& text, const std::string& token, int startColumn,
                      int& colBegin, int& colEnd) {
    const size_t pos = text.find(token);
    if (pos == std::string::npos) return false;
    colBegin = startColumn + static_cast<int>(textDisplayWidth(text.substr(0, pos)));
    colEnd = colBegin + static_cast<int>(textDisplayWidth(token)) - 1;
    return true;
}
}  // namespace

void clickableClear() {
    std::lock_guard<std::mutex> lock(g_clickMutex);
    g_clickRegions.clear();
}

int clickableRow() { return consoleCursorRow(); }
int clickableColumn() { return consoleCursorColumn(); }


void clickableAddRow(int row, int value, const std::string& label) {
    if (row < 0) return;
    clickDebugLog("[dbg] register row=" + std::to_string(row) + " value=" + std::to_string(value) +
                  " label=" + trimLeft(label));
    std::lock_guard<std::mutex> lock(g_clickMutex);
    g_clickRegions.push_back(ClickRegion{row, -1, -1, value, label});
}

void clickableAddSpan(int row, int colBegin, int colEnd, int value, const std::string& token) {
    if (row < 0 || colEnd < colBegin) return;
    clickDebugLog("[dbg] register span row=" + std::to_string(row) + " value=" +
                  std::to_string(value) + " token=" + token);
    std::lock_guard<std::mutex> lock(g_clickMutex);
    g_clickRegions.push_back(ClickRegion{row, colBegin, colEnd, value, token});
}

bool clickableAvailable() { return consoleMouseAvailable(); }

void printClickHint() {
    if (!clickableAvailable()) return;
    std::cout << paint("  （提示：可以直接用鼠标点击上面的条目选择）", Color::Gray) << "\n";
}

int askMenuChoice(const std::string& prompt, int low, int high, int defaultValue) {
    if (!prompt.empty()) std::cout << prompt << std::flush;
    if (!clickableAvailable()) {
        // 没有鼠标支持时退回原来的行输入行为（脚本/管道走这条路径），
        // 输入非法就再问一次；读到 EOF 时按默认值返回，不会卡住调用方。
        for (;;) {
            std::string line;
            if (!readLineUtf8("", line)) return defaultValue;
            if (line.empty()) return defaultValue;
            const int value = std::atoi(line.c_str());
            if (value >= low && value <= high) return value;
            std::cout << paint("  输入无效，请重新选择: ", Color::Yellow) << std::flush;
        }
    }

    std::string typed;
    for (;;) {
        const ConsoleEvent event = pollConsoleEvent();
        if (event.kind == ConsoleEventKind::Key) {
            const int key = event.key;
            if (key == '\r' || key == '\n') {
                std::cout << "\n";
                if (typed.empty()) return defaultValue;
                const int value = std::atoi(typed.c_str());
                if (value >= low && value <= high) return value;
                std::cout << paint("  输入无效，请重新选择: ", Color::Yellow) << std::flush;
                typed.clear();
                continue;
            }
            if (key == 27) {   // Esc = 返回上一层
                std::cout << "\n";
                return defaultValue;
            }
            if (key == 8 || key == 127) {
                if (!typed.empty()) {
                    typed.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
                continue;
            }
            if (key >= '0' && key <= '9') {
                typed.push_back(static_cast<char>(key));
                std::cout << static_cast<char>(key) << std::flush;
            }
            continue;
        }
        if (event.kind == ConsoleEventKind::MouseClick) {
            const int value = lookupClick(event.row, event.column);
            clickDebugLog("[dbg] click row=" + std::to_string(event.row) + " col=" +
                          std::to_string(event.column) + " -> value=" + std::to_string(value));
            if (value != kNoClickValue) {
                std::cout << value << "\n";
                return value;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

int askOptionRow(const std::string& title,
                 const std::vector<std::pair<int, std::string>>& options, int defaultValue) {
    if (options.empty()) return defaultValue;

    clickableClear();
    const int row = clickableRow();
    const int startColumn = clickableColumn();
    if (row < 0 || startColumn < 0) {   // 没有控制台：退回普通提问
        std::string line = title + "  ";
        for (const auto& option : options) {
            line += std::to_string(option.first) + ") " + option.second + "  ";
        }
        const int value = askInt(line + ": ", options.front().first, options.back().first,
                                 defaultValue);
        return value;
    }

    std::string line = title + "  ";
    std::vector<std::pair<int, int>> spans;
    spans.reserve(options.size());
    for (const auto& option : options) {
        const std::string text = std::to_string(option.first) + ") " + option.second;
        const int begin = startColumn + static_cast<int>(textDisplayWidth(line));
        line += text;
        const int end = startColumn + static_cast<int>(textDisplayWidth(line)) - 1;
        spans.emplace_back(begin, end);
        line += "   ";
    }
    for (size_t i = 0; i < options.size(); ++i) {
        clickableAddSpan(row, spans[i].first, spans[i].second, options[i].first,
                         std::to_string(options[i].first) + ") " + options[i].second);
    }
    if (clickableAvailable()) line += "（可点击）";
    const int low = options.front().first;
    const int high = options.back().first;
    return askMenuChoice(line + ": ", low, high, defaultValue);
}

bool askYesNo(const std::string& prompt, bool defaultValue) {
    std::string line;
    if (!readLineUtf8(prompt, line)) return defaultValue;
    return interpretYesNo(line, defaultValue);
}

bool askYesNoTimed(const std::string& prompt, bool defaultValue, int timeoutSeconds,
                   bool* timedOut) {
    if (timedOut != nullptr) *timedOut = false;
    // 非控制台（管道/文件）不在键盘上等：要么数据已经就绪，要么根本没人会回答
    if (!stdinIsConsole()) return askYesNo(prompt, defaultValue);

    // 把 "(Y/N" 里的 Y 和 N 变成可点击区域
    clickableClear();
    const int startColumn = clickableColumn();
    int yesBegin = 0, yesEnd = 0, noBegin = 0, noEnd = 0;
    const bool yesClickable = findTokenColumns(prompt, "(Y", startColumn, yesBegin, yesEnd);
    const bool noClickable = findTokenColumns(prompt, "/N", startColumn, noBegin, noEnd);

    std::cout << prompt << std::flush;
    if (yesClickable) clickableAddSpan(clickableRow(), yesBegin + 1, yesEnd, 1, "(Y");
    if (noClickable) clickableAddSpan(clickableRow(), noBegin + 1, noEnd, 0, "/N");

    std::string typed;
    const auto timeoutMs = std::chrono::milliseconds(timeoutSeconds > 0 ? timeoutSeconds * 1000 : 0);
    const auto deadline = std::chrono::steady_clock::now() + timeoutMs;
    while (true) {
        const ConsoleEvent event = pollConsoleEvent();
        if (event.kind == ConsoleEventKind::MouseClick) {
            const int value = lookupClick(event.row, event.column);
            if (value == 1 || value == 0) {
                std::cout << (value == 1 ? "Y" : "N") << "\n";
                return value == 1;
            }
        }
        const int key = event.kind == ConsoleEventKind::Key ? event.key : -1;
        if (key == '\r' || key == '\n') {
            std::cout << "\n";
            return interpretYesNo(typed, defaultValue);
        }
        if (key == 27) {   // Esc：放弃提权
            std::cout << "\n";
            return false;
        }
        if (key == 8 || key == 127) {   // 退格
            if (!typed.empty()) {
                typed.pop_back();
                std::cout << "\b \b" << std::flush;
            }
        } else if (key >= 32 && key < 127) {
            typed.push_back(static_cast<char>(key));
            std::cout << static_cast<char>(key) << std::flush;   // ReadConsoleInput 不回显，手动回显
        }
        if (timeoutMs.count() > 0 && std::chrono::steady_clock::now() >= deadline) {
            if (timedOut != nullptr) *timedOut = true;
            std::cout << "\n";
            return defaultValue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void pauseForEnter(const std::string& hint) {
    std::cout << "\n" << paint(hint, Color::Gray) << std::flush;

    // 支持鼠标：点击提示行本身也可继续（非交互式输入仍走原来的读行逻辑，脚本不受影响）
    if (!clickableAvailable()) {
        std::string line;
        std::getline(std::cin, line);
        return;
    }
    clickableClear();
    clickableAddRow(clickableRow(), 1, hint);
    for (;;) {
        const ConsoleEvent event = pollConsoleEvent();
        if (event.kind == ConsoleEventKind::Key) {
            if (event.key == '\r' || event.key == '\n' || event.key == 27 || event.key == ' ') break;
        } else if (event.kind == ConsoleEventKind::MouseClick) {
            if (lookupClick(event.row, event.column) != kNoClickValue) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    clickableClear();
    std::cout << "\n";
}

std::vector<std::wstring> askTargets(const std::string& prompt, bool multiple, bool folderMode) {
    std::vector<std::wstring> targets;
    std::string line;
    if (!readLineUtf8(prompt, line)) return targets;
    if (line.empty()) {
        // 空输入 → 调用 Windows 原生选择器
        if (folderMode) {
            const std::wstring folder = pickFolder(L"请选择文件夹");
            if (!folder.empty()) targets.push_back(folder);
        } else {
            targets = pickFiles(L"请选择文件", multiple);
        }
        return targets;
    }
    // 分号分隔的多个路径
    std::wstring wide = utf8ToWide(line);
    std::wstring current;
    for (wchar_t c : wide) {
        if (c == L';' || c == L'|') {
            const std::wstring item = stripQuotes(current);
            if (!item.empty()) targets.push_back(item);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    const std::wstring item = stripQuotes(current);
    if (!item.empty()) targets.push_back(item);
    return targets;
}

std::wstring askSavePath(const std::string& prompt, const std::wstring& defaultName,
                         const std::wstring& filterLabel, const std::wstring& filterSpec) {
    std::string line;
    if (!readLineUtf8(prompt, line)) return std::wstring();
    if (line.empty()) return pickSaveFile(L"保存结果", defaultName, filterLabel, filterSpec);
    return stripQuotes(utf8ToWide(line));
}

// ============================================================ 配置持久化 ====
std::vector<std::wstring> parseFilters(const std::wstring& text) {
    std::vector<std::wstring> filters;
    std::wstring current;
    for (wchar_t c : text) {
        if (c == L';' || c == L',' || c == L'|') {
            const std::wstring item = stripQuotes(current);
            if (!item.empty()) filters.push_back(item);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    const std::wstring item = stripQuotes(current);
    if (!item.empty()) filters.push_back(item);
    return filters;
}

size_t bufferBytes(const Config& cfg) {
    size_t mb = cfg.bufferMB;
    if (mb < 1) mb = 1;
    if (mb > 64) mb = 64;   // 每线程读取缓冲上限，避免超大缓冲导致内存暴涨
    return mb << 20;
}

namespace {

// 配置文件按顺序尝试的位置：
//   1) 可执行文件所在目录（绿色/便携模式，首选）
//   2) %APPDATA%\EHsc\ehsc.ini（exe 目录只读时，例如放在只读共享或光盘上）
//   3) %TEMP%\EHsc\ehsc.ini（最后的兜底）
// 这样即使把 EHsc.exe 单独拷到任意目录（甚至只读介质），程序依然可以正常保存设置。
std::wstring g_activeConfigPath;

const std::vector<std::wstring>& configCandidates() {
    static const std::vector<std::wstring> list = [] {
        std::vector<std::wstring> out;
        const std::wstring exeDir = executableDirectory();
        if (!exeDir.empty()) out.push_back(joinPath(exeDir, L"ehsc.ini"));
        const std::wstring roaming = roamingAppDataDirectory();
        if (!roaming.empty()) out.push_back(joinPath(joinPath(roaming, L"EHsc"), L"ehsc.ini"));
        const std::wstring temp = tempDirectory();
        if (!temp.empty()) out.push_back(joinPath(joinPath(temp, L"EHsc"), L"ehsc.ini"));
        if (out.empty()) out.push_back(L"ehsc.ini");
        return out;
    }();
    return list;
}

// 配置文件文本：第一行是彩蛋，其余为可读可改的键值对
std::string buildConfigText(const Config& cfg) {
    std::string text;
    // ——— 彩蛋（第一行）———
    text += "# 被你找到了喵 (=^･ω･^=)\n";
    text += "# EHsc 配置文件（UTF-8）· 这个文件被设成了隐藏属性，删掉也不影响使用，程序会重新生成。\n";
    text += "# 可以直接编辑下面的键值对；也可以在主菜单「8) 设置」里修改，程序会自动保存到这里。\n";
    text += "[general]\n";
    text += "algos=" + algoIdList(cfg.algos) + "\n";
    text += "threads=" + std::to_string(cfg.threads) + "\n";
    text += "bufferMB=" + std::to_string(cfg.bufferMB) + "\n";
    text += "format=" + std::string(formatName(cfg.format)) + "\n";
    text += std::string("progress=") + (cfg.progress ? "1" : "0") + "\n";
    text += std::string("ascii=") + (cfg.asciiBar ? "1" : "0") + "\n";
    text += std::string("overlap=") + (cfg.overlap ? "1" : "0") + "\n";
    text += std::string("recursive=") + (cfg.recursive ? "1" : "0") + "\n";
    text += std::string("hidden=") + (cfg.includeHidden ? "1" : "0") + "\n";
    text += std::string("checksize=") + (cfg.verifyCheckSize ? "1" : "0") + "\n";
    text += std::string("bom=") + (cfg.bom ? "1" : "0") + "\n";
    text += std::string("colors=") + (cfg.colors ? "1" : "0") + "\n";
    text += "filters=" + wideToUtf8(cfg.filters) + "\n";
    text += "lastdir=" + wideToUtf8(cfg.lastDirectory) + "\n";
    return text;
}

}  // namespace

std::wstring configFilePath() {
    if (!g_activeConfigPath.empty()) return g_activeConfigPath;
    return configCandidates().front();
}

bool configFileExists() { return pathExists(configFilePath()); }

bool configFileIsHidden() { return isFileHidden(configFilePath()); }

std::wstring configSearchDescription() {
    std::string out;
    for (size_t i = 0; i < configCandidates().size(); ++i) {
        if (i) out += "  →  ";
        out += wideToUtf8(configCandidates()[i]);
    }
    return utf8ToWide(out);
}

bool loadConfig(Config& cfg) {
    for (const std::wstring& path : configCandidates()) {
        std::string text;
        std::wstring err;
        if (!readFileUtf8(path, text, err)) continue;
        g_activeConfigPath = path;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            line = trimText(line);
            if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = trimText(line.substr(0, eq));
            const std::string value = trimText(line.substr(eq + 1));
            if (key == "algos") {
                std::string bad;
                const std::vector<Algo> parsed = parseAlgoList(value, &bad);
                if (!parsed.empty()) cfg.algos = parsed;
            } else if (key == "threads") {
                cfg.threads = std::atoi(value.c_str());
            } else if (key == "bufferMB") {
                const int mb = std::atoi(value.c_str());
                if (mb >= 1 && mb <= 64) cfg.bufferMB = static_cast<size_t>(mb);
            } else if (key == "format") {
                OutFormat format{};
                if (parseFormat(value, format)) cfg.format = format;
            } else if (key == "progress") {
                cfg.progress = (value != "0");
            } else if (key == "ascii") {
                cfg.asciiBar = (value != "0");
            } else if (key == "overlap") {
                cfg.overlap = (value != "0");
            } else if (key == "recursive") {
                cfg.recursive = (value != "0");
            } else if (key == "hidden") {
                cfg.includeHidden = (value != "0");
            } else if (key == "checksize") {
                cfg.verifyCheckSize = (value != "0");
            } else if (key == "bom") {
                cfg.bom = (value != "0");
            } else if (key == "colors") {
                cfg.colors = (value != "0");
            } else if (key == "filters") {
                cfg.filters = utf8ToWide(value);
            } else if (key == "lastdir") {
                cfg.lastDirectory = utf8ToWide(value);
            }
        }
        return true;
    }
    return false;
}

bool saveConfig(const Config& cfg) {
    const std::string text = buildConfigText(cfg);
    for (const std::wstring& path : configCandidates()) {
        const std::wstring dir = parentDir(path);
        if (dir.empty()) continue;
        if (!pathExists(dir) && !createDirectories(dir)) continue;
        if (!isDirectoryWritable(dir)) continue;
        std::wstring err;
        if (!writeFileUtf8(path, text, false, err)) continue;
        setFileHidden(path);          // 隐藏配置文件（不影响读写，只是不在资源管理器里显示）
        g_activeConfigPath = path;
        return true;
    }
    return false;
}

bool ensureConfigExists(const Config& cfg) {
    if (pathExists(configFilePath())) return true;
    return saveConfig(cfg);
}

// ============================================================ 进度监视器 ====
ProgressMonitor::ProgressMonitor(JobControl& control, bool asciiBar, bool enabled, bool force)
    : control_(control),
      ascii_(asciiBar),
      enabled_(enabled && (force || stdoutIsTerminal())),
      interactive_(stdoutIsTerminal()) {}

ProgressMonitor::~ProgressMonitor() { stop(); }

void ProgressMonitor::start(const std::string& title) {
    if (!enabled_) return;
    if (running_.load()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        title_ = title;
        logs_.clear();
    }
    running_.store(true);
    thread_ = std::thread([this] { loop(); });
}

void ProgressMonitor::stop() {
    if (!running_.exchange(false)) return;
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    clearPanel();
    flushLogs();
    std::cout << std::flush;
}

void ProgressMonitor::log(const std::string& line) {
    if (!enabled_ || !running_.load()) {
        std::cout << line << "\n" << std::flush;
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    logs_.push_back(line);
}

void ProgressMonitor::setHint(const std::string& hint) {
    std::lock_guard<std::mutex> lock(mutex_);
    hint_ = hint;
}

void ProgressMonitor::flushLogs() {
    std::deque<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(logs_);
    }
    if (pending.empty()) return;
    clearPanel();
    for (const std::string& line : pending) std::cout << line << "\n";
    std::cout << std::flush;
}

void ProgressMonitor::clearPanel() {
    if (linesDrawn_ <= 0) return;
    std::cout << "\x1b[" << linesDrawn_ << "A\x1b[0J" << std::flush;
    linesDrawn_ = 0;
}

void ProgressMonitor::loop() {
    constexpr int kClickTogglePause = -2001;
    constexpr int kClickCancel = -2002;
    auto lastDraw = std::chrono::steady_clock::now();
    while (running_.load()) {
        const ConsoleEvent event = pollConsoleEvent();
        int key = -1;
        if (event.kind == ConsoleEventKind::Key) {
            key = event.key;
        } else if (event.kind == ConsoleEventKind::MouseClick) {
            // 点击面板上的 [空格] / [Q] 区域等同于按对应按键
            const int hit = lookupClick(event.row, event.column);
            if (hit == kClickTogglePause) key = ' ';
            else if (hit == kClickCancel) key = 'q';
        }
        if (key == ' ') {
            control_.togglePause();
        } else if (key == 'q' || key == 'Q' || key == 27) {
            if (!control_.cancelled()) {
                control_.requestCancel();
                std::lock_guard<std::mutex> lock(mutex_);
                logs_.push_back(paint("已请求取消，正在安全停止…", Color::Yellow));
            }
        }
        flushLogs();
        const auto now = std::chrono::steady_clock::now();
        // 终端面板 10 Hz 刷新；重定向时每秒输出一行纯文本进度
        const double interval = interactive_ ? 0.1 : 1.0;
        if (std::chrono::duration<double>(now - lastDraw).count() >= interval) {
            lastDraw = now;
            ProgressSnapshot snapshot = control_.snapshot();
            if (interactive_) {
                drawPanel(snapshot);
            } else {
                drawStatusLine(snapshot);
            }
        }
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(25),
                       [this] { return !running_.load(); });
    }
}

// 非终端场景：每秒输出一行纯文本进度，便于写入日志文件
void ProgressMonitor::drawStatusLine(ProgressSnapshot& snapshot) {
    double fraction = 0;
    if (snapshot.bytesTotal > 0) {
        fraction = static_cast<double>(snapshot.bytesDone) / static_cast<double>(snapshot.bytesTotal);
    } else if (snapshot.filesTotal > 0) {
        fraction = static_cast<double>(snapshot.filesDone) / static_cast<double>(snapshot.filesTotal);
    }
    fraction = std::max(0.0, std::min(1.0, fraction));

    std::string title;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        title = title_;
    }
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%.2f%%", fraction * 100.0);
    std::cout << "[进度] " << title << " · " << percent << " · " << formatByteSize(snapshot.bytesDone)
              << " / " << formatByteSize(snapshot.bytesTotal) << " · " << snapshot.filesDone << "/"
              << snapshot.filesTotal << " 个文件 · 速度 " << formatSpeed(snapshot.speedNow)
              << " · 平均 " << formatSpeed(snapshot.speedAvg) << " · 已用 "
              << formatDuration(snapshot.elapsed) << " · 剩余 "
              << (snapshot.eta >= 0 ? formatDuration(snapshot.eta) : std::string("--:--"))
              << (snapshot.paused ? " · 已暂停" : "") << "\n"
              << std::flush;
}

void ProgressMonitor::drawPanel(ProgressSnapshot& snapshot) {
    const int columns = consoleColumns();
    const size_t usable = static_cast<size_t>(columns > 20 ? columns - 2 : 78);
    const int barWidth = static_cast<int>(std::max<size_t>(16, std::min<size_t>(44, usable - 30)));

    double fraction = 0;
    if (snapshot.bytesTotal > 0) {
        fraction = static_cast<double>(snapshot.bytesDone) / static_cast<double>(snapshot.bytesTotal);
    } else if (snapshot.filesTotal > 0) {
        fraction = static_cast<double>(snapshot.filesDone) / static_cast<double>(snapshot.filesTotal);
    }
    fraction = std::max(0.0, std::min(1.0, fraction));

    std::string title;
    std::string hint;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        title = title_;
        hint = hint_;
    }

    // 第 1 行：阶段 + 计数
    std::ostringstream head;
    head << " " << (snapshot.paused ? paint("已暂停", Color::Yellow) : paint(title, Color::Bold));
    head << paint("  ·  文件 ", Color::Gray) << snapshot.filesDone << "/" << snapshot.filesTotal;
    if (snapshot.filesFailed > 0)
        head << paint("  ·  失败 ", Color::Red) << snapshot.filesFailed;
    head << paint("  ·  速度实时统计", Color::Gray);

    // 第 2 行：当前文件
    std::ostringstream fileLine;
    fileLine << paint(" 文件  ", Color::Gray)
             << truncateMiddle(wideToUtf8(snapshot.currentFile), usable > 8 ? usable - 8 : 40);

    // 第 3 行：进度条 + 百分比 + 字节
    std::ostringstream progressLine;
    progressLine << " " << paint(renderBar(fraction, barWidth, ascii_), Color::Cyan) << " "
                 << paint(percentText(fraction), Color::Bold) << "  "
                 << formatByteSize(snapshot.bytesDone) << " / " << formatByteSize(snapshot.bytesTotal);

    // 第 4 行：速度 / 平均速度 / 已用 / 剩余
    std::ostringstream speedLine;
    speedLine << paint(" 速度 ", Color::Gray) << paint(formatSpeed(snapshot.speedNow), Color::Green)
              << paint("  ·  平均 ", Color::Gray) << formatSpeed(snapshot.speedAvg)
              << paint("  ·  已用 ", Color::Gray) << formatDuration(snapshot.elapsed)
              << paint("  ·  剩余 ", Color::Gray)
              << (snapshot.eta >= 0 ? formatDuration(snapshot.eta) : std::string("--:--"));

    // 第 5 行：操作提示（[空格] 与 [Q] 同时也是可点击区域）
    constexpr int kClickTogglePause = -2001;
    constexpr int kClickCancel = -2002;
    struct HintSegment {
        std::string text;
        Color       color;
        int         clickValue;
    };
    std::vector<HintSegment> hintSegments;
    if (snapshot.cancelled) {
        hintSegments.push_back({" 正在取消…", Color::Yellow, 0});
    } else if (snapshot.paused) {
        hintSegments.push_back({" 已暂停：按 ", Color::Gray, 0});
        hintSegments.push_back({"[空格]", Color::Cyan, kClickTogglePause});
        hintSegments.push_back({" 继续，按 ", Color::Gray, 0});
        hintSegments.push_back({"[Q]", Color::Cyan, kClickCancel});
        hintSegments.push_back({"/[Esc] 取消", Color::Gray, 0});
    } else if (!hint.empty()) {
        hintSegments.push_back({" " + hint, Color::Gray, 0});
    } else {
        hintSegments.push_back({" ", Color::Gray, 0});
        hintSegments.push_back({"[空格]", Color::Cyan, kClickTogglePause});
        hintSegments.push_back({" 暂停 / 继续    ", Color::Gray, 0});
        hintSegments.push_back({"[Q 或 Esc]", Color::Cyan, kClickCancel});
        hintSegments.push_back({" 取消", Color::Gray, 0});
    }
    // 可点击区域要按"可见列"登记，因此用不带 ANSI 的文本累计宽度
    std::string hintLine;
    std::vector<std::pair<int, int>> pauseSpans;
    std::vector<std::pair<int, int>> cancelSpans;
    int visibleColumn = 0;
    for (const HintSegment& segment : hintSegments) {
        const int begin = visibleColumn;
        const int width = static_cast<int>(textDisplayWidth(segment.text));
        visibleColumn += width;
        if (segment.clickValue == kClickTogglePause) {
            pauseSpans.emplace_back(begin, begin + width - 1);
        } else if (segment.clickValue == kClickCancel) {
            cancelSpans.emplace_back(begin, begin + width - 1);
        }
        hintLine += paint(segment.text, segment.color);
    }

    const std::string lines[5] = {head.str(), fileLine.str(), progressLine.str(), speedLine.str(),
                                  hintLine};

    clearPanel();
    clickableClear();   // 面板自行管理可点击区域（仅面板可见期间有效）
    for (size_t i = 0; i < 5; ++i) {
        const int row = clickableRow();
        if (i == 4 && interactive_) {
            for (const auto& span : pauseSpans) {
                clickableAddSpan(row, span.first, span.second, kClickTogglePause, "[空格]");
            }
            for (const auto& span : cancelSpans) {
                clickableAddSpan(row, span.first, span.second, kClickCancel,
                                 snapshot.paused ? "[Q]" : "[Q 或 Esc]");
            }
        }
        // 按显示宽度截断，避免折行破坏面板行列计算
        std::cout << truncateToWidth(lines[i], usable) << "\n";
    }
    linesDrawn_ = 5;
    std::cout << std::flush;
}

// ================================================================ 菜单 ====
void setActiveJob(JobControl* control) { g_activeJob.store(control); }
JobControl* activeJob() { return g_activeJob.load(); }

namespace {

std::string describeSettings(const Config& cfg) {
    std::ostringstream out;
    out << "算法 " << paint(algoDisplayList(cfg.algos), Color::Green);
    out << " | 线程 " << (cfg.threads > 0 ? std::to_string(cfg.threads)
                                         : std::to_string(effectiveThreadCount(0)) + "(自动)");
    out << " | 缓冲 " << cfg.bufferMB << " MB";
    out << " | 输出 " << formatName(cfg.format);
    out << " | 进度条 " << (cfg.progress ? "开" : "关");
    out << " | 流水线 " << (cfg.overlap ? "开" : "关");
    if (!cfg.filters.empty()) out << " | 过滤 " << wideToUtf8(cfg.filters);
    return out.str();
}

void settingsMenu(Config& cfg) {
    for (;;) {
        printSectionTitle("设置");
        clickableClear();
        std::cout << "  " << paint("配置文件: ", Color::Gray) << wideToUtf8(configFilePath())
                  << (configFileExists() ? paint(configFileIsHidden() ? "  [已隐藏]" : "  [未隐藏]",
                                                 Color::Gray)
                                         : paint("  [尚未创建]", Color::Yellow))
                  << "\n";
        // 设置项也做成可点击的
        {
            const std::vector<std::pair<int, std::string>> items = {
                {1, std::string("  1) 哈希算法           当前: ") + algoDisplayList(cfg.algos)},
                {2, std::string("  2) 线程数             当前: ") +
                        (cfg.threads > 0 ? std::to_string(cfg.threads)
                                         : std::to_string(effectiveThreadCount(0)) + " (自动)")},
                {3, std::string("  3) 读取缓冲区 (MB)    当前: ") + std::to_string(cfg.bufferMB)},
                {4, std::string("  4) 默认输出格式       当前: ") + formatDisplay(cfg.format)},
                {5, std::string("  5) 进度条             ") + (cfg.progress ? "开" : "关")},
                {6, std::string("  6) ASCII 进度条       ") + (cfg.asciiBar ? "开" : "关")},
                {7, std::string("  7) 读取/计算流水线    ") + (cfg.overlap ? "开" : "关")},
                {8, std::string("  8) 递归子目录         ") + (cfg.recursive ? "开" : "关")},
                {9, std::string("  9) 文件过滤          当前: ") +
                        (cfg.filters.empty() ? std::string("(无)") : wideToUtf8(cfg.filters))},
                {10, std::string(" 10) 校验时检查文件大小 ") + (cfg.verifyCheckSize ? "开" : "关")},
                {11, std::string(" 11) 输出文件加 UTF-8 BOM ") + (cfg.bom ? "开" : "关")},
                {12, std::string(" 12) 彩色输出           ") + (cfg.colors ? "开" : "关")},
                {0, "  0) 返回（自动保存设置）"},
            };
            for (const auto& item : items) {
                const int row = clickableRow();
                std::cout << item.second << "\n";
                clickableAddRow(row, item.first, item.second);
            }
        }
        printClickHint();

        const int choice = askMenuChoice("请选择 [0-12]: ", 0, 12, 0);
        if (choice == 0) break;
        switch (choice) {
            case 1: {
                std::cout << "可用算法: " << algoIdList(allAlgos()) << "\n";
                std::cout << "（可多选，用逗号分隔；all 表示全部）\n";
                std::string line;
                if (readLineUtf8("请输入算法: ", line) && !line.empty()) {
                    std::string bad;
                    const std::vector<Algo> parsed = parseAlgoList(line, &bad);
                    if (!parsed.empty()) {
                        cfg.algos = parsed;
                        printSuccess("算法已设置为: " + algoDisplayList(cfg.algos));
                    }
                    if (!bad.empty()) printWarning("无法识别的算法: " + bad);
                }
                break;
            }
            case 2:
                cfg.threads = askInt("线程数（0 = 自动）: ", 0, 128, cfg.threads);
                break;
            case 3:
                cfg.bufferMB = static_cast<size_t>(askInt("缓冲区大小 MB (1-64): ", 1, 64,
                                                          static_cast<int>(cfg.bufferMB)));
                break;
            case 4: {
                const int format = askOptionRow("输出格式", {{1, "文本"}, {2, "CSV"}, {3, "JSON"}},
                                                    static_cast<int>(cfg.format));
                cfg.format = format == 2 ? OutFormat::Csv : (format == 3 ? OutFormat::Json : OutFormat::Text);
                break;
            }
            case 5: cfg.progress = !cfg.progress; break;
            case 6: cfg.asciiBar = !cfg.asciiBar; break;
            case 7: cfg.overlap = !cfg.overlap; break;
            case 8: cfg.recursive = !cfg.recursive; break;
            case 9: {
                std::string line;
                if (readLineUtf8("文件过滤（如 *.exe;*.dll，留空清除）: ", line)) {
                    cfg.filters = utf8ToWide(line);
                }
                break;
            }
            case 10: cfg.verifyCheckSize = !cfg.verifyCheckSize; break;
            case 11: cfg.bom = !cfg.bom; break;
            case 12:
                cfg.colors = !cfg.colors;
                enableColors(cfg.colors);
                break;
            default: break;
        }
        saveConfig(cfg);
    }
}

}  // namespace

int runInteractiveMenu(Config& cfg) {
    enableColors(cfg.colors);
    // 首次运行时在程序所在目录生成隐藏的配置文件（放在只读位置时自动改用 %APPDATA%）
    if (!ensureConfigExists(cfg)) {
        printWarning("配置文件无法写入，本次设置只在内存中生效（不影响任何哈希功能）");
    }
    for (;;) {
        clearScreen();
        showBanner();
        std::cout << "\n  " << paint("当前设置: ", Color::Gray) << describeSettings(cfg) << "\n";

        printSectionTitle("主菜单");
        clickableClear();
        // 每个条目登记为一个可点击区域（鼠标支持时可直接点击选择）
        struct MenuItem { int value; const char* text; };
        static const MenuItem kMainMenu[] = {
            {1, "  1) 计算文件哈希          （可多选文件，Windows 原生选择器）"},
            {2, "  2) 计算文件夹哈希        （递归 / 按通配符过滤）"},
            {3, "  3) 生成校验清单文件      （TXT / CSV / JSON）"},
            {4, "  4) 校验清单文件          （检测篡改 / 缺失 / 大小变化）"},
            {5, "  5) 字节级文件比较        （定位第一个不同字节）"},
            {6, "  6) 算法速度对比          （同一数据下各算法吞吐量）"},
            {7, "  7) 运行内置自检          （标准测试向量验证实现）"},
            {8, "  8) 文件访问监听          （监听期间谁读取/写入了所选文件）"},
            {9, "  9) 设置                  （算法 / 线程 / 缓冲区 / 输出格式）"},
            {10, " 10) 查看支持的算法说明"},
            {0, "  0) 退出"},
        };
        for (const MenuItem& item : kMainMenu) {
            const int row = clickableRow();
            std::cout << item.text << "\n";
            clickableAddRow(row, item.value, item.text);
        }
        printClickHint();

        const int choice = askMenuChoice("\n请选择 [0-10]: ", 0, 10, -1);
        if (choice <= 0) {
            saveConfig(cfg);
            std::cout << "\n" << paint("感谢使用 EHsc，再见！", Color::Cyan) << "\n\n";
            return 0;
        }

        switch (choice) {
            case 1: {
                printSectionTitle("计算文件哈希");
                std::cout << "提示：直接回车将打开 Windows 文件选择器（可多选）\n";
                const std::vector<std::wstring> targets =
                    askTargets("请输入文件路径（多个用 ; 分隔）: ", true, false);
                if (targets.empty()) {
                    printWarning("未选择任何文件");
                } else {
                    cmdHash(cfg, targets, std::wstring(), cfg.format, false);
                }
                break;
            }
            case 2: {
                printSectionTitle("计算文件夹哈希");
                std::cout << "提示：直接回车将打开 Windows 文件夹选择器\n";
                const std::vector<std::wstring> targets =
                    askTargets("请输入文件夹路径: ", false, true);
                if (targets.empty()) {
                    printWarning("未选择任何文件夹");
                } else {
                    cmdHash(cfg, targets, std::wstring(), cfg.format, false);
                }
                break;
            }
            case 3: {
                printSectionTitle("生成校验清单");
                const std::vector<std::wstring> targets =
                    askTargets("请输入文件或文件夹路径（回车调用选择器）: ", true, false);
                if (targets.empty()) {
                    printWarning("未选择任何路径");
                    break;
                }
                const int format = askInt("清单格式 1) 文本  2) CSV  3) JSON : ", 1, 3,
                                          cfg.format == OutFormat::Csv ? 2
                                                                       : (cfg.format == OutFormat::Json ? 3 : 1));
                const OutFormat chosen = format == 2 ? OutFormat::Csv
                                                      : (format == 3 ? OutFormat::Json : OutFormat::Text);
                const std::wstring defaultName = std::wstring(L"ehsc-manifest.") +
                                                 utf8ToWide(formatName(chosen));
                const std::wstring output = askSavePath(
                    "请输入清单保存路径（回车打开保存对话框）: ", defaultName,
                    L"清单文件", chosen == OutFormat::Csv
                                     ? L"*.csv"
                                     : (chosen == OutFormat::Json ? L"*.json" : L"*.txt"));
                if (output.empty()) {
                    printWarning("未指定输出文件，已取消");
                    break;
                }
                cmdHash(cfg, targets, output, chosen, false);
                break;
            }
            case 4: {
                printSectionTitle("校验清单文件");
                std::wstring manifest;
                std::string line;
                if (readLineUtf8("请输入清单文件路径（回车打开选择器）: ", line) && !line.empty()) {
                    manifest = stripQuotes(utf8ToWide(line));
                } else {
                    const std::vector<std::wstring> picked =
                        pickFiles(L"请选择校验清单文件", false);
                    if (!picked.empty()) manifest = picked.front();
                }
                if (manifest.empty()) {
                    printWarning("未选择清单文件");
                    break;
                }
                std::wstring baseDir;
                if (readLineUtf8("请输入基准目录（回车 = 清单所在目录）: ", line) && !line.empty()) {
                    baseDir = stripQuotes(utf8ToWide(line));
                }
                cmdVerify(cfg, manifest, baseDir, std::vector<Algo>(), false, true);
                break;
            }
            case 5: {
                printSectionTitle("字节级文件比较");
                std::vector<std::wstring> files = pickFiles(L"请选择两个文件进行比较（可多选）", true);
                if (files.size() < 2) {
                    files.clear();
                    std::string line;
                    if (readLineUtf8("请输入文件 A 的路径: ", line) && !line.empty())
                        files.push_back(stripQuotes(utf8ToWide(line)));
                    if (readLineUtf8("请输入文件 B 的路径: ", line) && !line.empty())
                        files.push_back(stripQuotes(utf8ToWide(line)));
                }
                if (files.size() < 2) {
                    printWarning("需要两个文件才能比较");
                    break;
                }
                cmdCompare(cfg, files[0], files[1], false, true);
                break;
            }
            case 6: {
                printSectionTitle("算法速度对比");
                const std::vector<std::wstring> targets =
                    askTargets("请输入用于测试的文件（回车调用选择器）: ", false, false);
                if (targets.empty()) {
                    printWarning("未选择文件");
                    break;
                }
                const int seconds = askInt("每个算法至少测试秒数 (1-10): ", 1, 10, 2);
                cmdBench(cfg, targets, static_cast<double>(seconds));
                break;
            }
            case 7: {
                printSectionTitle("内置自检");
                cmdSelfTest(true);
                break;
            }
            case 8: {
                printSectionTitle("文件访问监听");
                std::cout << "监听期间，记录所选文件/文件夹被哪个进程读取或写入。\n";
                std::cout << "提示：想知道是哪个进程，建议以管理员身份运行本程序；\n";
                std::cout << "      非管理员只能归因当前用户可访问的进程（仍可看到文件变更时间线）。\n";
                std::cout << "提示：直接回车将打开 Windows 文件/文件夹选择器\n";
                const std::vector<std::wstring> targets =
                    askTargets("请输入要监听的文件或文件夹: ", true, false);
                if (targets.empty()) {
                    printWarning("未选择任何目标");
                    break;
                }
                const int seconds = askInt("监听时长（秒，0 = 直到按 Q 停止）[默认 30]: ", 0, 86400, 30);
                WatchRunOptions watch;
                watch.seconds = static_cast<double>(seconds);
                watch.recursive = cfg.recursive;
                watch.filters = parseFilters(cfg.filters);
                watch.interactivePrompt = true;   // 菜单流程：照常询问是否提权
                if (askYesNo("是否把监听报告保存成文件? (y/N): ", false)) {
                    const int format = askOptionRow("报告格式", {{1, "文本"}, {2, "CSV"}, {3, "JSON"}}, 1);
                    watch.format = format == 2 ? OutFormat::Csv
                                               : (format == 3 ? OutFormat::Json : OutFormat::Text);
                    std::string line;
                    if (readLineUtf8("报告保存路径（回车打开保存对话框）: ", line) && !line.empty()) {
                        watch.output = stripQuotes(utf8ToWide(line));
                    } else {
                        const std::wstring defaultName =
                            std::wstring(L"ehsc-watch.") + utf8ToWide(formatName(watch.format));
                        watch.output = pickSaveFile(L"保存监听报告", defaultName, L"报告文件",
                                                    watch.format == OutFormat::Csv
                                                        ? L"*.csv"
                                                        : (watch.format == OutFormat::Json ? L"*.json"
                                                                                           : L"*.txt"));
                    }
                }
                cmdWatch(cfg, targets, watch);
                break;
            }
            case 9:
                settingsMenu(cfg);
                break;
            case 10:
                cmdListAlgos();
                break;
            default: break;
        }
        pauseForEnter();
    }
}

}  // namespace ehsc
