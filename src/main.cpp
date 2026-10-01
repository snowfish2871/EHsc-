// ============================================================================
//  EHsc · main.cpp
//  程序入口、命令行参数解析、各子命令实现（hash / verify / compare / bench /
//  selftest / algos / 交互式菜单）。
// ============================================================================
#include "core.h"
#include "hashes.h"
#include "report.h"
#include "ui.h"
#include "win32_utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace ehsc {
namespace {

// Ctrl+C 处理：有任务在跑时只请求取消（协作式停止），空闲时交回系统默认处理
bool consoleCtrlHandler(void* /*ctx*/) {
    JobControl* job = activeJob();
    if (job == nullptr) return false;
    job->requestCancel();
    return true;
}

std::string pathText(const std::wstring& path) { return wideToUtf8(path); }

// ------------------------------------------------------- 结果输出（控制台） --
void printHashResults(const HashJobResult& result, const std::vector<Algo>& algos, bool quiet) {
    if (quiet) {
        for (const HashRecord& record : result.records) {
            if (!record.ok) continue;
            for (size_t i = 0; i < algos.size() && i < record.digests.size(); ++i) {
                std::cout << algoInfo(algos[i]).id << "  " << record.digests[i] << "  "
                          << pathText(record.path) << "\n";
            }
        }
        return;
    }

    const bool single = result.records.size() == 1;
    for (const HashRecord& record : result.records) {
        if (single) {
            std::cout << "\n";
            std::cout << paint("文件   : ", Color::Gray) << pathText(record.path) << "\n";
            std::cout << paint("大小   : ", Color::Gray) << formatByteSize(record.size) << " ("
                      << formatCount(record.size) << " 字节)\n";
            std::cout << paint("修改   : ", Color::Gray) << formatUnixTime(record.mtime) << "\n";
        } else {
            std::cout << "\n" << paint("▶ ", Color::Cyan) << pathText(record.path) << paint(
                "  (" + formatByteSize(record.size) + ")", Color::Gray) << "\n";
        }
        if (!record.ok) {
            if (record.cancelled) {
                printWarning("   已取消，未完成");
            } else {
                printError("   处理失败：" + pathText(record.error));
            }
            continue;
        }
        for (size_t i = 0; i < algos.size() && i < record.digests.size(); ++i) {
            std::string name = algoInfo(algos[i]).display;
            while (name.size() < 11) name.push_back(' ');
            std::cout << "    " << paint(name, Color::Green) << " " << record.digests[i] << "\n";
        }
        if (single) {
            std::cout << paint("耗时   : ", Color::Gray) << formatDuration(record.seconds)
                      << paint("    速度: ", Color::Gray)
                      << formatSpeed(record.seconds > 0
                                         ? static_cast<double>(record.size) / record.seconds
                                         : 0.0)
                      << "\n";
        }
    }
}

void printHashSummary(const HashJobResult& result, const std::vector<Algo>& algos) {
    printSectionTitle("统计");
    std::cout << "  文件总数 : " << formatCount(result.records.size()) << "\n";
    std::cout << "  成功/失败: " << result.okFiles << " / " << result.failedFiles << "\n";
    if (result.cancelledFiles > 0) {
        std::cout << "  已取消   : " << result.cancelledFiles << "（未处理完，不计入失败）\n";
    }
    std::cout << "  数据总量 : " << formatByteSize(result.totalBytes) << " ("
              << formatCount(result.totalBytes) << " 字节)\n";
    std::cout << "  总耗时   : " << formatDuration(result.elapsed) << "\n";
    if (result.elapsed > 0) {
        std::cout << "  平均速度 : " << formatSpeed(static_cast<double>(result.totalBytes) /
                                                    result.elapsed)
                  << paint("（" + algoDisplayList(algos) + "）", Color::Gray) << "\n";
    }
    if (result.cancelled) printWarning("任务已被用户取消，结果不完整");
    for (const std::wstring& error : result.errors) {
        printWarning("扫描：" + pathText(error));
    }
}

// 把单个文件读入内存（基准测试用，限制最大字节数）
bool loadFileForBench(const std::wstring& path, size_t maxBytes, std::vector<uint8_t>& data,
                      std::wstring& err) {
    WinFile file;
    if (!file.open(path, err)) return false;
    const uint64_t want = std::min<uint64_t>(file.size(), maxBytes);
    data.resize(static_cast<size_t>(want));
    size_t total = 0;
    while (total < data.size()) {
        size_t got = 0;
        if (!file.read(data.data() + total, data.size() - total, got, err)) return false;
        if (got == 0) break;
        total += got;
    }
    data.resize(total);
    return true;
}

bool writeReportToFile(const Config& cfg, const HashJobResult& result,
                       const std::vector<Algo>& algos, const std::wstring& outputPath,
                       OutFormat format) {
    ReportMeta meta;
    meta.version = versionString();
    meta.generated = isoNowUtc();
    meta.algos = algos;
    meta.roots = result.roots;
    {
        wchar_t computer[256] = {0};
        DWORD size = static_cast<DWORD>(sizeof(computer) / sizeof(computer[0]));
        if (GetComputerNameW(computer, &size)) meta.host = wideToUtf8(computer);
    }
    const std::string text = buildReport(result, meta, format);
    std::wstring err;
    if (!writeFileUtf8(outputPath, text, cfg.bom, err)) {
        printError("写入失败：" + pathText(err));
        return false;
    }
    printSuccess("已写入 " + std::string(formatDisplay(format)) + " 清单：" +
                 pathText(fullPath(outputPath)) + "（" + formatByteSize(text.size()) + "）");
    return true;
}

void printUsage() {
    showBanner();
    std::cout <<
        "\n用法:\n"
        "  ehsc                                  启动交互式菜单（推荐）\n"
        "  ehsc hash <文件|文件夹...> [选项]      计算哈希，可多目标\n"
        "  ehsc verify <清单文件> [选项]          校验清单（比对摘要/大小/缺失）\n"
        "  ehsc compare <文件A> <文件B> [选项]    字节级比较，定位首个差异\n"
        "  ehsc bench <文件> [选项]               各算法吞吐量对比\n"
        "  ehsc selftest                          运行内置标准测试向量\n"
        "  ehsc algos                             列出支持的算法\n"
        "  ehsc pick [--dir]                      调用 Windows 选择器并输出路径\n"
        "\n通用选项:\n"
        "  -a, --algo <列表>     算法，如 sha256,md5、all（默认 sha256）\n"
        "  -t, --threads <N>     线程数，0 = 自动（默认 CPU 核心数，最多 8）\n"
        "  -b, --buffer <MB>     读取缓冲区大小，1-64（默认 4）\n"
        "  -f, --format <fmt>    输出格式 txt | csv | json（默认 txt）\n"
        "  -o, --output <文件>   结果写入文件（清单模式）\n"
        "      --pick            用 Windows 原生选择器挑选输入文件\n"
        "      --pick-dir        用 Windows 原生选择器挑选输入文件夹\n"
        "      --filter <模式>   文件名过滤，如 \"*.exe;*.dll\"\n"
        "  -r, --recursive       递归子目录（默认开启）\n"
        "      --no-recursive    仅当前目录\n"
        "      --no-progress     关闭进度条（适合重定向输出）\n"
        "      --ascii           使用 ASCII 进度条\n"
        "      --quiet           只输出 算法 摘要 路径\n"
        "      --no-overlap      关闭读取/计算流水线\n"
        "      --bom             输出文件带 UTF-8 BOM（方便 Excel 打开 CSV）\n"
        "      --no-colors       关闭彩色输出\n"
        "\nverify 选项:\n"
        "      --base <目录>     清单中相对路径的基准目录（默认清单所在目录）\n"
        "      --no-checksize    不检查清单中记录的文件大小\n"
        "      --all             连通过的条目也一起打印\n"
        "\ncompare 选项:\n"
        "      --quick           仅比较文件大小\n"
        "      --no-hash         不额外计算摘要，只做逐字节比较\n"
        "\nbench 选项:\n"
        "      --seconds <N>     每个算法的最短测试时长（默认 2 秒）\n"
        "\n示例:\n"
        "  ehsc hash D:\\data\\a.iso -a sha256,md5\n"
        "  ehsc hash D:\\data -o manifest.csv -f csv -a all\n"
        "  ehsc verify manifest.csv --base D:\\data\n"
        "  ehsc compare a.bin b.bin\n"
        "  ehsc bench big.dat --seconds 3\n";
}

}  // namespace

