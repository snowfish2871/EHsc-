#pragma once

#include <filesystem>
#include <string>

struct HashResult
{
    std::string md5;
    std::string crc32;
    std::string sha1;
    std::string sha256;
    std::string sha384;
    std::string sha512;
};

// 单次读取文件，同时计算全部哈希值。
bool CalculateFileHashes(
    const std::filesystem::path& filePath,
    HashResult& result,
    std::string& errorMessage
);

// 逐字节精确比较两个文件。
bool FilesExactlyEqual(
    const std::filesystem::path& first,
    const std::filesystem::path& second,
    bool& equal,
    std::string& errorMessage
);
