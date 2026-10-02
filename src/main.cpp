// ============================================================================
//  EHsc · main.cpp
//  程序入口、命令行参数解析、各子命令实现（hash / verify / compare / bench /
//  watch / selftest / algos / config / 交互式菜单）。
// ============================================================================
#include "core.h"
#include "hashes.h"
#include "report.h"
#include "ui.h"
#include "watch.h"
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
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
        "  EHsc.exe                                  启动交互式菜单（推荐）\n"
        "  EHsc.exe hash <文件|文件夹...> [选项]      计算哈希，可多目标\n"
        "  EHsc.exe verify <清单文件> [选项]          校验清单（比对摘要/大小/缺失）\n"
        "  EHsc.exe compare <文件A> <文件B> [选项]    字节级比较，定位首个差异\n"
        "  EHsc.exe bench <文件> [选项]               各算法吞吐量对比\n"
        "  EHsc.exe watch <文件|文件夹...> [选项]     监听谁在读/写这些文件\n"
        "  EHsc.exe selftest                          运行内置标准测试向量\n"
        "  EHsc.exe algos                             列出支持的算法与安全性说明\n"
        "  EHsc.exe config                            显示配置文件位置与状态\n"
        "  EHsc.exe pick [--dir]                      调用 Windows 选择器并输出路径\n"
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
        "\nwatch 选项（监听谁读写了目标文件）:\n"
        "      --watch <秒>      监听时长，0 = 一直监听到按 Q 停止（默认 30 秒）\n"
        "      --pid <N>         只看这个进程号的访问\n"
        "      --no-handles      关闭句柄采样（只剩文件变更通知，无需权限但看不到进程）\n"
        "      --no-notify       关闭目录变更通知（只保留\"谁打开着文件\"）\n"
        "      --include-self    连 EHsc 自己产生的事件也记录\n"
        "      --max-records <N> 最多保留多少条记录（默认 20000）\n"
        "      --elevate         直接请求管理员权限（弹 UAC），不询问\n"
        "      --no-elevate      不提权、不询问，以当前权限监听\n"
        "      -o/-f             把监听报告写成文件（txt | csv | json）\n"
        "\n  启动时会主动询问是否以管理员身份重新运行：\n"
        "    · 选 Y → 走正规 UAC 提权流程，在新窗口中以管理员权限执行监听（结果最完整）\n"
        "    · 选 N → 以当前权限监听（只能归因当前用户可访问的进程）\n"
        "    · 若 UAC 授权被拒绝 → 本次监听不执行，直接返回上一层\n"
        "    · 无人应答时 30 秒后按 N 继续（可用 EHSC_ELEVATE_TIMEOUT 调整，0=一直等）\n"
        "\n示例:\n"
        "  EHsc.exe hash D:\\data\\a.iso -a sha256,md5\n"
        "  EHsc.exe hash D:\\data -o manifest.csv -f csv -a all\n"
        "  EHsc.exe verify manifest.csv --base D:\\data\n"
        "  EHsc.exe compare a.bin b.bin\n"
        "  EHsc.exe bench big.dat --seconds 3\n"
        "  EHsc.exe watch D:\\data\\config.ini --watch 60\n"
        "  EHsc.exe watch D:\\share -o watch.json -f json --watch 120\n"
        "\n说明: 想知道是【哪个进程】在读写文件，请以管理员身份运行（否则只能看到当前用户\n"
        "      可访问的进程）。非管理员同样可以拿到文件变更时间线。\n"
        "\n⚠ 安全提醒: CRC32 / MD5 / SHA-1 仅用于文件完整性检查，绝不可用于安全用途。\n";
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