// ============================================================== 子命令 ====
int cmdSelfTest(bool verbose) {
    const SelfTestReport report = runSelfTest();
    printSectionTitle("内置自检（标准测试向量 + 块边界扫描）");
    if (verbose) {
        for (const SelfTestItem& item : report.items) {
            if (!item.pass) {
                printError(item.name);
                std::cout << "      期望: " << item.expected << "\n";
                std::cout << "      实际: " << item.actual << "\n";
            }
        }
    }
    std::cout << "  通过: " << paint(std::to_string(report.passed), Color::Green)
              << " 项    失败: "
              << paint(std::to_string(report.failed),
                       report.failed ? Color::Red : Color::Gray)
              << " 项\n";
    if (report.ok()) {
        printSuccess("全部测试向量通过，算法实现与标准一致");
        return 0;
    }
    printError("存在未通过的测试向量，算法实现可能有误");
    return 1;
}

int cmdListAlgos() {
    printSectionTitle("支持的算法（全部为本项目纯 C++ 实现）");
    std::cout << "  " << paint("标识", Color::Bold) << std::string(10, ' ')
              << paint("名称", Color::Bold) << std::string(6, ' ') << paint("摘要长度", Color::Bold)
              << "  " << paint("安全用途", Color::Bold) << "   说明\n";
    for (int i = 0; i < kAlgoCount; ++i) {
        const AlgoInfo& info = algoTable()[i];
        std::string id = info.id;
        while (id.size() < 12) id.push_back(' ');
        std::string name = info.display;
        while (name.size() < 14) name.push_back(' ');
        char length[32];
        std::snprintf(length, sizeof(length), "%3d 字节", info.digestBytes);
        const std::string verdict =
            info.secure ? paint("✔ 可用  ", Color::Green) : paint("✘ 不适用", Color::Red);
        std::cout << "  " << paint(id, Color::Cyan) << name << length << "  " << verdict << "   "
                  << info.note << "\n";
    }

    std::cout << "\n";
    const std::string line(76, '=');
    std::cout << paint(line, Color::Red) << "\n";
    std::cout << paint("  ⚠ 安全警告：CRC32、MD5、SHA-1 已经完全不适合任何安全用途", Color::Red)
              << "\n";
    std::cout << paint(line, Color::Red) << "\n";
    std::cout << "  · MD5   ：碰撞可在数秒内人工构造（2004 年即被攻破）。即使对方给出的 MD5\n"
                 "            与你的计算结果一致，也完全不能证明文件没有被替换。\n"
                 "  · SHA-1 ：2017 年 SHAttered 攻击已实际生成两份碰撞 PDF，此后攻击成本持续\n"
                 "            下降，用其伪造“防篡改”标记已不具备任何安全意义。\n"
                 "  · CRC32 ：本质是线性校验和，只要在修改数据时同步调整校验值就能任意伪造，\n"
                 "            甚至不需要真正制造碰撞。\n";
    std::cout << "  · 这三者现在只应用于：检测下载/复制过程中的偶然损坏、比对两处副本是否一致、\n"
                 "    读取老系统遗留的清单。\n";
    std::cout << "  · 凡涉及防篡改、数字签名、口令存储、软件发布校验、供应链验证，请使用 "
              << paint("SHA-256 / SHA-384 / SHA-512 /\n    SHA3-256 / SHA3-512 / BLAKE2b-512",
                       Color::Green)
              << "。\n";
    std::cout << "  · 同时列出多个弱算法不会提高安全性：一份清单的可信度取决于其中最弱的一环。\n";
    std::cout << paint(line, Color::Red) << "\n";
    std::cout << "\n  提示：命令行用 -a sha256,md5 指定；-a all 表示全部算法同时计算"
                 "（其中含三个弱算法，仅建议用于兼容性测试）。\n";
    return 0;
}

