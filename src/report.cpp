// ============================================================================
//  EHsc · report.cpp
//  报告生成（TXT / CSV / JSON）、清单解析（三种格式）、清单校验。
// ============================================================================
#include "report.h"
#include "win32_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace ehsc {
namespace {

// ------------------------------------------------------------ 小工具 ----
std::string jsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string csvEscape(const std::string& text) {
    const bool needQuote = text.find_first_of(",\"\r\n") != std::string::npos;
    if (!needQuote) return text;
    std::string out = "\"";
    for (char c : text) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
}

std::string shortenHex(const std::string& hex, size_t keep = 16) {
    if (hex.size() <= keep * 2) return hex;
    return hex.substr(0, keep) + "…" + hex.substr(hex.size() - keep);
}

std::string toLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// 统一的“清单条目必需的信息”存取
struct DigestKey {
    std::wstring path;
    bool operator<(const DigestKey& other) const { return path < other.path; }
};

// ------------------------------------------------------------ CSV 解析 ----
std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    current.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                current.push_back(c);
            }
        } else if (c == '"') {
            inQuotes = true;
        } else if (c == ',') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    fields.push_back(current);
    for (std::string& field : fields) field = trimText(field);
    return fields;
}

// ------------------------------------------------------------ JSON 解析 ----
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type        type = Type::Null;
    bool        boolean = false;
    double      number = 0;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* find(const std::string& key) const {
        for (const auto& kv : object) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

void appendUtf8(std::string& out, unsigned int code) {
    if (code <= 0x7F) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    bool parse(JsonValue& out, std::string& err) {
        skipSpace();
        if (!parseValue(out)) {
            err = error_.empty() ? "JSON 语法错误" : error_;
            return false;
        }
        skipSpace();
        return true;
    }

private:
    // 读取 4 位十六进制（\\uXXXX），失败返回 false 且不改变位置
    bool readHex4(unsigned int& value) {
        if (pos_ + 4 > text_.size()) return false;
        unsigned int result = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_ + static_cast<size_t>(i)];
            result <<= 4;
            if (h >= '0' && h <= '9') result |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') result |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') result |= static_cast<unsigned>(h - 'A' + 10);
            else return false;
        }
        pos_ += 4;
        value = result;
        return true;
    }

    void skipSpace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool fail(const std::string& message) {
        if (error_.empty()) error_ = message;
        return false;
    }

    bool parseValue(JsonValue& out) {
        skipSpace();
        if (pos_ >= text_.size()) return fail("意外的文件结尾");
        const char c = text_[pos_];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parseString(out.text);
        }
        if (c == 't' || c == 'f') {
            const bool value = (c == 't');
            const char* literal = value ? "true" : "false";
            const size_t len = std::strlen(literal);
            if (text_.compare(pos_, len, literal) != 0) return fail("无效的字面量");
            pos_ += len;
            out.type = JsonValue::Type::Bool;
            out.boolean = value;
            return true;
        }
        if (c == 'n') {
            if (text_.compare(pos_, 4, "null") != 0) return fail("无效的字面量");
            pos_ += 4;
            out.type = JsonValue::Type::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* begin = text_.c_str() + pos_;
            char* end = nullptr;
            const double value = std::strtod(begin, &end);
            if (end == begin) return fail("无效的数字");
            pos_ += static_cast<size_t>(end - begin);
            out.type = JsonValue::Type::Number;
            out.number = value;
            return true;
        }
        return fail(std::string("意外的字符 '") + c + "'");
    }

    bool parseString(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') return fail("缺少字符串起始引号");
        ++pos_;
        out.clear();        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return fail("字符串转义不完整");
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return fail("\\u 转义不完整");
                    unsigned int code = 0;
                    if (!readHex4(code)) return fail("\\u 转义包含非十六进制字符");
                    // 代理对：高代理必须紧跟一个合法的 \\uXXXX 低代理
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 6 <= text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        const size_t save = pos_;
                        pos_ += 2;
                        unsigned int low = 0;
                        if (!readHex4(low)) {
                            pos_ = save;   // 不是合法低代理，按单码点处理
                        } else if (low >= 0xDC00 && low <= 0xDFFF) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            pos_ = save;
                        }
                    }
                    appendUtf8(out, code);
                    break;
                }
                default: return fail("未知的转义字符");
            }
        }
        return fail("字符串未闭合");
    }

    bool parseArray(JsonValue& out) {
        ++pos_;   // '['
        out.type = JsonValue::Type::Array;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            JsonValue item;
            if (!parseValue(item)) return false;
            out.array.push_back(std::move(item));
            skipSpace();
            if (pos_ >= text_.size()) return fail("数组未闭合");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("数组中出现意外字符");
        }
    }

    bool parseObject(JsonValue& out) {
        ++pos_;   // '{'
        out.type = JsonValue::Type::Object;
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skipSpace();
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("对象缺少冒号");
            ++pos_;
            JsonValue value;
            if (!parseValue(value)) return false;
            out.object.emplace_back(std::move(key), std::move(value));
            skipSpace();
            if (pos_ >= text_.size()) return fail("对象未闭合");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("对象中出现意外字符");
        }
    }

    const std::string& text_;
    size_t             pos_ = 0;
    std::string        error_;
};