// ====================================================== 文件访问监听命令 ====
namespace {

std::string describeAccessRecord(const AccessRecord& record) {
    std::string line;
    line += paint("[" + formatUnixTime(record.firstSeenMs / 1000) + "] ", Color::Gray);
    const WatchOp op = record.op;
    const Color color = (op == WatchOp::Write || op == WatchOp::ReadWrite)
                            ? Color::Yellow
                            : (op == WatchOp::Read ? Color::Green : Color::Gray);
    line += paint(padToWidth(watchOpDisplay(op), 10), color);
    line += paint(padToWidth(wideToUtf8(record.processName) + " (pid " +
                                 std::to_string(record.pid) + ")",
                             34),
                  Color::Cyan);
    line += wideToUtf8(record.filePath);
    return line;
}

std::string describeChangeRecord(const ChangeRecord& record) {
    std::string line;
    line += paint("[" + formatUnixTime(record.timeMs / 1000) + "] ", Color::Gray);
    line += paint(padToWidth(changeKindDisplay(record.kind), 10),
                  record.kind == ChangeKind::Removed ? Color::Red : Color::Magenta);
    line += wideToUtf8(record.path);
    if (!record.newPath.empty()) line += "  ->  " + wideToUtf8(record.newPath);
    return line;
}

}  // namespace

// 提权询问的等待秒数：默认 30 秒，0 表示一直等；可用环境变量 EHSC_ELEVATE_TIMEOUT 调整
int elevatePromptTimeoutSeconds() {
    int seconds = 30;
    wchar_t buffer[16] = {0};
    const DWORD len = GetEnvironmentVariableW(L"EHSC_ELEVATE_TIMEOUT", buffer, 16);
    if (len > 0 && len < 16) {
        const int parsed = std::atoi(wideToUtf8(buffer).c_str());
        if (parsed >= 0 && parsed <= 3600) seconds = parsed;
    }
    return seconds;
}