int cmdHash(const Config& cfg, const std::vector<std::wstring>& targets,
            const std::wstring& outputPath, OutFormat format, bool quiet) {
    if (targets.empty()) {
        printError("没有指定要处理的文件或文件夹");
        return 4;
    }

    HashJobOptions options;
    options.algos = normalizeAlgoList(cfg.algos);
    options.threads = cfg.threads;
    options.bufferSize = bufferBytes(cfg);
    options.overlap = cfg.overlap;
    options.scan.recursive = cfg.recursive;
    options.scan.filters = parseFilters(cfg.filters);
    options.scan.includeHidden = cfg.includeHidden;

    JobControl control;
    setActiveJob(&control);
    ProgressMonitor monitor(control, cfg.asciiBar, cfg.progress && !quiet, cfg.progressForce);

    if (!quiet) {
        printSectionTitle("计算哈希");
        printInfo("算法   : " + algoDisplayList(options.algos));
        printInfo("线程   : " + std::to_string(effectiveThreadCount(options.threads)) + "（缓冲区 " +
                  std::to_string(options.bufferSize >> 20) + " MB，流水线 " +
                  (options.overlap ? "开" : "关") + "）");
        printInfo("目标   : " + std::to_string(targets.size()) + " 个路径");
        // 选中弱算法时给出安全提醒（不阻止执行：完整性检查场景仍然合理）
        if (hasInsecureAlgo(options.algos)) {
            printWarning("所选算法包含 " + insecureAlgoList(options.algos) +
                         "：它们已完全不适合安全用途（防篡改/签名/口令），\n"
                         "            仅可用于文件完整性、误码与副本一致性检查；"
                         "安全场景请改用 SHA-256/SHA3-256/BLAKE2b-512。");
        }
    }

    monitor.start("计算哈希 " + algoDisplayList(options.algos));
    const HashJobResult result = runHashJob(targets, options, control);
    monitor.stop();
    setActiveJob(nullptr);

    if (result.records.empty()) {
        printWarning("没有找到任何可处理的文件");
        for (const std::wstring& error : result.errors) printError(pathText(error));
        return 4;
    }

    printHashResults(result, options.algos, quiet);
    if (!quiet) printHashSummary(result, options.algos);

    int code = 0;
    if (!outputPath.empty()) {
        if (!writeReportToFile(cfg, result, options.algos, outputPath, format)) code = 4;
    }
    if (result.cancelled) return 3;
    if (result.failedFiles > 0) return 1;
    return code;
}

