#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <wincrypt.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Comdlg32.lib")

#ifndef CALG_SHA_256
#define CALG_SHA_256 0x0000800c
#endif

#ifndef CALG_SHA_384
#define CALG_SHA_384 0x0000800d
#endif

#ifndef CALG_SHA_512
#define CALG_SHA_512 0x0000800e
#endif

struct HashItem {
    const wchar_t* name;
    ALG_ID alg;
    HCRYPTHASH handle;
};

static std::wstring GetLastErrorText(DWORD errorCode) {
    LPWSTR buffer = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr
    );

    std::wstring message;
    if (size && buffer) {
        message.assign(buffer, size);
        LocalFree(buffer);
    }
    else {
        std::wstringstream ss;
        ss << L"Unknown error: " << errorCode;
        message = ss.str();
    }
    return message;
}

static void ThrowWin32(const wchar_t* action) {
    DWORD err = GetLastError();
    std::wstringstream ss;
    ss << action << L" failed. " << GetLastErrorText(err);
    throw std::runtime_error(std::string(ss.str().begin(), ss.str().end()));
}

static std::wstring SelectFile() {
    wchar_t fileName[MAX_PATH * 4] = { 0 };

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(fileName) / sizeof(fileName[0]));
    ofn.lpstrTitle = L"Select a file to calculate hashes";
    ofn.lpstrFilter = L"All files\0*.*\0\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) {
        DWORD err = CommDlgExtendedError();
        if (err == 0) {
            return L"";
        }
        std::wstringstream ss;
        ss << L"File selection failed. CommDlgExtendedError=" << err;
        throw std::runtime_error(std::string(ss.str().begin(), ss.str().end()));
    }

    return fileName;
}

static std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return std::string();

    int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        ws.c_str(),
        static_cast<int>(ws.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (size <= 0) return std::string();

    std::string out(size, '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        ws.c_str(),
        static_cast<int>(ws.size()),
        &out[0],
        size,
        nullptr,
        nullptr
    );
    return out;
}

static std::string BytesToHex(const std::vector<BYTE>& bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);

    for (BYTE b : bytes) {
        out.push_back(hex[(b >> 4) & 0x0F]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

static uint32_t* Crc32Table() {
    static uint32_t table[256];
    static bool initialized = false;

    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                if (c & 1) {
                    c = 0xEDB88320u ^ (c >> 1);
                }
                else {
                    c >>= 1;
                }
            }
            table[i] = c;
        }
        initialized = true;
    }

    return table;
}

static uint32_t Crc32Update(uint32_t crc, const BYTE* data, DWORD size) {
    uint32_t* table = Crc32Table();
    crc = ~crc;

    for (DWORD i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }

    return ~crc;
}

static std::string UInt32ToHex(uint32_t value) {
    std::ostringstream ss;
    ss << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << value;
    return ss.str();
}

static std::string GetHashHex(HCRYPTHASH hash) {
    DWORD hashLen = 0;
    DWORD paramLen = sizeof(hashLen);

    if (!CryptGetHashParam(hash, HP_HASHSIZE, reinterpret_cast<BYTE*>(&hashLen), &paramLen, 0)) {
        ThrowWin32(L"CryptGetHashParam(HP_HASHSIZE)");
    }

    std::vector<BYTE> bytes(hashLen);
    paramLen = hashLen;

    if (!CryptGetHashParam(hash, HP_HASHVAL, bytes.data(), &paramLen, 0)) {
        ThrowWin32(L"CryptGetHashParam(HP_HASHVAL)");
    }

    return BytesToHex(bytes);
}

static bool CreateHashChecked(HCRYPTPROV provider, ALG_ID alg, HCRYPTHASH* outHash) {
    *outHash = 0;
    return CryptCreateHash(provider, alg, 0, 0, outHash) != FALSE;
}

static void PrintLine(const char* name, const std::string& value) {
    std::cout << std::left << std::setw(8) << name << " : " << value << '\n';
}