int cmdWatch(const Config& cfg, const std::vector<std::wstring>& targets,
             const WatchRunOptions& options) {
    if (targets.empty()) {
        printError("没有指定要监听的文件或文件夹");
        return 4;
    }

    // ---------------------------------------------------------- 提权询问 --
    // 归因"是哪个进程"需要管理员权限才能看到系统进程与其他用户的句柄。
    // 因此启动监听前主动问一次：选 Y 就走正规 UAC 提权流程（重启一个提权后的自己）；
    // 用户在 UAC 对话框上拒绝时，本次监听不执行，直接返回上一层。
    std::vector<std::wstring> resolvedTargets;
    resolvedTargets.reserve(targets.size());
    for (const std::wstring& raw : targets) {
        const std::wstring full = fullPath(stripQuotes(raw));
        resolvedTargets.push_back(full.empty() ? raw : full);
    }

    if (!isProcessElevated() && options.elevate != ElevateMode::Never) {
        bool attempt = options.elevate == ElevateMode::Force;
        if (options.elevate == ElevateMode::Ask) {
            // 能问就问：控制台、管道里已有数据、或调用方确认这是交互流程（主菜单）。
            // 注意：如果是"重绑之后才拿到控制台"，说明标准流本来是被重定向的 ——
            // 那种情况（脚本、管道、计划任务）不能提问，否则会把调用方卡在提问上。
            const bool genuineConsole = stdinIsConsole() && !consoleStreamsWereRebound();
            const bool canAsk = options.interactivePrompt || genuineConsole ||
                                stdinHasPendingInput();
            if (canAsk) {
                std::cout << "\n" << paint("⚠ 权限提示", Color::Yellow) << "\n";
                std::cout << "  监听\"是哪个进程读写了文件\"需要管理员权限：只有提权后才能解析\n"
                             "  系统进程、服务与其他用户的句柄。不提权也能运行，但归因结果会不完整。\n";
                // 限时询问：无人应答（脚本、计划任务、走开了）时按"不提权"继续，
                // 绝不把调用方永久卡在提问上。可用 --elevate / --no-elevate 直接表态。
                const int timeout = elevatePromptTimeoutSeconds();
                std::string question = "是否以管理员身份重新启动监听任务（会弹出 UAC 授权窗口）? (Y/N): ";
                if (timeout > 0) {
                    question = "是否以管理员身份重新启动监听任务（会弹出 UAC 授权窗口）? (Y/N，"
                               + std::to_string(timeout) + " 秒内未选择则按 N 继续): ";
                }
                bool timedOut = false;
                attempt = askYesNoTimed(question, false, timeout, &timedOut);
                if (timedOut) {
                    printInfo("等待超时，按\"不提权\"处理，使用当前权限继续");
                } else if (!attempt) {
                    printInfo("已选择不提权，使用当前权限继续（报告里会注明覆盖面限制）");
                }
            } else {
                printWarning("标准输入没有可读内容，无法询问是否提权；将以当前权限运行"
                             "（如需完整归因请加 --elevate 或直接以管理员身份运行）");
            }
        }

        if (attempt) {
            // 重新拼装命令行：同一个 exe + 同样的参数 + 提权子进程标记
            std::wstring parameters;
            auto append = [&parameters](const std::wstring& text) {
                if (!parameters.empty()) parameters += L' ';
                parameters += text;
            };
            append(L"watch");
            for (const std::wstring& target : resolvedTargets) {
                append(quoteCommandLineArgument(target));
            }
            append(L"--no-elevate");                                  // 子进程已提权，不再询问
            append(L"--elevated-child");                              // 结束后暂停，便于查看结果
            char number[64];
            std::snprintf(number, sizeof(number), "%.3f", options.seconds);
            append(L"--watch " + utf8ToWide(number));
            if (!options.recursive) append(L"--no-recursive");
            if (!options.pollHandles) append(L"--no-handles");
            if (!options.notifyChanges) append(L"--no-notify");
            if (options.includeSelf) append(L"--include-self");
            if (options.onlyPid != 0) append(L"--pid " + std::to_wstring(options.onlyPid));
            if (options.maxRecords != 20000) {
                append(L"--max-records " + std::to_wstring(options.maxRecords));
            }
            if (options.quiet) append(L"--quiet");
            if (options.pollHandles || options.notifyChanges) { /* 默认即为双通道 */ }
            if (!options.output.empty()) {
                append(L"-o " + quoteCommandLineArgument(fullPath(options.output)));
                append(L"-f " + utf8ToWide(formatName(options.format)));
            }
            for (const std::wstring& filter : options.filters) {
                append(L"--filter " + quoteCommandLineArgument(filter));
            }

            unsigned long elevateError = 0;
            const ElevateStatus status = relaunchElevated(parameters, &elevateError);
            if (status == ElevateStatus::Cancelled) {
                std::cout << "\n";
                printWarning("已取消管理员授权（UAC 被拒绝），本次监听未执行，返回上一层");
                return 3;
            }
            if (status == ElevateStatus::Failed) {
                std::cout << "\n";
                printError("无法以管理员身份启动：" + pathText(formatWinError(elevateError)));
                printInfo("本次监听未执行。如确实不想提权，可加 --no-elevate 以当前权限运行。");
                return 4;
            }
            printSuccess("已在新窗口中启动管理员权限的监听任务（UAC 已通过）");
            printInfo("该窗口会实时显示访问事件，结束后停留等待回车；本窗口等它跑完再返回");
            const int childCode = waitForElevatedChild();
            if (childCode >= 0) {
                printInfo("管理员监听任务已结束（退出码 " + std::to_string(childCode) + "）");
            }
            return childCode >= 0 ? childCode : 0;
        }
    }

    WatchOptions engine;
    engine.recursive = options.recursive;
    engine.pollHandles = options.pollHandles;
    engine.notifyChanges = options.notifyChanges;
    engine.includeSelf = options.includeSelf;
    engine.onlyPid = options.onlyPid;
    engine.seconds = options.seconds;
    engine.maxRecords = options.maxRecords;
    engine.filters = options.filters;
    engine.pollIntervalMs = 400;

    FileActivityMonitor monitor(resolvedTargets, engine);
    std::wstring err;
    if (!monitor.start(err)) {
        printError("无法启动监听：" + pathText(err));
        return 4;
    }

    const bool quietMode = options.quiet;
    double watchSeconds = options.seconds;
    if (watchSeconds <= 0 && !stdinIsConsole()) {
        // 无时限监听要靠键盘 Q / Ctrl+C 停止；输入被重定向时这两条路都不通，
        // 因此自动兜底成有限时长，避免留下一个停不掉的进程。
        printWarning("无时限监听需要交互式控制台才能按 Q 停止；当前标准输入不是控制台，"
                     "已自动限制为 60 秒（可用 --watch <秒> 指定时长）");
        watchSeconds = 60.0;
    }
    printSectionTitle("文件访问监听");
    printInfo("监听目标 : " + std::to_string(resolvedTargets.size()) + " 个路径");
    for (const std::wstring& target : resolvedTargets) printInfo("           " + pathText(target));
    printInfo(std::string("监听时长 : ") +
              (watchSeconds > 0 ? formatDuration(watchSeconds) + "（到时可提前按 Q 停止）"
                                   : std::string("直到按 Q / Ctrl+C 停止")));
    printInfo(std::string("探测通道 : ") + (options.pollHandles ? "句柄采样" : "") +
              (options.pollHandles && options.notifyChanges ? " + " : "") +
              (options.notifyChanges ? "目录变更通知" : ""));
    if (!monitor.stats().handleScanAvailable) {
        printWarning("当前系统不支持句柄枚举，已退化为仅目录变更通知");
    }
    if (options.pollHandles && !monitor.stats().elevated) {
        printWarning("未以管理员身份运行：只能归因当前用户可访问的进程；系统进程与其他用户"
                     "的访问不会出现。需要完整结果请用管理员身份运行。");
    }
    printInfo("说明     : 句柄采样能指出\"谁打开着它（读/写）\"；目录变更能给出\"何时被改\"，"
              "但 Windows 不提供变更发起者");

    JobControl control;
    setActiveJob(&control);
    // 进度面板真正跑起来时由它轮询键盘；否则（--no-progress / --quiet / 输出被重定向 /
    // 控制台句柄异常导致面板没能启动）必须由主循环自己轮询，否则"按 Q 停止"会失效。
    ProgressMonitor progress(control, cfg.asciiBar, cfg.progress && !options.quiet,
                             cfg.progressForce);
    progress.setHint("[Q 或 Esc] 停止监听");

    const int64_t startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    control.beginRun(L"监听文件访问");
    control.setTotals(1, static_cast<uint64_t>(std::max(1.0, watchSeconds) * 1000.0));
    progress.start("文件访问监听");

    size_t shownAccess = 0;
    size_t shownChange = 0;
    bool interrupted = false;
    while (true) {
        if (!control.checkpoint()) {
            interrupted = true;
            break;
        }
        // 面板没真正运行就自己响应键盘（面板在跑时由面板线程轮询，避免重复消费按键）
        if (!progress.isActive()) {
            const int key = pollKey();
            if (key == 'q' || key == 'Q' || key == 27) {
                interrupted = true;
                break;
            }
        }
        std::vector<AccessRecord> newAccess;
        std::vector<ChangeRecord> newChange;
        monitor.drain(newAccess, newChange);
        // 只打印"首次出现"的访问记录（samples==1 即新记录），持续占用不再每轮刷屏，
        // 完整明细在结束后的结果区统一给出。
        for (const AccessRecord& record : newAccess) {
            if (record.samples > 1) continue;
            progress.log(describeAccessRecord(record));
            ++shownAccess;
        }
        for (const ChangeRecord& record : newChange) {
            progress.log(describeChangeRecord(record));
            ++shownChange;
        }

        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        const double elapsed = static_cast<double>(nowMs - startMs) / 1000.0;
        control.addBytes(250);   // 循环固定 250ms 一次，用来推进进度条
        if (watchSeconds > 0 && elapsed >= watchSeconds) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    monitor.stop();
    progress.stop();
    control.endRun();
    setActiveJob(nullptr);

    if (interrupted) printWarning("监听被用户提前停止");

    WatchResult result = monitor.snapshot();
    result.elapsed = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count() -
                                         startMs) /
                     1000.0;
    result.cancelled = interrupted;

    printSectionTitle("监听结果");
    std::cout << "  " << paint("权限说明 : ", Color::Gray) << pathText(result.stats.note) << "\n";
    std::cout << "  句柄采样 : " << result.stats.samples << " 轮（最近一轮 "
              << result.stats.handleScanMs << " ms，命中目标句柄 " << result.stats.handlesMatched
              << " 个）\n";
    std::cout << "  访问记录 : " << paint(std::to_string(result.accesses.size()), Color::Cyan)
              << " 条（涉及 " << result.stats.processes << " 个进程 / " << result.stats.files
              << " 个文件）\n";
    std::cout << "  变更事件 : " << paint(std::to_string(result.changes.size()), Color::Magenta)
              << " 条\n";
    std::cout << "  监听时长 : " << formatDuration(result.elapsed) << "\n";
    (void)quietMode;

    if (!result.accesses.empty()) {
        std::cout << "\n" << paint("── 进程访问明细 ──", Color::Bold) << "\n";
        for (const AccessRecord& record : result.accesses) {
            std::cout << "  " << describeAccessRecord(record);
            if (!record.stillOpen) std::cout << paint("   [已关闭]", Color::Gray);
            std::cout << "\n";
        }
    } else {
        printWarning("监听期间没有观察到任何进程打开目标文件"
                     "（瞬时打开可能短于采样间隔；系统进程需要管理员权限才能归因）");
    }
    if (!result.changes.empty()) {
        std::cout << "\n" << paint("── 文件变更事件 ──", Color::Bold) << "\n";
        for (const ChangeRecord& record : result.changes) {
            std::cout << "  " << describeChangeRecord(record) << "\n";
        }
    }

    int code = 0;
    if (!options.output.empty()) {
        const std::string text = buildWatchReport(result, options.format);
        std::wstring writeErr;
        if (writeFileUtf8(options.output, text, cfg.bom, writeErr)) {
            printSuccess("已写入监听报告：" + pathText(fullPath(options.output)) + "（" +
                         formatByteSize(text.size()) + "）");
        } else {
            printError("写入报告失败：" + pathText(writeErr));
            code = 4;
        }
    }

    // 提权后的子进程跑在自己的控制台窗口里：结束时停一下，别让结果一闪而过
    if (options.pauseAtEnd && stdoutIsTerminal()) {
        std::cout << "\n" << paint("按回车键关闭此窗口…", Color::Gray) << std::flush;
        std::string line;
        std::getline(std::cin, line);
    }
    return interrupted ? 3 : code;
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
    bool   secondsSet = false;
    // watch 专用
    double   watchSeconds = 30.0;
    uint32_t watchPid = 0;
    size_t   watchMaxRecords = 20000;
    bool     watchHandles = true;
    bool     watchNotify = true;
    bool     watchSelf = false;
    ElevateMode watchElevate = ElevateMode::Ask;
    bool     watchElevatedChild = false;
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
            out.watchSeconds = out.seconds;
            out.secondsSet = true;
            continue;
        }
        if (lower == L"--watch") {
            // --watch <秒>：监听时长（0 = 直到取消）
            std::wstring value;
            if (!nextValue(value)) return false;
            out.watchSeconds = std::atof(wideToUtf8(value).c_str());
            out.secondsSet = true;
            if (out.watchSeconds < 0) out.watchSeconds = 0;
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
        if (lower == L"--pid") {
            std::wstring value;
            if (!nextValue(value)) return false;
            out.watchPid = static_cast<uint32_t>(std::strtoul(wideToUtf8(value).c_str(), nullptr, 10));
            continue;
        }
        if (lower == L"--max-records") {
            std::wstring value;
            if (!nextValue(value)) return false;
            out.watchMaxRecords =
                static_cast<size_t>(std::strtoull(wideToUtf8(value).c_str(), nullptr, 10));
            if (out.watchMaxRecords < 100) out.watchMaxRecords = 100;
            continue;
        }
        if (lower == L"--no-handles") {
            out.watchHandles = false;
            continue;
        }
        if (lower == L"--no-notify") {
            out.watchNotify = false;
            continue;
        }
        if (lower == L"--include-self") {
            out.watchSelf = true;
            continue;
        }
        if (lower == L"--elevate") {
            out.watchElevate = ElevateMode::Force;
            continue;
        }
        if (lower == L"--no-elevate") {
            out.watchElevate = ElevateMode::Never;
            continue;
        }
        if (lower == L"--elevated-child") {
            // 内部使用：提权后的子进程实例
            out.watchElevatedChild = true;
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
    if (command == "watch" || command == "monitor" || command == "w") {
        std::vector<std::wstring> targets = args.positional;
        if (targets.empty() && args.usePicker) {
            const std::vector<std::wstring> picked =
                pickFiles(L"请选择要监听的文件", true);
            targets.insert(targets.end(), picked.begin(), picked.end());
        }
        if (targets.empty()) {
            printError("请指定要监听的文件或文件夹，例如：EHsc.exe watch D:\\data --watch 60");
            return 4;
        }
        WatchRunOptions watch;
        watch.seconds = args.watchSeconds;
        watch.recursive = args.recursive;
        watch.pollHandles = args.watchHandles;
        watch.notifyChanges = args.watchNotify;
        watch.includeSelf = args.watchSelf;
        watch.onlyPid = args.watchPid;
        watch.maxRecords = args.watchMaxRecords;
        watch.filters = parseFilters(cfg.filters);
        watch.output = args.output;
        watch.format = args.format;
        watch.quiet = args.quiet;
        watch.elevate = args.watchElevate;
        watch.elevatedChild = args.watchElevatedChild;
        watch.pauseAtEnd = args.watchElevatedChild;
        return cmdWatch(cfg, targets, watch);
    }
    if (command == "selftest" || command == "test") {
        return cmdSelfTest(true);
    }
    if (command == "algos" || command == "algorithms" || command == "list") {
        return cmdListAlgos();
    }
    if (command == "config" || command == "cfg" || command == "settings") {
        const std::wstring path = configFilePath();
        FileInfo info;
        std::wstring err;
        const bool exists = getFileInfo(path, info, err);
        std::cout << "配置文件 : " << pathText(path) << "\n";
        if (exists) {
            std::cout << "状态     : 已存在（" << formatByteSize(info.size) << "）"
                      << ((info.attributes & FileAttrHidden) ? "，隐藏属性已设置" : "，未隐藏")
                      << "\n";
            std::cout << "查看方法 : 资源管理器勾选「显示隐藏的项目」，或执行 attrib -h \""
                      << pathText(path) << "\" 后直接编辑\n";
        } else {
            std::cout << "状态     : 尚未创建（首次进入交互式菜单时会自动生成，并设为隐藏）\n";
        }
        std::cout << "查找顺序 : " << pathText(configSearchDescription()) << "\n";
        std::cout << "说明     : 删除配置文件不影响任何哈希功能，程序会按默认值重新生成。\n";
        return 0;
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
    // 先把自己的标准流接回本进程的控制台：UAC 提权启动的新进程可能继承了
    // 父进程（中完整性级别）的控制台句柄，那种句柄既打印不出东西、也读不到按键。
    if (ehsc::rebindConsoleStreams()) {
        std::cerr << "[EHsc] 检测到标准流未连接到本进程的控制台（提权启动时常见），已自动重新连接。"
                  << "当前状态: 输出=" << (ehsc::stdoutIsTerminal() ? "终端窗口" : "重定向/不可用")
                  << "，输入=" << (ehsc::stdinIsConsole() ? "控制台" : "重定向/不可用") << "\n";
    }
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