int cmdVerify(const Config& cfg, const std::wstring& manifestPath, const std::wstring& baseDir,
              const std::vector<Algo>& filterAlgos, bool quiet, bool reportAll) {
    printSectionTitle("校验清单");
    const Manifest manifest = parseManifestFile(manifestPath);
    if (!manifest.ok) {
        printError("清单解析失败：" + pathText(manifest.error));
        return 2;
    }
    const char* kindName = manifest.kind == ManifestKind::Json
                               ? "JSON"
                               : (manifest.kind == ManifestKind::Csv ? "CSV" : "文本");
    printInfo(std::string("清单格式 : ") + kindName + "    条目数 : " +
              std::to_string(manifest.entries.size()) + "    算法 : " + algoIdList(manifest.algos));

    std::wstring base = baseDir;
    if (base.empty()) base = parentDir(fullPath(manifestPath));

    VerifyOptions options;
    options.filterAlgos = filterAlgos;
    options.bufferSize = bufferBytes(cfg);
    options.threads = cfg.threads;
    options.checkSize = cfg.verifyCheckSize;
    options.reportAll = reportAll && !quiet;
    options.baseDirIsExplicit = !baseDir.empty();

    JobControl control;
    setActiveJob(&control);
    ProgressMonitor monitor(control, cfg.asciiBar, cfg.progress && !quiet, cfg.progressForce);
    monitor.start("校验清单");
    const VerifyResult result = verifyManifest(manifest, base, options, control);
    monitor.stop();
    setActiveJob(nullptr);

    if (!quiet) {
        printSectionTitle("校验结果");
        for (const VerifyItem& item : result.items) {
            switch (item.status) {
                case VerifyStatus::Passed:
                    std::cout << paint("  [通过] ", Color::Green) << pathText(item.rel) << "\n";
                    break;
                case VerifyStatus::Mismatch:
                case VerifyStatus::SizeMismatch:
                    std::cout << paint("  [不一致] ", Color::Red) << pathText(item.rel) << "\n";
                    std::cout << "           " << pathText(item.detail) << "\n";
                    break;
                case VerifyStatus::Missing:
                    std::cout << paint("  [缺失] ", Color::Yellow) << pathText(item.rel) << "\n";
                    std::cout << "           " << pathText(item.detail) << "\n";
                    break;
                case VerifyStatus::ReadError:
                    std::cout << paint("  [错误] ", Color::Red) << pathText(item.rel) << "\n";
                    std::cout << "           " << pathText(item.detail) << "\n";
                    break;
                case VerifyStatus::Skipped:
                    std::cout << paint("  [跳过] ", Color::Gray) << pathText(item.rel) << "\n";
                    if (!item.detail.empty() && item.detail != L"校验通过") {
                        std::cout << "           " << pathText(item.detail) << "\n";
                    }
                    break;
            }
        }
    }

    printSectionTitle("校验统计");
    std::cout << "  基准目录 : " << pathText(result.resolvedBase)
              << "（用于解析清单中的相对路径）\n";
    std::cout << "  条目总数 : " << formatCount(result.total) << "\n";
    std::cout << "  通过     : " << paint(formatCount(result.passed), Color::Green) << "\n";
    std::cout << "  不一致   : "
              << paint(formatCount(result.failed), result.failed ? Color::Red : Color::Gray) << "\n";
    std::cout << "  缺失     : "
              << paint(formatCount(result.missing), result.missing ? Color::Yellow : Color::Gray)
              << "\n";
    std::cout << "  读取错误 : "
              << paint(formatCount(result.errors), result.errors ? Color::Red : Color::Gray) << "\n";
    if (result.skipped) std::cout << "  跳过     : " << result.skipped << "\n";
    std::cout << "  已校验量 : " << formatByteSize(result.bytesChecked) << "\n";
    std::cout << "  耗时     : " << formatDuration(result.elapsed) << "\n";
    if (result.elapsed > 0) {
        std::cout << "  平均速度 : "
                  << formatSpeed(static_cast<double>(result.bytesChecked) / result.elapsed) << "\n";
    }

    if (result.cancelled) {
        printWarning("校验被取消，结果不完整");
        return 3;
    }
    if (result.failed == 0 && result.missing == 0 && result.errors == 0) {
        printSuccess("全部条目校验通过，文件未被篡改");
        return 0;
    }
    printError("存在校验失败的条目，文件可能已被修改或损坏");
    return 1;
}

