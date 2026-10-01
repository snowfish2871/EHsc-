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

bool askYesNo(const std::string& prompt, bool defaultValue) {
    std::string line;
    if (!readLineUtf8(prompt, line)) return defaultValue;
    if (line.empty()) return defaultValue;
    const std::string lower = [&] {
        std::string t = line;
        for (char& c : t) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return t;
    }();
    if (lower == "y" || lower == "yes" || lower == "1" || lower == "是" || lower == "好")
        return true;
    if (lower == "n" || lower == "no" || lower == "0" || lower == "否" || lower == "不")
        return false;
    return defaultValue;
}

void pauseForEnter(const std::string& hint) {
    std::cout << "\n" << paint(hint, Color::Gray) << std::flush;
    std::string line;
    std::getline(std::cin, line);
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
std::wstring configFilePath() { return joinPath(executableDirectory(), L"ehsc.ini"); }

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

bool loadConfig(Config& cfg) {
    std::string text;
    std::wstring err;
    if (!readFileUtf8(configFilePath(), text, err)) return false;
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
            if (mb >= 1 && mb <= 256) cfg.bufferMB = static_cast<size_t>(mb);
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

bool saveConfig(const Config& cfg) {
    std::string text;
    text += "# EHsc 配置文件（UTF-8）\n";
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
    std::wstring err;
    return writeFileUtf8(configFilePath(), text, false, err);
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
    auto lastDraw = std::chrono::steady_clock::now();
    while (running_.load()) {
        const int key = pollKey();
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

    // 第 5 行：操作提示
    std::string hintLine = snapshot.cancelled
                               ? paint(" 正在取消…", Color::Yellow)
                               : " " + paint(snapshot.paused
                                                 ? "已暂停：按 [空格] 继续，按 [Q]/[Esc] 取消"
                                                 : (hint.empty()
                                                        ? std::string("[空格] 暂停 / 继续    [Q 或 Esc] 取消")
                                                        : hint),
                                             Color::Gray);

    const std::string lines[5] = {head.str(), fileLine.str(), progressLine.str(), speedLine.str(),
                                  hintLine};

    clearPanel();
    for (const std::string& line : lines) {
        // 按显示宽度截断，避免折行破坏面板行列计算
        std::cout << truncateToWidth(line, usable) << "\n";
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
        std::cout << "  1) 哈希算法           当前: " << algoDisplayList(cfg.algos) << "\n";
        std::cout << "  2) 线程数             当前: "
                  << (cfg.threads > 0 ? std::to_string(cfg.threads)
                                      : std::to_string(effectiveThreadCount(0)) + " (自动)")
                  << "\n";
        std::cout << "  3) 读取缓冲区 (MB)    当前: " << cfg.bufferMB << "\n";
        std::cout << "  4) 默认输出格式       当前: " << formatDisplay(cfg.format) << "\n";
        std::cout << "  5) 进度条             " << (cfg.progress ? "开" : "关") << "\n";
        std::cout << "  6) ASCII 进度条       " << (cfg.asciiBar ? "开" : "关") << "\n";
        std::cout << "  7) 读取/计算流水线    " << (cfg.overlap ? "开" : "关") << "\n";
        std::cout << "  8) 递归子目录         " << (cfg.recursive ? "开" : "关") << "\n";
        std::cout << "  9) 文件过滤          当前: "
                  << (cfg.filters.empty() ? std::string("(无)") : wideToUtf8(cfg.filters)) << "\n";
        std::cout << " 10) 校验时检查文件大小 " << (cfg.verifyCheckSize ? "开" : "关") << "\n";
        std::cout << " 11) 输出文件加 UTF-8 BOM " << (cfg.bom ? "开" : "关") << "\n";
        std::cout << " 12) 彩色输出           " << (cfg.colors ? "开" : "关") << "\n";
        std::cout << "  0) 返回（自动保存设置）\n";

        const int choice = askInt("请选择 [0-12]: ", 0, 12, 0);
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
                const int format = askInt("输出格式 1) 文本  2) CSV  3) JSON : ", 1, 3, 1);
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
    for (;;) {
        clearScreen();
        showBanner();
        std::cout << "\n  " << paint("当前设置: ", Color::Gray) << describeSettings(cfg) << "\n";

        printSectionTitle("主菜单");
        std::cout << "  1) 计算文件哈希          （可多选文件，Windows 原生选择器）\n";
        std::cout << "  2) 计算文件夹哈希        （递归 / 按通配符过滤）\n";
        std::cout << "  3) 生成校验清单文件      （TXT / CSV / JSON）\n";
        std::cout << "  4) 校验清单文件          （检测篡改 / 缺失 / 大小变化）\n";
        std::cout << "  5) 字节级文件比较        （定位第一个不同字节）\n";
        std::cout << "  6) 算法速度对比          （同一数据下各算法吞吐量）\n";
        std::cout << "  7) 运行内置自检          （标准测试向量验证实现）\n";
        std::cout << "  8) 设置                  （算法 / 线程 / 缓冲区 / 输出格式）\n";
        std::cout << "  9) 查看支持的算法说明\n";
        std::cout << "  0) 退出\n";

        const int choice = askInt("\n请选择 [0-9]: ", 0, 9, -1);
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
            case 8:
                settingsMenu(cfg);
                break;
            case 9:
                cmdListAlgos();
                break;
            default: break;
        }
        pauseForEnter();
    }
}

}  // namespace ehsc