// ------------------------------------------------------- 各类格式解析 ----
void addDigest(ManifestEntry& entry, Algo algo, const std::string& hex, Manifest& manifest) {
    const std::string clean = toLowerAscii(trimText(hex));
    if (clean.empty()) return;
    for (ManifestDigest& d : entry.digests) {
        if (d.algo == algo) {
            d.hex = clean;
            return;
        }
    }
    entry.digests.push_back(ManifestDigest{algo, clean});
    if (std::find(manifest.algos.begin(), manifest.algos.end(), algo) == manifest.algos.end())
        manifest.algos.push_back(algo);
}

Manifest parseTextManifest(const std::string& text, const std::wstring& fileNameHint) {
    Manifest manifest;
    manifest.kind = ManifestKind::Text;
    Algo current = Algo::SHA256;
    bool haveCurrent = false;

    // 文件名后缀作为默认算法提示（如 a.sha256.txt）
    const std::string hint = toLowerAscii(wideToUtf8(baseName(fileNameHint)));
    if (!hint.empty()) {
        for (int i = 0; i < kAlgoCount; ++i) {
            const Algo algo = static_cast<Algo>(i);
            if (hint.find(algoInfo(algo).id) != std::string::npos) {
                current = algo;
                haveCurrent = true;
                break;
            }
        }
    }

    size_t begin = 0;
    while (begin <= text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        begin = end + 1;
        if (line.empty()) continue;

        const std::string trimmed = trimText(line);
        if (trimmed.empty()) continue;
        if (trimmed[0] == '#' || trimmed[0] == ';') {
            const std::string lower = toLowerAscii(trimmed);
            // "# 根目录: D:\data | E:\more" 或 "# root: ..."
            const size_t rootPos = lower.find("根目录") != std::string::npos
                                       ? lower.find("根目录")
                                       : lower.find("root:");
            if (rootPos != std::string::npos) {
                const size_t colon = trimmed.find(':', rootPos);
                if (colon != std::string::npos) {
                    std::string roots = trimmed.substr(colon + 1);
                    size_t cursor = 0;
                    while (cursor <= roots.size()) {
                        size_t sep = roots.find(" | ", cursor);
                        if (sep == std::string::npos) sep = roots.size();
                        const std::string item = trimText(roots.substr(cursor, sep - cursor));
                        if (!item.empty()) manifest.roots.push_back(utf8ToWide(item));
                        cursor = sep + 3;
                    }
                }
            }
            // 识别 "# algorithm: sha256" 段落标记
            const size_t pos = lower.find("algorithm");
            if (pos != std::string::npos) {
                const size_t colon = lower.find(':', pos);
                if (colon != std::string::npos) {
                    Algo parsed{};
                    if (parseAlgo(trimmed.substr(colon + 1), parsed)) {
                        current = parsed;
                        haveCurrent = true;
                    }
                }
            }
            continue;
        }

        // 形式： <hex> [ *|  ]<path>
        size_t space = trimmed.find_first_of(" \t");
        if (space == std::string::npos) continue;
        const std::string hex = trimmed.substr(0, space);
        std::vector<uint8_t> raw;
        if (!fromHex(hex, raw)) continue;   // 非摘要行，忽略
        size_t cursor = trimmed.find_first_not_of(" \t", space);
        if (cursor == std::string::npos) continue;
        if (trimmed[cursor] == '*') ++cursor;             // 二进制模式标记
        std::string pathText = trimmed.substr(cursor);
        pathText = trimText(pathText);
        if (pathText.size() >= 2 && pathText.front() == '"' && pathText.back() == '"')
            pathText = pathText.substr(1, pathText.size() - 2);
        if (pathText.empty()) continue;

        ManifestEntry entry;
        entry.path = utf8ToWide(pathText);
        addDigest(entry, haveCurrent ? current : Algo::SHA256, hex, manifest);
        manifest.entries.push_back(std::move(entry));
    }
    manifest.ok = !manifest.entries.empty();
    if (!manifest.ok) manifest.error = L"清单中没有找到任何有效的摘要行";
    return manifest;
}