int cmdCompare(const Config& cfg, const std::wstring& pathA, const std::wstring& pathB, bool quick,
               bool withHash) {
    printSectionTitle("字节级文件比较");

    FileInfo infoA, infoB;
    std::wstring err;
    if (!getFileInfo(pathA, infoA, err)) {
        printError("无法读取「" + pathText(pathA) + "」：" + pathText(err));
        return 2;
    }
    if (!getFileInfo(pathB, infoB, err)) {
        printError("无法读取「" + pathText(pathB) + "」：" + pathText(err));
        return 2;
    }
    std::cout << "  " << paint("文件 A: ", Color::Gray) << pathText(fullPath(pathA)) << "\n";
    std::cout << "           " << formatByteSize(infoA.size) << " · " << formatUnixTime(infoA.mtime)
              << "\n";
    std::cout << "  " << paint("文件 B: ", Color::Gray) << pathText(fullPath(pathB)) << "\n";
    std::cout << "           " << formatByteSize(infoB.size) << " · " << formatUnixTime(infoB.mtime)
              << "\n";

    CompareOptions options;
    options.bufferSize = bufferBytes(cfg);
    options.quick = quick;
    options.withHash = withHash;
    options.algos = normalizeAlgoList(cfg.algos);

    JobControl control;
    setActiveJob(&control);
    ProgressMonitor monitor(control, cfg.asciiBar, cfg.progress, cfg.progressForce);
    monitor.start("比较文件内容");
    const CompareResult result = compareFiles(pathA, pathB, options, control);
    monitor.stop();
    setActiveJob(nullptr);

    if (result.status == 2) {
        printError(pathText(result.error));
        return 2;
    }
    if (result.status == 3) {
        printWarning("比较已取消");
        return 3;
    }

    printSectionTitle("比较结果");
    std::cout << "  大小 : A = " << formatCount(result.sizeA) << " 字节, B = " << formatCount(result.sizeB)
              << " 字节 → " << (result.sizeEqual ? paint("一致", Color::Green) : paint("不一致", Color::Red))
              << "\n";
    if (!quick) {
        std::cout << "  逐字节比较 : " << formatByteSize(result.compared) << "\n";
    }

    if (withHash && !result.digestsA.empty()) {
        std::cout << "\n";
        for (size_t i = 0; i < options.algos.size() && i < result.digestsA.size(); ++i) {
            const bool same = result.digestsA[i] == result.digestsB[i];
            std::string name = algoInfo(options.algos[i]).display;
            while (name.size() < 11) name.push_back(' ');
            std::cout << "  " << paint(name, Color::Gray)
                      << (same ? paint("相同  ", Color::Green) : paint("不同  ", Color::Red));
            if (same) {
                std::cout << result.digestsA[i] << "\n";
            } else {
                std::cout << "\n    A " << result.digestsA[i] << "\n    B " << result.digestsB[i]
                          << "\n";
            }
        }
    }

    if (result.contentEqual) {
        std::cout << "\n";
        printSuccess("两个文件完全相同");
    } else {
        std::cout << "\n";
        printError("两个文件内容不同");
        if (result.firstDiff != UINT64_MAX) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(result.firstDiff));
            std::cout << "  首个差异位置 : 偏移 " << buffer << " (0x";
            std::cout << std::hex << result.firstDiff << std::dec << ")\n";
            auto dump = [](const std::vector<uint8_t>& data) {
                std::string out;
                char hex[8];
                for (uint8_t byte : data) {
                    std::snprintf(hex, sizeof(hex), "%02X ", byte);
                    out += hex;
                }
                return out;
            };
            std::cout << "  A: " << dump(result.windowA) << "\n";
            std::cout << "  B: " << dump(result.windowB) << "\n";
        } else if (!result.sizeEqual) {
            printInfo("仅文件长度不同，公共部分内容一致");
        }
    }
    std::cout << "\n  耗时 : " << formatDuration(result.seconds) << "    速度 : "
              << formatSpeed(result.seconds > 0 ? static_cast<double>(result.compared * 2) / result.seconds
                                                : 0.0)
              << "\n";
    return result.contentEqual ? 0 : 1;
}

