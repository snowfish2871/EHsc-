#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <commdlg.h>
#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "hashes.hpp"

#pragma comment(lib, "Comdlg32.lib")

namespace
{
    std::filesystem::path SelectFile(const wchar_t* title)
    {
        // 预留较大的路径缓冲区，支持较长文件名。
        std::vector<wchar_t> fileBuffer(32768, L'\0');

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = GetConsoleWindow();
        dialog.lpstrFile = fileBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(fileBuffer.size());
        dialog.lpstrFilter =
            L"所有文件 (*.*)\0"
            L"*.*\0"
            L"\0";
        dialog.nFilterIndex = 1;
        dialog.lpstrTitle = title;
        dialog.Flags =
            OFN_EXPLORER |
            OFN_FILEMUSTEXIST |
            OFN_PATHMUSTEXIST |
            OFN_HIDEREADONLY |
            OFN_NOCHANGEDIR;

        if (!GetOpenFileNameW(&dialog))
        {
            return {};
        }

        return std::filesystem::path(fileBuffer.data());
    }

    std::wstring ToWideASCII(const std::string& text)
    {
        return std::wstring(text.begin(), text.end());
    }

    std::wstring FormatFileSize(std::uintmax_t bytes)
    {
        constexpr long double KB = 1024.0L;
        constexpr long double MB = KB * 1024.0L;
        constexpr long double GB = MB * 1024.0L;
        constexpr long double TB = GB * 1024.0L;

        std::wostringstream stream;
        stream << std::fixed << std::setprecision(2);

        const long double value = static_cast<long double>(bytes);

        if (value >= TB)
        {
            stream << value / TB << L" TiB";
        }
        else if (value >= GB)
        {
            stream << value / GB << L" GiB";
        }
        else if (value >= MB)
        {
            stream << value / MB << L" MiB";
        }
        else if (value >= KB)
        {
            stream << value / KB << L" KiB";
        }
        else
        {
            stream.unsetf(std::ios::floatfield);
            stream << bytes << L" 字节";
        }

        return stream.str();
    }

    void PrintHashResult(const HashResult& result)
    {
        std::wcout
            << L"CRC32  : " << ToWideASCII(result.crc32) << L'\n'
            << L"MD5    : " << ToWideASCII(result.md5) << L'\n'
            << L"SHA1   : " << ToWideASCII(result.sha1) << L'\n'
            << L"SHA256 : " << ToWideASCII(result.sha256) << L'\n'
            << L"SHA384 : " << ToWideASCII(result.sha384) << L'\n'
            << L"SHA512 : " << ToWideASCII(result.sha512) << L'\n';
    }

    void CalculateSingleFile()
    {
        const auto filePath = SelectFile(L"选择需要计算哈希值的文件");

        if (filePath.empty())
        {
            std::wcout << L"\n已取消选择。\n";
            return;
        }

        std::error_code sizeError;
        const auto fileSize =
            std::filesystem::file_size(filePath, sizeError);

        std::wcout
            << L"\n文件：\n"
            << filePath.wstring()
            << L"\n";

        if (!sizeError)
        {
            std::wcout
                << L"大小："
                << FormatFileSize(fileSize)
                << L"（"
                << fileSize
                << L" 字节）\n";
        }

        std::wcout << L"\n正在计算，请稍候……\n\n";

        HashResult result;
        std::string errorMessage;

        const auto start = std::chrono::steady_clock::now();

        const bool success = CalculateFileHashes(
            filePath,
            result,
            errorMessage
        );

        const auto end = std::chrono::steady_clock::now();

        if (!success)
        {
            std::wcout
                << L"计算失败："
                << ToWideASCII(errorMessage)
                << L"\n";
            return;
        }

        PrintHashResult(result);

        const std::chrono::duration<double> elapsed = end - start;

        std::wcout
            << L"\n耗时："
            << std::fixed
            << std::setprecision(3)
            << elapsed.count()
            << L" 秒\n";

        if (!sizeError && elapsed.count() > 0.0)
        {
            const double mib =
                static_cast<double>(fileSize) /
                (1024.0 * 1024.0);

            std::wcout
                << L"读取及计算速度："
                << std::fixed
                << std::setprecision(2)
                << mib / elapsed.count()
                << L" MiB/s\n";
        }
    }