Manifest parseCsvManifest(const std::string& text, const std::wstring& /*fileNameHint*/) {
    Manifest manifest;
    manifest.kind = ManifestKind::Csv;

    std::vector<std::string> lines;
    size_t begin = 0;
    while (begin <= text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        begin = end + 1;
        if (!trimText(line).empty()) lines.push_back(line);
    }
    if (lines.empty()) {
        manifest.error = L"CSV 清单为空";
        return manifest;
    }

    // 跳过以 # 开头的注释行，找到表头；同时提取注释里的根目录
    size_t headerIndex = 0;
    while (headerIndex < lines.size() && lines[headerIndex][0] == '#') {
        const std::string lower = toLowerAscii(lines[headerIndex]);
        const size_t rootPos = lower.find("根目录") != std::string::npos
                                   ? lower.find("根目录")
                                   : lower.find("root:");
        if (rootPos != std::string::npos) {
            const size_t colon = lines[headerIndex].find(':', rootPos);
            if (colon != std::string::npos) {
                const std::string roots = lines[headerIndex].substr(colon + 1);
                size_t cursor = 0;
                while (cursor <= roots.size()) {
                    size_t sep = roots.find(" | ", cursor);
                    if (sep == std::string::npos) sep = roots.size();
                    const std::string item = trimText(roots.substr(cursor, sep - cursor));
                    if (!item.empty()) manifest.roots.push_back(utf8ToWide(item));
                    cursor = sep + 3;
                }
            }
        }
        ++headerIndex;
    }
    if (headerIndex >= lines.size()) {
        manifest.error = L"CSV 清单缺少表头";
        return manifest;
    }

    const std::vector<std::string> header = splitCsvLine(lines[headerIndex]);
    int pathColumn = -1, sizeColumn = -1, modifiedColumn = -1, algoColumn = -1, digestColumn = -1;
    int noteColumn = -1;
    std::vector<std::pair<int, Algo>> algoColumns;
    for (size_t i = 0; i < header.size(); ++i) {
        const std::string lower = toLowerAscii(header[i]);
        if (lower == "path" || lower == "file" || lower == "文件" || lower == "路径") {
            pathColumn = static_cast<int>(i);
            continue;
        }
        if (lower == "size" || lower == "bytes" || lower == "大小") {
            sizeColumn = static_cast<int>(i);
            continue;
        }
        if (lower == "modified" || lower == "mtime" || lower == "修改时间") {
            modifiedColumn = static_cast<int>(i);
            continue;
        }
        if (lower == "algorithm" || lower == "algo" || lower == "算法") {
            algoColumn = static_cast<int>(i);
            continue;
        }
        if (lower == "digest" || lower == "hash" || lower == "摘要" || lower == "checksum") {
            digestColumn = static_cast<int>(i);
            continue;
        }
        if (lower == "note" || lower == "error" || lower == "备注" || lower == "错误") {
            noteColumn = static_cast<int>(i);
            continue;
        }
        Algo parsed{};
        if (parseAlgo(header[i], parsed)) algoColumns.emplace_back(static_cast<int>(i), parsed);
    }

    if (pathColumn < 0) {
        manifest.error = L"CSV 清单缺少 Path 列";
        return manifest;
    }

    // 便于按路径合并同一文件的多个摘要（返回下标而不是引用：
    // 后续 push_back 可能让 vector 重新分配，引用会立刻失效）
    std::map<std::wstring, size_t> indexByPath;

    auto find_or_create = [&](const std::wstring& key) -> size_t {
        auto it = indexByPath.find(key);
        if (it != indexByPath.end()) return it->second;
        ManifestEntry entry;
        entry.path = key;
        const size_t index = manifest.entries.size();
        indexByPath.emplace(key, index);
        manifest.entries.push_back(std::move(entry));
        return index;
    };

    for (size_t row = headerIndex + 1; row < lines.size(); ++row) {
        const std::vector<std::string> fields = splitCsvLine(lines[row]);
        if (fields.empty()) continue;
        auto field = [&fields](int index) -> std::string {
            if (index < 0 || index >= static_cast<int>(fields.size())) return std::string();
            return fields[static_cast<size_t>(index)];
        };
        const std::string pathText = field(pathColumn);
        if (pathText.empty()) continue;
        const std::wstring widePath = utf8ToWide(pathText);
        const std::wstring note = noteColumn >= 0 ? utf8ToWide(field(noteColumn)) : std::wstring();

        if (algoColumn >= 0 && digestColumn >= 0) {
            // 长表：每行一个“文件 + 算法”记录
            Algo parsed{};
            if (!parseAlgo(field(algoColumn), parsed)) continue;
            // 即便摘要为空也要保留条目：这说明生成清单时该文件读取失败，
            // 静默丢弃会让“缺失文件”永远无法被发现。
            const size_t index = find_or_create(widePath);
            if (manifest.entries[index].error.empty()) manifest.entries[index].error = note;
            const std::string digest = field(digestColumn);
            if (!digest.empty()) addDigest(manifest.entries[index], parsed, digest, manifest);
            if (sizeColumn >= 0) {
                const std::string sizeText = field(sizeColumn);
                if (!sizeText.empty()) {
                    manifest.entries[index].size = std::strtoull(sizeText.c_str(), nullptr, 10);
                    manifest.entries[index].hasSize = true;
                }
            }
        } else {
            // 宽表：一行包含所有算法列
            const size_t index = find_or_create(widePath);
            if (manifest.entries[index].error.empty()) manifest.entries[index].error = note;
            if (sizeColumn >= 0) {
                const std::string sizeText = field(sizeColumn);
                if (!sizeText.empty()) {
                    manifest.entries[index].size = std::strtoull(sizeText.c_str(), nullptr, 10);
                    manifest.entries[index].hasSize = true;
                }
            }
            (void)modifiedColumn;
            for (const auto& column : algoColumns) {
                const std::string digest = field(column.first);
                if (!digest.empty()) addDigest(manifest.entries[index], column.second, digest, manifest);
            }
        }
    }

    manifest.ok = !manifest.entries.empty();
    if (!manifest.ok) manifest.error = L"CSV 清单中没有有效记录";
    return manifest;
}