int cmdBench(const Config& cfg, const std::vector<std::wstring>& targets, double minSeconds) {
    if (targets.empty()) {
        printError("没有指定用于测试的文件");
        return 4;
    }
    const std::wstring path = fullPath(targets.front());
    printSectionTitle("算法速度对比");

    std::vector<uint8_t> data;
    std::wstring err;
    const size_t maxBytes = 64u << 20;
    if (!loadFileForBench(path, maxBytes, data, err) || data.empty()) {
        printError("无法读取测试数据：" + pathText(err));
        return 4;
    }
    printInfo("测试文件 : " + pathText(path));
    printInfo("测试数据 : " + formatByteSize(data.size()) +
              (data.size() == maxBytes ? "（已截断到 64 MB）" : ""));
    printInfo("CPU 线程 : " + std::to_string(effectiveThreadCount(0)) +
              "    每算法最短测试时间: " + formatDuration(minSeconds));

    JobControl control;
    setActiveJob(&control);
    ProgressMonitor monitor(control, cfg.asciiBar, cfg.progress, cfg.progressForce);
    monitor.start("算法吞吐量测试");
    const std::vector<BenchRow> rows =
        benchmarkMemory(data.data(), data.size(), normalizeAlgoList(cfg.algos), minSeconds, control);
    monitor.stop();
    setActiveJob(nullptr);

    if (rows.empty()) {
        printWarning("没有完成任何测试");
        return 3;
    }

    std::vector<BenchRow> sorted = rows;
    std::sort(sorted.begin(), sorted.end(),
              [](const BenchRow& a, const BenchRow& b) { return a.mbPerSec > b.mbPerSec; });

    printSectionTitle("结果（按吞吐量排序）");
    std::cout << "  " << paint("排名  算法            单次耗时        吞吐量          相对最快", Color::Bold)
              << "\n";
    const double best = sorted.front().mbPerSec > 0 ? sorted.front().mbPerSec : 1;
    int rank = 0;
    for (const BenchRow& row : sorted) {
        char timing[64];
        std::snprintf(timing, sizeof(timing), "%8.3f ms", row.seconds * 1000.0);
        std::string name = algoInfo(row.algo).display;
        while (name.size() < 15) name.push_back(' ');
        char speed[64];
        std::snprintf(speed, sizeof(speed), "%10.1f MB/s", row.mbPerSec);
        char relative[32];
        std::snprintf(relative, sizeof(relative), "%6.2fx", row.mbPerSec / best);
        std::cout << "  " << ++rank << "     " << paint(name, Color::Cyan) << timing << "   " << speed
                  << "   " << relative << "\n";
    }
    const BenchRow& fastest = sorted.front();
    std::cout << "\n";
    printSuccess(std::string("最快：") + algoInfo(fastest.algo).display + " —— " +
                 formatSpeed(fastest.mbPerSec * 1024 * 1024) +
                 "（纯内存吞吐，含哈希初始化开销）");
    return 0;
}