int wmain() {
    try {
        std::wstring path = SelectFile();
        if (path.empty()) {
            std::cout << "No file selected.\n";
            return 0;
        }

        HCRYPTPROV provider = 0;

        if (!CryptAcquireContextW(
            &provider,
            nullptr,
            nullptr,
            PROV_RSA_AES,
            CRYPT_VERIFYCONTEXT)) {
            ThrowWin32(L"CryptAcquireContextW(PROV_RSA_AES)");
        }

        HashItem hashes[] = {
            {L"MD5",    CALG_MD5,     0},
            {L"SHA1",   CALG_SHA1,    0},
            {L"SHA256", CALG_SHA_256, 0},
            {L"SHA384", CALG_SHA_384, 0},
            {L"SHA512", CALG_SHA_512, 0}
        };

        for (HashItem& item : hashes) {
            if (!CreateHashChecked(provider, item.alg, &item.handle)) {
                std::wcerr << L"This Windows CryptoAPI provider does not support "
                    << item.name << L".\n";
                CryptReleaseContext(provider, 0);
                return 1;
            }
        }

        HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );

        if (file == INVALID_HANDLE_VALUE) {
            for (HashItem& item : hashes) {
                if (item.handle) CryptDestroyHash(item.handle);
            }
            CryptReleaseContext(provider, 0);
            ThrowWin32(L"CreateFileW");
        }

        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(file, &fileSize)) {
            CloseHandle(file);
            for (HashItem& item : hashes) {
                if (item.handle) CryptDestroyHash(item.handle);
            }
            CryptReleaseContext(provider, 0);
            ThrowWin32(L"GetFileSizeEx");
        }

        const DWORD bufferSize = 4 * 1024 * 1024;
        std::vector<BYTE> buffer(bufferSize);
        uint32_t crc32 = 0;
        uint64_t totalRead = 0;

        for (;;) {
            DWORD bytesRead = 0;
            if (!ReadFile(file, buffer.data(), bufferSize, &bytesRead, nullptr)) {
                CloseHandle(file);
                for (HashItem& item : hashes) {
                    if (item.handle) CryptDestroyHash(item.handle);
                }
                CryptReleaseContext(provider, 0);
                ThrowWin32(L"ReadFile");
            }

            if (bytesRead == 0) {
                break;
            }

            crc32 = Crc32Update(crc32, buffer.data(), bytesRead);

            for (HashItem& item : hashes) {
                if (!CryptHashData(item.handle, buffer.data(), bytesRead, 0)) {
                    CloseHandle(file);
                    for (HashItem& cleanup : hashes) {
                        if (cleanup.handle) CryptDestroyHash(cleanup.handle);
                    }
                    CryptReleaseContext(provider, 0);
                    ThrowWin32(L"CryptHashData");
                }
            }

            totalRead += bytesRead;

            if (fileSize.QuadPart > 0) {
                int percent = static_cast<int>((totalRead * 100ULL) / static_cast<uint64_t>(fileSize.QuadPart));
                std::cout << "\rProcessing: " << std::setw(3) << percent << "%" << std::flush;
            }
        }

        CloseHandle(file);
        std::cout << "\rProcessing: 100%\n\n";

        std::cout << "File   : " << WideToUtf8(path) << '\n';
        std::cout << "Size   : " << static_cast<unsigned long long>(fileSize.QuadPart) << " bytes\n\n";

        PrintLine("MD5", GetHashHex(hashes[0].handle));
        PrintLine("CRC32", UInt32ToHex(crc32));
        PrintLine("SHA1", GetHashHex(hashes[1].handle));
        PrintLine("SHA256", GetHashHex(hashes[2].handle));
        PrintLine("SHA384", GetHashHex(hashes[3].handle));
        PrintLine("SHA512", GetHashHex(hashes[4].handle));

        for (HashItem& item : hashes) {
            if (item.handle) CryptDestroyHash(item.handle);
        }
        CryptReleaseContext(provider, 0);

        std::cout << "\nDone. Press Enter to exit...";
        std::cin.get();
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        std::cerr << "Press Enter to exit...";
        std::cin.get();
        return 1;
    }
}