Manifest parseJsonManifest(const std::string& text, const std::wstring& /*fileNameHint*/) {
    Manifest manifest;
    manifest.kind = ManifestKind::Json;

    JsonValue root;
    std::string error;
    JsonParser parser(text);
    if (!parser.parse(root, error) || root.type != JsonValue::Type::Object) {
        manifest.error = L"JSON 解析失败：" + utf8ToWide(error);
        return manifest;
    }

    const JsonValue* files = root.find("files");
    if (!files || files->type != JsonValue::Type::Array) {
        manifest.error = L"JSON 清单缺少 files 数组";
        return manifest;
    }
    const JsonValue* roots = root.find("roots");
    if (roots && roots->type == JsonValue::Type::Array) {
        for (const JsonValue& item : roots->array) {
            if (item.type == JsonValue::Type::String && !item.text.empty())
                manifest.roots.push_back(utf8ToWide(item.text));
        }
    }

    for (const JsonValue& item : files->array) {
        if (item.type != JsonValue::Type::Object) continue;
        ManifestEntry entry;
        const JsonValue* path = item.find("path");
        if (!path || path->type != JsonValue::Type::String) continue;
        entry.path = utf8ToWide(path->text);
        const JsonValue* size = item.find("size");
        if (size && size->type == JsonValue::Type::Number) {
            entry.size = static_cast<uint64_t>(size->number);
            entry.hasSize = true;
        }
        const JsonValue* errorField = item.find("error");
        if (errorField && errorField->type == JsonValue::Type::String) {
            entry.error = utf8ToWide(errorField->text);
        }
        const JsonValue* digests = item.find("digests");
        if (digests && digests->type == JsonValue::Type::Object) {
            for (const auto& kv : digests->object) {
                Algo parsed{};
                if (!parseAlgo(kv.first, parsed)) continue;
                if (kv.second.type != JsonValue::Type::String) continue;
                addDigest(entry, parsed, kv.second.text, manifest);
            }
        } else {
            // 兼容 “algorithm + digest” 形式
            const JsonValue* algo = item.find("algorithm");
            const JsonValue* digest = item.find("digest");
            if (algo && digest && algo->type == JsonValue::Type::String &&
                digest->type == JsonValue::Type::String) {
                Algo parsed{};
                if (parseAlgo(algo->text, parsed)) addDigest(entry, parsed, digest->text, manifest);
            }
        }
        if (!entry.digests.empty() || !entry.path.empty()) {
            // 即使没有任何摘要也保留条目：生成清单时读取失败的文件必须能被后续校验发现
            manifest.entries.push_back(std::move(entry));
        }
    }

    manifest.ok = !manifest.entries.empty();
    if (!manifest.ok) manifest.error = L"JSON 清单中没有有效记录";
    return manifest;
}

}  // namespace