// ========================================================== 参数解析 ====
namespace {

struct ParsedArgs {
    std::string command;
    std::vector<std::wstring> positional;
    std::vector<Algo> algos;
    bool hasAlgos = false;
    int threads = -1;
    int bufferMB = -1;
    OutFormat format = OutFormat::Text;
    bool hasFormat = false;
    std::wstring output;
    std::wstring baseDir;
    std::wstring filter;
    bool recursive = true;
    bool progress = true;
    bool progressSet = false;
    bool asciiBar = false;
    bool quiet = false;
    bool overlap = true;
    bool bom = false;
    bool colors = true;
    bool usePicker = false;
    bool pickerIsFolder = false;
    bool reportAll = false;
    bool checkSize = true;
    bool quick = false;
    bool withHash = true;
    double seconds = 2.0;
    bool help = false;
    bool version = false;
};

std::wstring toWideLower(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return out;
}

bool parseArgs(int argc, wchar_t** argv, ParsedArgs& out) {
    std::string bad;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        const std::wstring lower = toWideLower(arg);

        auto nextValue = [&](std::wstring& value) -> bool {
            const size_t eq = arg.find(L'=');
            if (eq != std::wstring::npos) {
                value = arg.substr(eq + 1);
                return true;
            }
            if (i + 1 < argc) {
                value = argv[++i];
                return true;
            }
            return false;
        };

        if (lower == L"-h" || lower == L"--help" || lower == L"-?" || lower == L"/?") {
            out.help = true;
            continue;
        }
        if (lower == L"-v" || lower == L"--version") {
            out.version = true;
            continue;
        }
        if (lower == L"-a" || lower == L"--algo" || lower == L"--algos" ||
            lower.rfind(L"--algo=", 0) == 0 || lower.rfind(L"-a=", 0) == 0) {
            std::wstring value;
            if (!nextValue(value)) return false;
            out.algos = parseAlgoList(wideToUtf8(value), &bad);
            out.hasAlgos = true;
            if (!bad.empty()) printWarning("无法识别的算法: " + bad);
            continue;
        }
        if (lower == L"-t" || lower == L"--threads") {
            std::wstring value;
            if (!nextValue(value)) return false;
            out.threads = std::atoi(wideToUtf8(value).c_str());
            continue;
        }
        if (lower == L"-b" || lower == L"--buffer") {
            std::wstring value;
            if (!nextValue(value)) return false;
            out.bufferMB = std::atoi(wideToUtf8(value).c_str());
            continue;
        }
        if (lower == L"-f" || lower == L"--format") {
            std::wstring value;
            if (!nextValue(value)) return false;
            if (!parseFormat(wideToUtf8(value), out.format)) {
                printWarning("未知的输出格式，已回退为 txt");
            }
            out.hasFormat = true;
            continue;
        }
        if (lower == L"-o" || lower == L"--output") {
            std::wstring value;
            if (!nextValue(value)) return false;
            out.output = value;
            continue;
        }
        if (lower == L"--base") {
            std::wstring value;
            if (!nextValue(value)) return false;
            out.baseDir = value;
            continue;
        }
        if (lower == L"--filter") {
            std::wstring value;
            if (!nextValue(value)) return false;
            out.filter = value;
            continue;
        }
        if (lower == L"--seconds") {
            std::wstring value;
            if (!nextValue(value)) return false;
            out.seconds = std::atof(wideToUtf8(value).c_str());
            if (out.seconds < 0.1) out.seconds = 0.1;
            continue;
        }
        if (lower == L"-r" || lower == L"--recursive") {
            out.recursive = true;
            continue;
        }
        if (lower == L"--no-recursive" || lower == L"-n") {
            out.recursive = false;
            continue;
        }
        if (lower == L"--progress") {
            out.progress = true;
            out.progressSet = true;
            continue;
        }
        if (lower == L"--no-progress" || lower == L"--no-progress-bar") {
            out.progress = false;
            out.progressSet = true;
            continue;
        }
        if (lower == L"--ascii") {
            out.asciiBar = true;
            continue;
        }
        if (lower == L"--quiet" || lower == L"-q") {
            out.quiet = true;
            continue;
        }
        if (lower == L"--no-overlap") {
            out.overlap = false;
            continue;
        }
        if (lower == L"--bom") {
            out.bom = true;
            continue;
        }
        if (lower == L"--no-colors" || lower == L"--no-color") {
            out.colors = false;
            continue;
        }
        if (lower == L"--pick") {
            out.usePicker = true;
            out.pickerIsFolder = false;
            continue;
        }
        if (lower == L"--pick-dir" || lower == L"--pick-folder") {
            out.usePicker = true;
            out.pickerIsFolder = true;
            continue;
        }
        if (lower == L"--all") {
            out.reportAll = true;
            continue;
        }
        if (lower == L"--no-checksize" || lower == L"--no-check-size") {
            out.checkSize = false;
            continue;
        }
        if (lower == L"--quick") {
            out.quick = true;
            continue;
        }
        if (lower == L"--no-hash") {
            out.withHash = false;
            continue;
        }
        if (!arg.empty() && (arg[0] == L'-') && arg.size() > 1) {
            printWarning("未知的选项: " + wideToUtf8(arg));
            continue;
        }

        if (out.command.empty()) {
            out.command = wideToUtf8(lower);
        } else {
            out.positional.push_back(arg);
        }
    }
    return true;
}

}  // namespace