    void PrintComparison(
        const wchar_t* name,
        const std::string& first,
        const std::string& second)
    {
        const bool same = first == second;

        std::wcout
            << std::left
            << std::setw(8)
            << name
            << L": "
            << (same ? L"相同" : L"不同")
            << L'\n';
    }

    void CompareTwoFiles()
    {
        const auto firstPath = SelectFile(L"选择第一个文件");

        if (firstPath.empty())
        {
            std::wcout << L"\n已取消选择。\n";
            return;
        }

        const auto secondPath = SelectFile(L"选择第二个文件");

        if (secondPath.empty())
        {
            std::wcout << L"\n已取消选择。\n";
            return;
        }

        std::wcout
            << L"\n第一个文件：\n"
            << firstPath.wstring()
            << L"\n\n第二个文件：\n"
            << secondPath.wstring()
            << L"\n\n正在计算并比较，请稍候……\n";

        HashResult firstResult;
        HashResult secondResult;
        std::string errorMessage;

        const auto start = std::chrono::steady_clock::now();

        if (!CalculateFileHashes(
            firstPath,
            firstResult,
            errorMessage))
        {
            std::wcout
                << L"\n第一个文件计算失败："
                << ToWideASCII(errorMessage)
                << L"\n";
            return;
        }

        if (!CalculateFileHashes(
            secondPath,
            secondResult,
            errorMessage))
        {
            std::wcout
                << L"\n第二个文件计算失败："
                << ToWideASCII(errorMessage)
                << L"\n";
            return;
        }

        std::wcout << L"\n哈希对比结果：\n";

        PrintComparison(
            L"CRC32",
            firstResult.crc32,
            secondResult.crc32
        );

        PrintComparison(
            L"MD5",
            firstResult.md5,
            secondResult.md5
        );

        PrintComparison(
            L"SHA1",
            firstResult.sha1,
            secondResult.sha1
        );

        PrintComparison(
            L"SHA256",
            firstResult.sha256,
            secondResult.sha256
        );

        PrintComparison(
            L"SHA384",
            firstResult.sha384,
            secondResult.sha384
        );

        PrintComparison(
            L"SHA512",
            firstResult.sha512,
            secondResult.sha512
        );

        bool exactlyEqual = false;

        if (!FilesExactlyEqual(
            firstPath,
            secondPath,
            exactlyEqual,
            errorMessage))
        {
            std::wcout
                << L"\n逐字节比较失败："
                << ToWideASCII(errorMessage)
                << L"\n";
            return;
        }

        const auto end = std::chrono::steady_clock::now();
        const std::chrono::duration<double> elapsed = end - start;

        std::wcout
            << L"\n逐字节精确比较："
            << (exactlyEqual
                ? L"两个文件内容完全相同"
                : L"两个文件内容不同")
            << L"\n";

        std::wcout
            << L"总耗时："
            << std::fixed
            << std::setprecision(3)
            << elapsed.count()
            << L" 秒\n";
    }

    void PrintMenu()
    {
        std::wcout
            << L"\n"
            << L"========================================\n"
            << L"        离线文件哈希计算与比较工具\n"
            << L"========================================\n"
            << L"1. 计算单个文件的全部哈希值\n"
            << L"2. 比较两个文件\n"
            << L"0. 退出\n"
            << L"========================================\n"
            << L"请输入选项：";
    }
}

int wmain()
{
    // 让 Windows 控制台使用宽字符输出，避免中文路径乱码。
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);

    while (true)
    {
        PrintMenu();

        int option = -1;

        if (!(std::wcin >> option))
        {
            std::wcin.clear();
            std::wcin.ignore(65536, L'\n');

            std::wcout << L"\n输入无效，请重新输入。\n";
            continue;
        }

        switch (option)
        {
        case 1:
            CalculateSingleFile();
            break;

        case 2:
            CompareTwoFiles();
            break;

        case 0:
            std::wcout << L"\n程序已退出。\n";
            return 0;

        default:
            std::wcout << L"\n未知选项，请重新输入。\n";
            break;
        }
    }
}