// 清单里若包含弱算法，就在文件头部给出明确的安全提示
namespace {
std::string insecureListOf(const std::vector<Algo>& algos) {
    std::string list;
    for (Algo a : algos) {
        if (algoInfo(a).secure) continue;
        if (!list.empty()) list += ", ";
        list += algoInfo(a).display;
    }
    return list;
}
}  // namespace

// ============================================================ 报告生成 ====
std::string buildTextReport(const HashJobResult& result, const ReportMeta& meta) {
    std::string out;
    out += "# EHsc 校验清单 v" + meta.version + "\n";
    out += "# 生成时间: " + (meta.generated.empty() ? isoNowUtc() : meta.generated) + "\n";
    const std::string weak = insecureListOf(meta.algos);
    if (!weak.empty()) {
        out += "#\n";
        out += "# ⚠ 安全提示：本清单包含 " + weak + "。\n";
        out += "#   CRC32 / MD5 / SHA-1 已经完全不适合任何安全用途：\n";
        out += "#     · MD5 碰撞可在数秒内构造；SHA-1 已有实际碰撞攻击（SHAttered 2017）；\n";
        out += "#     · CRC32 只是线性校验和，改数据时同步改校验值即可随意伪造；\n";
        out += "#   它们仅可用于检测下载/复制过程的偶发损坏、比对副本是否一致，\n";
        out += "#   绝不能作为防篡改、数字签名或发布验证的依据；安全场景请用 SHA-256 及以上。\n";
        out += "#   注意：同时列出多个弱算法不会更安全 —— 可信度取决于最弱的一环。\n";
        out += "#\n";
    }
    if (!meta.roots.empty()) {
        out += "# 根目录: ";
        for (size_t i = 0; i < meta.roots.size(); ++i) {
            if (i) out += " | ";
            out += wideToUtf8(meta.roots[i]);
        }
        out += "\n";
    }
    out += "# 文件数: " + std::to_string(result.records.size()) + "\n";
    out += "# 算法: " + algoIdList(meta.algos) + "\n";
    out += "# 格式: <摘要> *<相对路径>\n";

    for (Algo algo : meta.algos) {
        int algoIndex = -1;
        for (size_t i = 0; i < meta.algos.size(); ++i) {
            if (meta.algos[i] == algo) algoIndex = static_cast<int>(i);
        }
        if (algoIndex < 0) continue;
        out += "# algorithm: ";
        out += algoInfo(algo).id;
        out += "\n";
        for (const HashRecord& record : result.records) {
            if (!record.ok) continue;
            if (static_cast<size_t>(algoIndex) >= record.digests.size()) continue;
            out += record.digests[static_cast<size_t>(algoIndex)];
            out += " *";
            out += wideToUtf8(record.rel.empty() ? record.path : record.rel);
            out += "\n";
        }
    }
    // 读取失败的文件无法给出摘要：在注释里明确列出，避免它们“凭空消失”。
    // （文本格式兼容 sha256sum，无法表达“无摘要的条目”；CSV / JSON 会保留条目并在校验时提示。）
    if (result.failedFiles > 0) {
        out += "#\n# 以下 " + std::to_string(result.failedFiles) +
               " 个文件在生成清单时读取失败，未记录摘要：\n";
        for (const HashRecord& record : result.records) {
            if (record.ok) continue;
            out += "#   " + wideToUtf8(record.rel.empty() ? record.path : record.rel) + "  —— " +
                   wideToUtf8(record.error) + "\n";
        }
    }
    return out;
}