int runCommandLine(int argc, wchar_t** argv) {
    ParsedArgs args;
    if (!parseArgs(argc, argv, args)) {
        printUsage();
        return 4;
    }

    Config cfg;
    loadConfig(cfg);
    enableColors(args.colors);
    cfg.colors = args.colors;
    if (args.hasAlgos && !args.algos.empty()) cfg.algos = args.algos;
    if (args.threads >= 0) cfg.threads = args.threads;
    if (args.bufferMB > 0) cfg.bufferMB = static_cast<size_t>(args.bufferMB);
    if (args.hasFormat) cfg.format = args.format;
    if (!args.filter.empty()) cfg.filters = args.filter;
    cfg.recursive = args.recursive;
    if (args.progressSet) {
        cfg.progress = args.progress;
        // 显式 --progress：输出被重定向时也每秒输出一行纯文本进度
        cfg.progressForce = args.progress;
    }
    cfg.asciiBar = args.asciiBar || cfg.asciiBar;
    cfg.overlap = args.overlap;
    cfg.bom = args.bom || cfg.bom;
    cfg.verifyCheckSize = args.checkSize;

    installConsoleCtrlHandler(consoleCtrlHandler, nullptr);

    if (args.version) {
        std::cout << "EHsc " << versionString() << "  (" << buildInfoString() << ")\n";
        return 0;
    }
    if (args.help) {
        printUsage();
        return 0;
    }

    if (args.command.empty()) {
        return runInteractiveMenu(cfg);
    }

    const std::string& command = args.command;
    if (command == "hash" || command == "h") {
        std::vector<std::wstring> targets = args.positional;
        if (args.usePicker || targets.empty()) {
            if (args.pickerIsFolder) {
                const std::wstring folder = pickFolder(L"请选择文件夹");
                if (!folder.empty()) targets.push_back(folder);
            } else if (args.usePicker) {
                const std::vector<std::wstring> picked = pickFiles(L"请选择文件", true);
                targets.insert(targets.end(), picked.begin(), picked.end());
            }
        }
        if (targets.empty()) {
            printError("没有指定目标文件或文件夹（可用 --pick 打开选择器）");
            return 4;
        }
        return cmdHash(cfg, targets, args.output, args.format, args.quiet);
    }
    if (command == "verify" || command == "check" || command == "v") {
        std::wstring manifest;
        if (!args.positional.empty()) manifest = args.positional.front();
        if (manifest.empty() && args.usePicker) {
            const std::vector<std::wstring> picked = pickFiles(L"请选择校验清单", false);
            if (!picked.empty()) manifest = picked.front();
        }
        if (manifest.empty()) {
            printError("请指定清单文件，例如：ehsc verify manifest.csv");
            return 4;
        }
        return cmdVerify(cfg, manifest, args.baseDir,
                         args.hasAlgos ? args.algos : std::vector<Algo>(), args.quiet, args.reportAll);
    }
    if (command == "compare" || command == "cmp" || command == "c") {
        std::vector<std::wstring> files = args.positional;
        if (files.size() < 2 && args.usePicker) {
            const std::vector<std::wstring> picked = pickFiles(L"请选择两个文件", true);
            files.insert(files.end(), picked.begin(), picked.end());
        }
        if (files.size() < 2) {
            printError("需要两个文件，例如：ehsc compare a.bin b.bin");
            return 4;
        }
        return cmdCompare(cfg, files[0], files[1], args.quick, args.withHash);
    }
    if (command == "bench" || command == "benchmark" || command == "b") {
        std::vector<std::wstring> targets = args.positional;
        if (targets.empty() && args.usePicker) {
            const std::vector<std::wstring> picked = pickFiles(L"请选择用于测试的文件", false);
            targets.insert(targets.end(), picked.begin(), picked.end());
        }
        return cmdBench(cfg, targets, args.seconds);
    }
    if (command == "selftest" || command == "test") {
        return cmdSelfTest(true);
    }
    if (command == "algos" || command == "algorithms" || command == "list") {
        return cmdListAlgos();
    }
    if (command == "pick" || command == "select") {
        std::vector<std::wstring> picked;
        if (args.pickerIsFolder) {
            const std::wstring folder = pickFolder(L"请选择文件夹");
            if (!folder.empty()) picked.push_back(folder);
        } else {
            picked = pickFiles(L"请选择文件", true);
        }
        for (const std::wstring& path : picked) std::cout << pathText(path) << "\n";
        return picked.empty() ? 1 : 0;
    }

    printError("未知的命令: " + command);
    printUsage();
    return 4;
}

}  // namespace ehsc

// ================================================================ 入口 ====
int main() {
    ehsc::setupConsoleUtf8();

    static ehsc::Config fallbackConfig;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int code = 0;
    if (argv == nullptr) {
        std::cout << "EHsc " << ehsc::versionString() << "\n";
        code = ehsc::runInteractiveMenu(fallbackConfig);
    } else {
        code = ehsc::runCommandLine(argc, argv);
        LocalFree(argv);
    }

    ehsc::restoreConsole();
    return code;
}