std::string buildCsvReport(const HashJobResult& result, const ReportMeta& meta) {
    std::string out;
    out += "# EHsc 校验清单 v" + meta.version + " 生成于 " +
           (meta.generated.empty() ? isoNowUtc() : meta.generated) + "\n";
    const std::string weak = insecureListOf(meta.algos);
    if (!weak.empty()) {
        out += "# ⚠ 安全提示: 本清单包含 " + weak +
               "，它们已完全不适合安全用途（易被伪造/碰撞），\n";
        out += "#            仅可用于检测偶发损坏与副本一致性，不可作为防篡改依据；"
               "安全场景请使用 SHA-256 及以上。\n";
    }
    if (!meta.roots.empty()) {
        out += "# 根目录: ";
        for (size_t i = 0; i < meta.roots.size(); ++i) {
            if (i) out += " | ";
            out += wideToUtf8(meta.roots[i]);
        }
        out += "\n";
    }
    out += "Path,Size,Modified";
    for (Algo algo : meta.algos) {
        out += ",";
        out += algoInfo(algo).display;
    }
    // 有文件读取失败时才增加备注列：这些条目摘要为空，校验时会被明确指出
    const bool anyFailed = result.failedFiles > 0;
    if (anyFailed) out += ",Note";
    out += "\n";
    for (const HashRecord& record : result.records) {
        const std::wstring shown = record.rel.empty() ? record.path : record.rel;
        out += csvEscape(wideToUtf8(shown));
        out += ",";
        out += std::to_string(record.size);
        out += ",";
        out += isoTimeUtc(record.mtime);
        for (size_t i = 0; i < meta.algos.size(); ++i) {
            out += ",";
            if (record.ok && i < record.digests.size()) out += record.digests[i];
        }
        if (anyFailed) {
            out += ",";
            if (!record.ok) out += csvEscape(wideToUtf8(record.error));
        }
        out += "\n";
    }
    return out;
}

std::string buildJsonReport(const HashJobResult& result, const ReportMeta& meta) {
    std::string out;
    out += "{\n";
    out += "  \"tool\": \"" + jsonEscape(meta.tool) + "\",\n";
    out += "  \"version\": \"" + jsonEscape(meta.version) + "\",\n";
    out += "  \"generated\": \"" +
           jsonEscape(meta.generated.empty() ? isoNowUtc() : meta.generated) + "\",\n";
    if (!meta.host.empty()) out += "  \"host\": \"" + jsonEscape(meta.host) + "\",\n";
    out += "  \"roots\": [";
    for (size_t i = 0; i < meta.roots.size(); ++i) {
        if (i) out += ", ";
        out += "\"" + jsonEscape(wideToUtf8(meta.roots[i])) + "\"";
    }
    out += "],\n";
    out += "  \"algorithms\": [";
    for (size_t i = 0; i < meta.algos.size(); ++i) {
        if (i) out += ", ";
        out += "\"" + std::string(algoInfo(meta.algos[i]).id) + "\"";
    }
    out += "],\n";
    {
        const std::string weak = insecureListOf(meta.algos);
        if (!weak.empty()) {
            out += "  \"securityWarning\": \"本清单包含 " + jsonEscape(weak) +
                   "：CRC32/MD5/SHA-1 已完全不适合安全用途，"
                   "仅可用于文件完整性检查，不可作为防篡改或签名依据。\",\n";
        }
        out += "  \"secure\": ";
        out += weak.empty() ? "true" : "false";
        out += ",\n";
    }
    out += "  \"fileCount\": " + std::to_string(result.records.size()) + ",\n";
    out += "  \"totalBytes\": " + std::to_string(result.totalBytes) + ",\n";
    out += "  \"failedCount\": " + std::to_string(result.failedFiles) + ",\n";
    out += "  \"elapsedSeconds\": ";
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.3f", result.elapsed);
        out += buffer;
    }
    out += ",\n";
    out += "  \"files\": [\n";
    for (size_t index = 0; index < result.records.size(); ++index) {
        const HashRecord& record = result.records[index];
        const std::wstring shown = record.rel.empty() ? record.path : record.rel;
        out += "    {\n";
        out += "      \"path\": \"" + jsonEscape(wideToUtf8(shown)) + "\",\n";
        out += "      \"size\": " + std::to_string(record.size) + ",\n";
        out += "      \"modified\": \"" + isoTimeUtc(record.mtime) + "\",\n";
        out += "      \"ok\": ";
        out += record.ok ? "true" : "false";
        out += ",\n";
        if (!record.ok) {
            out += "      \"error\": \"" + jsonEscape(wideToUtf8(record.error)) + "\",\n";
        }
        out += "      \"digests\": {";
        bool first = true;
        for (size_t i = 0; i < meta.algos.size() && i < record.digests.size(); ++i) {
            if (!first) out += ", ";
            first = false;
            out += "\"" + std::string(algoInfo(meta.algos[i]).id) + "\": \"" + record.digests[i] +
                   "\"";
        }
        out += "}\n";
        out += "    }";
        if (index + 1 < result.records.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

std::string buildReport(const HashJobResult& result, const ReportMeta& meta, OutFormat format) {
    switch (format) {
        case OutFormat::Csv: return buildCsvReport(result, meta);
        case OutFormat::Json: return buildJsonReport(result, meta);
        case OutFormat::Text: return buildTextReport(result, meta);
    }
    return buildTextReport(result, meta);
}

// ============================================================ 清单解析 ====
Manifest parseManifestText(const std::string& text, const std::wstring& fileNameHint) {
    // 自动识别格式
    size_t pos = 0;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' ||
                                 text[pos] == '\n'))
        ++pos;
    if (pos < text.size() && text[pos] == '{') return parseJsonManifest(text, fileNameHint);

    // 找到第一行非注释内容，判断是否 CSV 表头
    size_t lineBegin = pos;
    while (lineBegin < text.size()) {
        size_t lineEnd = text.find('\n', lineBegin);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        const std::string line = trimText(text.substr(lineBegin, lineEnd - lineBegin));
        if (!line.empty() && line[0] != '#') {
            const std::string lower = toLowerAscii(line);
            const bool csvLike = line.find(',') != std::string::npos &&
                                 (lower.find("path") != std::string::npos ||
                                  lower.find("file") != std::string::npos);
            if (csvLike) return parseCsvManifest(text, fileNameHint);
            break;
        }
        lineBegin = lineEnd + 1;
    }
    return parseTextManifest(text, fileNameHint);
}

Manifest parseManifestFile(const std::wstring& path) {
    Manifest manifest;
    std::string text;
    std::wstring err;
    if (!readFileUtf8(path, text, err)) {
        manifest.error = L"无法读取清单文件：" + err;
        return manifest;
    }
    return parseManifestText(text, path);
}

// ============================================================ 清单校验 ====
VerifyResult verifyManifest(const Manifest& manifest, const std::wstring& baseDir,
                            const VerifyOptions& options, JobControl& control) {
    VerifyResult result;
    const auto start = std::chrono::steady_clock::now();

    // 汇总：同一路径的多个算法合并为一次读取
    struct Pending {
        std::wstring       fullPath;
        std::wstring       rel;
        std::wstring       note;      // 清单里记录的生成期错误
        uint64_t           size = 0;
        bool               hasSize = false;
        std::vector<Algo>  algos;
        std::vector<std::string> expected;
        std::vector<Algo>  allAlgos;
        std::vector<std::string> allExpected;
    };
    std::vector<Pending> pending;
    std::map<std::wstring, size_t> indexByPath;

    // 相对路径基准目录的选取（只选一次，之后严格解析，绝不“另找一个同名文件”）：
    //   1) 用户显式给出的 --base 目录优先
    //   2) 清单自身记录的根目录中第一个真实存在的目录
    //   3) 清单所在目录
    std::wstring effectiveBase = baseDir;
    if (!options.baseDirIsExplicit) {
        for (const std::wstring& root : manifest.roots) {
            if (isDirectory(root)) {
                effectiveBase = root;
                break;
            }
        }
    }
    result.resolvedBase = effectiveBase;

    auto resolvePath = [&](const std::wstring& recorded) -> std::wstring {
        if (isAbsolutePath(recorded)) return recorded;
        if (effectiveBase.empty()) return recorded;
        return joinPath(effectiveBase, recorded);
    };

    for (const ManifestEntry& entry : manifest.entries) {
        const std::wstring full = resolvePath(entry.path);
        const std::wstring key = full;
        auto it = indexByPath.find(key);
        size_t index;
        if (it == indexByPath.end()) {
            Pending item;
            item.fullPath = full;
            item.rel = entry.path;
            item.note = entry.error;
            item.size = entry.size;
            item.hasSize = entry.hasSize;
            index = pending.size();
            indexByPath.emplace(key, index);
            pending.push_back(std::move(item));
        } else {
            index = it->second;
        }
        Pending& item = pending[index];
        if (item.note.empty()) item.note = entry.error;
        for (const ManifestDigest& digest : entry.digests) {
            item.allAlgos.push_back(digest.algo);
            item.allExpected.push_back(digest.hex);
        }
    }

    // 应用算法过滤
    for (Pending& item : pending) {
        for (size_t i = 0; i < item.allAlgos.size(); ++i) {
            if (!options.filterAlgos.empty() &&
                std::find(options.filterAlgos.begin(), options.filterAlgos.end(), item.allAlgos[i]) ==
                    options.filterAlgos.end())
                continue;
            item.algos.push_back(item.allAlgos[i]);
            item.expected.push_back(item.allExpected[i]);
        }
    }

    result.total = pending.size();
    std::vector<VerifyItem> slot(pending.size());

    uint64_t knownBytes = 0;
    for (const Pending& item : pending) {
        if (item.hasSize) knownBytes += item.size;
    }

    control.beginRun(L"校验清单");
    control.setTotals(pending.size(), knownBytes);

    parallelFor(pending.size(), effectiveThreadCount(options.threads), [&](size_t index) {
        const Pending& item = pending[index];
        VerifyItem& out = slot[index];
        out.path = item.fullPath;
        out.rel = item.rel;
        out.manifestNote = item.note;
        out.algos = item.algos;
        out.expected = item.expected;
        out.size = item.size;

        if (item.algos.empty()) {
            out.status = VerifyStatus::Skipped;
            out.detail = item.note.empty()
                             ? std::wstring(L"清单中没有可用的摘要算法（可能被 -a 过滤）")
                             : (L"清单未记录摘要（生成时读取失败：" + item.note + L"）");
            control.fileFinished(true);
            return;
        }

        FileInfo info;
        std::wstring err;
        if (!getFileInfo(item.fullPath, info, err)) {
            out.status = VerifyStatus::Missing;
            out.detail = L"文件不存在或无法访问：" + err;
            control.fileFinished(false);
            return;
        }
        if (info.isDir) {
            out.status = VerifyStatus::Skipped;
            out.detail = L"这是文件夹，已跳过";
            control.fileFinished(true);
            return;
        }
        out.size = info.size;
        control.setCurrentFile(item.fullPath, info.size);

        const SingleHashResult hash = hashSingleFile(item.fullPath, item.algos, options.bufferSize,
                                                     true, &control, true);
        if (!hash.ok) {
            out.status = hash.cancelled ? VerifyStatus::Skipped : VerifyStatus::ReadError;
            out.detail = hash.error;
            control.fileFinished(hash.cancelled);   // 取消不算失败
            return;
        }
        out.actual = hash.digests;
        out.seconds = hash.seconds;

        std::wstring detail;
        bool sizeBad = false;
        if (options.checkSize && item.hasSize && item.size != info.size) {
            sizeBad = true;
            detail += L"大小不符（清单 " + utf8ToWide(formatCount(item.size)) + L" 字节，实际 " +
                      utf8ToWide(formatCount(info.size)) + L" 字节）";
        }
        for (size_t i = 0; i < item.algos.size() && i < hash.digests.size(); ++i) {
            const std::string actual = toLowerAscii(hash.digests[i]);
            const std::string expected = toLowerAscii(item.expected[i]);
            if (actual != expected) {
                if (!detail.empty()) detail += L"；";
                detail += utf8ToWide(std::string(algoInfo(item.algos[i]).display)) + L" 不一致（期望 " +
                          utf8ToWide(shortenHex(expected)) + L"，实际 " + utf8ToWide(shortenHex(actual)) +
                          L"）";
            }
        }
        if (!detail.empty()) {
            out.status = sizeBad && detail.find(L"不一致") == std::wstring::npos
                             ? VerifyStatus::SizeMismatch
                             : VerifyStatus::Mismatch;
            out.detail = detail;
            control.fileFinished(false);
        } else {
            out.status = VerifyStatus::Passed;
            out.detail = L"校验通过";
            control.fileFinished(true);
        }
    });

    control.endRun();
    result.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    result.cancelled = control.cancelled();

    for (const VerifyItem& item : slot) {
        result.bytesChecked += item.size;
        switch (item.status) {
            case VerifyStatus::Passed: result.passed++; break;
            case VerifyStatus::Mismatch:
            case VerifyStatus::SizeMismatch: result.failed++; break;
            case VerifyStatus::Missing: result.missing++; break;
            case VerifyStatus::ReadError: result.errors++; break;
            case VerifyStatus::Skipped: result.skipped++; break;
        }
        if (options.reportAll || item.status != VerifyStatus::Passed) {
            result.items.push_back(item);
        }
    }
    return result;
}

}  // namespace ehsc
