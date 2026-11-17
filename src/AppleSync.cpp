// ============================================================
// AppleSync 实现 —— tar 解析 / bplist(.strings) 解析 / 映射 / 合并 / 编排
// ============================================================

#include "AppleSync.h"

#include <windows.h>
#include <wininet.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <set>

#include "MiniJson.h"

#pragma comment(lib, "wininet.lib")

namespace apple_sync {
namespace {

constexpr wchar_t kCatalogBase[] = L"https://sylvan.apple.com/Aerials/";
constexpr int kFallbackResourceVersion = 16;
constexpr size_t kMaxTarBytes = 64ULL << 20;  // 资源包实测 ~3MB，上限 64MB
constexpr size_t kMaxBplistObjects = 1ULL << 20;
constexpr size_t kMaxBplistOffsets = 16ULL << 20;

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

// 64 位大端读取（bplist 数字/偏移均为大端）
uint64_t ReadBE(const char* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    }
    return v;
}

// ------------------------------------------------------------
// bplist（二进制 plist）最小子集解析器
//
// 布局：对象区（0x5x ASCII 串 / 0x6x UTF-16BE 串 / 0xDx dict）+
// 偏移表 + 末 32 字节 trailer（+6=offsetIntSize、+7=refSize、
// +8/+16/+24 为 8B 大端 numObjects/topObject/offsetTableOffset）。
// dict(0xDn) 对象后跟 2n 个 ref，keys-then-values（前 n 键后 n 值）。
// ------------------------------------------------------------
struct Bplist {
    const std::string& bytes;
    std::vector<size_t> offsets;
    size_t refSize = 0;
    uint64_t top = 0;

    // 对象 objRef 处的内联长度（0xF nibble 后随内联 int 0x1n，n≤4）
    bool LengthAt(size_t objRef, size_t nibble, size_t& len, size_t& headerEnd) const {
        if (nibble < 0xF) {
            len = nibble;
            headerEnd = objRef + 1;
            return true;
        }
        if (objRef + 2 > bytes.size()) return false;
        unsigned char marker = static_cast<unsigned char>(bytes[objRef + 1]);
        if ((marker & 0xF0) != 0x10) return false;
        size_t n = static_cast<size_t>(marker & 0x0F);
        if (n > 4 || objRef + 2 + n > bytes.size()) return false;
        len = static_cast<size_t>(ReadBE(bytes.data() + objRef + 2, n));
        headerEnd = objRef + 2 + n;
        return true;
    }

    // 字符串对象（0x5x ASCII / 0x6x UTF-16BE）→ UTF-16
    bool StringAt(size_t objRef, std::wstring& out) const {
        if (objRef >= bytes.size()) return false;
        unsigned char marker = static_cast<unsigned char>(bytes[objRef]);
        if ((marker & 0xF0) != 0x50 && (marker & 0xF0) != 0x60) return false;
        size_t len = 0, data = 0;
        if (!LengthAt(objRef, marker & 0x0F, len, data)) return false;
        if ((marker & 0xF0) == 0x50) {  // ASCII，高位补 0
            if (data + len > bytes.size()) return false;
            out.assign(bytes.begin() + data, bytes.begin() + data + len);
            return true;
        }
        if (len > (bytes.size() - data) / 2) return false;
        out.resize(len);
        for (size_t i = 0; i < len; ++i) {
            out[i] = static_cast<wchar_t>(
                ReadBE(bytes.data() + data + i * 2, 2));  // 逐单元大端
        }
        return true;
    }

    // dict 对象的 keys/values ref 段起始位置与键值对数
    bool DictAt(size_t objRef, size_t& refsStart, size_t& count) const {
        if (objRef >= bytes.size()) return false;
        unsigned char marker = static_cast<unsigned char>(bytes[objRef]);
        if ((marker & 0xF0) != 0xD0) return false;
        size_t headerEnd = 0;
        if (!LengthAt(objRef, marker & 0x0F, count, headerEnd)) return false;
        if (count > (bytes.size() - headerEnd) / refSize / 2) return false;
        refsStart = headerEnd;
        return true;
    }

    uint64_t RefAt(size_t pos) const {
        return ReadBE(bytes.data() + pos, refSize);
    }
};

// trailer 校验 + 偏移表装载；失败返回 false
bool BplistInit(const std::string& bytes, Bplist& bp) {
    if (bytes.size() < 8 + 32) return false;  // 头 8B + trailer 32B
    if (bytes.compare(0, 8, "bplist00", 8) != 0) return false;
    const char* trailer = bytes.data() + bytes.size() - 32;
    size_t offsetIntSize = static_cast<unsigned char>(trailer[6]);
    bp.refSize = static_cast<unsigned char>(trailer[7]);
    if (offsetIntSize < 1 || offsetIntSize > 8 || bp.refSize < 1 || bp.refSize > 8)
        return false;
    uint64_t numObjects = ReadBE(trailer + 8, 8);
    bp.top = ReadBE(trailer + 16, 8);
    uint64_t offsetTable = ReadBE(trailer + 24, 8);
    if (numObjects == 0 || numObjects > kMaxBplistObjects) return false;
    if (bp.top >= numObjects) return false;
    size_t tableBytes = static_cast<size_t>(numObjects) * offsetIntSize;
    if (offsetTable > bytes.size() || tableBytes > bytes.size() - offsetTable)
        return false;
    if (tableBytes > kMaxBplistOffsets) return false;
    bp.offsets.resize(static_cast<size_t>(numObjects));
    for (size_t i = 0; i < numObjects; ++i) {
        bp.offsets[i] = static_cast<size_t>(
            ReadBE(bytes.data() + offsetTable + i * offsetIntSize, offsetIntSize));
        if (bp.offsets[i] >= bytes.size()) return false;
    }
    return true;
}

// 八进制长度字段（tar 头 156..163 前的 12B，允许前导空格/NUL）
bool OctalAt(const char* p, size_t n, size_t& out) {
    size_t v = 0;
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        char c = p[i];
        if (c == ' ' || c == '\0') {
            if (any) break;  // 数字后的填充
            continue;
        }
        if (c < '0' || c > '7') return false;
        any = true;
        v = v * 8 + static_cast<size_t>(c - '0');
    }
    out = v;
    return true;
}

}  // namespace

// ------------------------------------------------------------
// tar_extract：512B 头 + 八进制 size + 块对齐；name 全等或 "/"+name 后缀命中
// ------------------------------------------------------------
bool tar_extract(const std::vector<char>& tar, const std::string& name,
                 std::string& out) {
    constexpr size_t kBlock = 512;
    size_t pos = 0;
    while (pos + kBlock <= tar.size()) {
        const char* h = tar.data() + pos;
        if (h[0] == '\0') return false;  // 全零块 = 归档结束
        // ustar 长路径：prefix(345..499) + '/' + name(0..99)
        std::string entryName;
        if (h[257] != '\0' && strncmp(h + 257, "ustar", 5) == 0) {
            size_t pfxLen = 0;
            while (pfxLen < 155 && h[345 + pfxLen] != '\0') ++pfxLen;
            if (pfxLen > 0) {
                entryName.assign(h + 345, pfxLen);
                entryName.push_back('/');
            }
        }
        size_t nameLen = 0;
        while (nameLen < 100 && h[nameLen] != '\0') ++nameLen;
        entryName.append(h, nameLen);

        size_t size = 0;
        if (!OctalAt(h + 124, 12, size)) return false;
        if (size > kMaxTarBytes) return false;
        size_t dataStart = pos + kBlock;
        const size_t padded = ((size + kBlock - 1) / kBlock) * kBlock;
        if (padded > tar.size() - dataStart) return false;  // 截断/损坏（含块对齐填充）

        char type = h[156];
        if (type != '5' && type != 'L') {
            bool hit = (entryName == name);
            if (!hit && entryName.size() > name.size()) {
                const size_t tail = entryName.size() - name.size();
                hit = entryName[tail - 1] == '/' &&
                      entryName.compare(tail, std::string::npos, name) == 0;
            }
            if (hit) {
                out.assign(tar.data() + dataStart, size);
                return true;
            }
        }
        pos = dataStart + padded;  // 块对齐
    }
    return false;
}

// ------------------------------------------------------------
// parse_strings：.strings = bplist00 的 dict<string,string>
// （顶层须 dict；非字符串键/值对跳过）
// ------------------------------------------------------------
bool parse_strings(const std::string& bytes,
                   std::map<std::wstring, std::wstring>& out) {
    Bplist bp{bytes};
    if (!BplistInit(bytes, bp)) return false;
    size_t refsStart = 0, count = 0;
    if (!bp.DictAt(bp.offsets[static_cast<size_t>(bp.top)], refsStart, count))
        return false;
    for (size_t i = 0; i < count; ++i) {
        uint64_t k = bp.RefAt(refsStart + i * bp.refSize);
        uint64_t v = bp.RefAt(refsStart + (count + i) * bp.refSize);
        if (k >= bp.offsets.size() || v >= bp.offsets.size()) return false;
        std::wstring key, val;
        if (!bp.StringAt(bp.offsets[static_cast<size_t>(k)], key) ||
            !bp.StringAt(bp.offsets[static_cast<size_t>(v)], val)) {
            continue;  // 非字符串键值对跳过
        }
        out[key] = val;
    }
    return true;
}

// ------------------------------------------------------------
// 紧凑 JSON 序列化（键序固定、无空白，与现有 apples.json 样式一致）
// ------------------------------------------------------------
namespace {

// 字符串先转 UTF-8，仅转义 ASCII 特殊字符，其余字节原样输出
void DumpString(const std::wstring& s, std::string& out) {
    const std::string u8 = WideToUtf8(s);
    out.push_back('"');
    for (char c : u8) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x",
                         static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

void DumpNumber(double v, std::string& out) {
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, res.ptr);
}

// 任意 JSON 值按自身键序原样紧凑回写（旧条目无损保留）
void DumpJsonValue(const json::Value& v, std::string& out) {
    switch (v.type) {
    case json::Value::Type::Null:
        out += "null";
        break;
    case json::Value::Type::Bool:
        out += v.b ? "true" : "false";
        break;
    case json::Value::Type::Number:
        DumpNumber(v.num, out);
        break;
    case json::Value::Type::String:
        DumpString(v.str, out);
        break;
    case json::Value::Type::Array:
        out.push_back('[');
        for (size_t i = 0; i < v.arr.size(); ++i) {
            if (i) out.push_back(',');
            DumpJsonValue(v.arr[i], out);
        }
        out.push_back(']');
        break;
    case json::Value::Type::Object:
        out.push_back('{');
        for (size_t i = 0; i < v.obj.size(); ++i) {
            if (i) out.push_back(',');
            DumpString(v.obj[i].first, out);
            out.push_back(':');
            DumpJsonValue(v.obj[i].second, out);
        }
        out.push_back('}');
        break;
    }
}

// 条目 → {"id","label","category","url","hevc","uhd","dv","captions"}
void ClipToValue(const AppleClip& c, std::string& out) {
    out += "{\"id\":";
    DumpString(c.id, out);
    out += ",\"label\":";
    DumpString(c.label, out);
    out += ",\"category\":";
    DumpString(c.category, out);
    out += ",\"url\":";
    DumpString(c.url, out);
    out += ",\"hevc\":";
    DumpString(c.hevc, out);
    out += ",\"uhd\":";
    DumpString(c.uhd, out);
    out += ",\"dv\":";
    DumpString(c.dv, out);
    out += ",\"captions\":[";
    for (size_t i = 0; i < c.captions.size(); ++i) {
        if (i) out.push_back(',');
        out.push_back('[');
        DumpNumber(c.captions[i].timeSec, out);
        out.push_back(',');
        DumpString(c.captions[i].zh, out);
        out.push_back(',');
        DumpString(c.captions[i].en, out);
        out.push_back(']');
    }
    out += "]}";
}

// 分类 UUID → slug：已知四项精确映射，未知回退去掉 AerialCategory 前缀小写
std::wstring CategorySlug(const std::wstring& uuid) {
    if (uuid == L"55B7C95D-CEAF-4FD8-ADEF-F5BC657D8F6D") return L"space";
    if (uuid == L"A33A55D9-EDEA-4596-A850-6C10B54FBBB5") return L"landscapes";
    if (uuid == L"5EF41171-4862-4F93-800C-AD86CE5E6891") return L"cities";
    if (uuid == L"8BE8B524-6EAE-43F5-A3E8-01DCFA1BCD4B") return L"underwater";
    std::wstring s = uuid;
    const std::wstring prefix = L"AerialCategory";
    if (s.compare(0, prefix.size(), prefix) == 0) s.erase(0, prefix.size());
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

}  // namespace

// ------------------------------------------------------------
// clips_from_entries：entries.json → AppleClip 列表
// ------------------------------------------------------------
bool clips_from_entries(const std::string& entriesJson,
                        const std::map<std::wstring, std::wstring>& zh,
                        const std::map<std::wstring, std::wstring>& en,
                        std::vector<AppleClip>& out) {
    json::Value root;
    if (!json::Parse(entriesJson, root)) return false;
    const json::Value* assets = root.Find(L"assets");
    if (!assets || !assets->IsArray()) return false;

    out.clear();
    for (const json::Value& a : assets->arr) {
        AppleClip c;
        c.id = a.GetStr(L"id");
        if (c.id.empty()) continue;
        c.label = a.GetStr(L"accessibilityLabel");
        c.url = a.GetStr(L"url-1080-H264");   // 保底档
        c.hevc = a.GetStr(L"url-1080-SDR");
        c.uhd = a.GetStr(L"url-4K-SDR");
        c.dv = a.GetStr(L"url-4K-HDR");
        if (c.url.empty()) continue;  // 无保底档不可用

        // 分类：每资产恰 1 个 UUID，映射为 slug
        const json::Value* cats = a.Find(L"categories");
        if (cats && cats->IsArray() && !cats->arr.empty() &&
            cats->arr[0].IsString()) {
            c.category = CategorySlug(cats->arr[0].str);
        }

        // 字幕：pointsOfInterest {秒字符串: strings键} → [秒, zh, en] 按时间升序
        const json::Value* poi = a.Find(L"pointsOfInterest");
        if (poi && poi->IsObject()) {
            for (const auto& kv : poi->obj) {
                if (!kv.second.IsString()) continue;
                wchar_t* end = nullptr;
                double t = wcstod(kv.first.c_str(), &end);
                if (end == kv.first.c_str()) continue;  // 键须为秒数
                CaptionCue cue;
                cue.timeSec = t;
                auto izh = zh.find(kv.second.str);
                if (izh != zh.end()) cue.zh = izh->second;
                auto ien = en.find(kv.second.str);
                if (ien != en.end()) cue.en = ien->second;
                if (cue.zh.empty() && cue.en.empty()) continue;  // 双空跳过
                c.captions.push_back(std::move(cue));
            }
            std::stable_sort(c.captions.begin(), c.captions.end(),
                             [](const CaptionCue& x, const CaptionCue& y) {
                                 return x.timeSec < y.timeSec;
                             });
        }
        out.push_back(std::move(c));
    }
    return !out.empty();
}

// ------------------------------------------------------------
// merge_catalog：按 id 合并（底本条目原样保序保留，新 id 追加尾部）
// ------------------------------------------------------------
std::string merge_catalog(const std::vector<std::string>& baseJsons,
                          const std::vector<AppleClip>& fresh,
                          const std::wstring& sourceUrl,
                          size_t& total, size_t& added) {
    std::string out = "{\"source\":";
    DumpString(sourceUrl, out);
    out += ",\"videos\":[";
    total = 0;
    std::set<std::wstring> seen;

    // 底本：逐个解析，videos 数组原样回写（键序/内容不动），id 去重首现胜出
    for (const std::string& base : baseJsons) {
        json::Value root;
        if (!json::Parse(base, root)) continue;
        const json::Value* videos = root.Find(L"videos");
        if (!videos || !videos->IsArray()) continue;
        for (const json::Value& v : videos->arr) {
            const std::wstring id = v.GetStr(L"id");
            if (id.empty() || !seen.insert(id).second) continue;
            if (total) out.push_back(',');
            DumpJsonValue(v, out);  // Value 按自身键序直出，旧条目原样保留
            ++total;
        }
    }

    // 新条目：未见过的 id 追加
    added = 0;
    for (const AppleClip& c : fresh) {
        if (c.id.empty() || !seen.insert(c.id).second) continue;
        if (total) out.push_back(',');
        ClipToValue(c, out);
        ++total;
        ++added;
    }
    out += "]}";
    return out;
}

// ------------------------------------------------------------
// sync_catalog：联网编排
// ------------------------------------------------------------
namespace {

bool ReadWholeFile(const std::filesystem::path& p, std::string& out);

// 证书链回退：Windows 信任库默认不含 Apple 根证书，WinInet 会报
// ERROR_INTERNET_INVALID_CA。此时改用系统自带 curl.exe，以仓库内置的
// Apple 根证书（certs/AppleRootCA.pem）做定点校验——不动系统信任库，
// 也绝不退化为跳过校验。urlBuf 为空 = 不读 body（探测用）。
DWORD CurlFetch(const std::wstring& url, std::string* out, size_t maxBytes) {
    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    const std::wstring curlExe = std::wstring(sysDir) + L"\\curl.exe";
    if (GetFileAttributesW(curlExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }

    wchar_t exe[32768] = {};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    const std::wstring caPath =
        (std::filesystem::path(exe).parent_path() / L"certs" / L"AppleRootCA.pem")
            .wstring();
    if (GetFileAttributesW(caPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }

    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    wchar_t bodyPath[MAX_PATH] = {}, statusPath[MAX_PATH] = {};
    if (!GetTempFileNameW(tmpDir, L"asb", 0, bodyPath)) return 0;
    if (!GetTempFileNameW(tmpDir, L"ass", 0, statusPath)) {
        DeleteFileW(bodyPath);
        return 0;
    }

    // body 走 -o 文件；-w 状态码走 stdout → 重定向到 statusPath
    const std::wstring bodyArg = out ? bodyPath : L"NUL";
    std::wstring args = L"\"" + curlExe + L"\" -sS --connect-timeout 15 --max-time 300"
                        L" --max-filesize 67108864 --cacert \"" + caPath +
                        L"\" -o \"" + bodyArg + L"\" -w \"%{http_code}\" \"" + url + L"\"";

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE hStatus = CreateFileW(statusPath, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hStatus == INVALID_HANDLE_VALUE) {
        DeleteFileW(bodyPath);
        DeleteFileW(statusPath);
        return 0;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nullptr;
    si.hStdOutput = hStatus;
    si.hStdError = hStatus;
    PROCESS_INFORMATION pi = {};
    DWORD status = 0;
    if (CreateProcessW(curlExe.c_str(), args.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 360 * 1000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(hStatus);  // 先关写句柄再读，避免共享冲突
        hStatus = INVALID_HANDLE_VALUE;

        std::string codeStr;
        if (ReadWholeFile(statusPath, codeStr)) {
            while (!codeStr.empty() && (codeStr.back() < '0' || codeStr.back() > '9'))
                codeStr.pop_back();
            status = static_cast<DWORD>(_wtoi(std::wstring(codeStr.begin(),
                                                           codeStr.end()).c_str()));
        }
        if (out && (status == 200 || status == 206)) {
            std::string body;
            if (ReadWholeFile(bodyPath, body) && !body.empty() &&
                body.size() <= maxBytes) {
                *out = std::move(body);
            } else {
                out->clear();
                status = 0;  // 超限/读取失败视为异常响应
            }
        }
    }
    if (hStatus != INVALID_HANDLE_VALUE) CloseHandle(hStatus);
    DeleteFileW(bodyPath);
    DeleteFileW(statusPath);
    return status;
}

// WinInet 下载（同 ClipCache 先例）。urlBuf 为空 = 不读 body（探测用）。
// 返回 HTTP 状态码；0 = 连接失败
DWORD HttpFetch(const std::wstring& url, std::string* out, size_t maxBytes) {
    HINTERNET inet = InternetOpenW(L"NewWallpaper.Apple", INTERNET_OPEN_TYPE_PRECONFIG,
                                   nullptr, nullptr, 0);
    if (!inet) return 0;
    HINTERNET req = InternetOpenUrlW(inet, url.c_str(), nullptr, 0,
                                     INTERNET_FLAG_NO_CACHE_WRITE |
                                     INTERNET_FLAG_NO_UI, 0);
    if (!req) {
        const DWORD err = GetLastError();
        InternetCloseHandle(inet);
        // 证书链类错误才回退；其余（断网等）维持原状
        if (err == ERROR_INTERNET_INVALID_CA ||
            err == ERROR_INTERNET_SEC_CERT_CN_INVALID ||
            err == ERROR_INTERNET_SEC_CERT_DATE_INVALID ||
            err == ERROR_INTERNET_SEC_CERT_ERRORS ||
            err == ERROR_INTERNET_SECURITY_CHANNEL_ERROR) {
            return CurlFetch(url, out, maxBytes);
        }
        return 0;
    }
    DWORD status = 0, statusLen = sizeof(status);
    HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &status, &statusLen, nullptr);
    if (status == 0) {  // 句柄创建成功但握手/读取阶段才报证书错的形态
        InternetCloseHandle(req);
        InternetCloseHandle(inet);
        return CurlFetch(url, out, maxBytes);
    }
    if (out && (status == 200 || status == 206)) {
        out->clear();
        char buf[64 * 1024];
        DWORD got = 0;
        while (out->size() < maxBytes &&
               InternetReadFile(req, buf, sizeof(buf), &got) && got > 0) {
            if (out->size() + got > maxBytes) {  // 超限视为异常响应
                out->clear();
                status = 0;
                break;
            }
            out->append(buf, got);
        }
    }
    InternetCloseHandle(req);
    InternetCloseHandle(inet);
    return status;
}

bool ReadWholeFile(const std::filesystem::path& p, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.wstring().c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > static_cast<long>(kMaxTarBytes)) {
        fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(n));
    bool ok = n == 0 || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// .tmp + 原子替换写入，失败清理残留
bool WriteAtomic(const std::filesystem::path& outPath, const std::string& data) {
    const std::wstring tmp = outPath.wstring() + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (ok) {
        ok = MoveFileExW(tmp.c_str(), outPath.wstring().c_str(),
                         MOVEFILE_REPLACE_EXISTING) != 0;
    }
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// 合并底本候选（按清单加载优先级）：用户目录 → 程序目录 → assets
std::vector<std::filesystem::path> DefaultBasePaths(
    const std::filesystem::path& output) {
    wchar_t exe[32768] = {};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::filesystem::path exeDir =
        exe[0] ? std::filesystem::path(exe).parent_path() : std::filesystem::path();
    return {output, exeDir / L"apples.json", exeDir / L"assets" / L"apples.json"};
}

}  // namespace

SyncResult sync_catalog(const std::vector<std::filesystem::path>& basePaths,
                        const std::filesystem::path& output) {
    SyncResult result;

    // 1) 版本探测：自 17 向上取最大 200（允许跳号）；404 = 不存在，
    //    其它失败 = 网络故障 → 静默回退。探测不读 body。
    int version = 0;
    for (int n = kFallbackResourceVersion + 1; n <= kFallbackResourceVersion + 10; ++n) {
        wchar_t probe[128];
        swprintf_s(probe, L"%sresources-%d.tar", kCatalogBase, n);
        DWORD status = HttpFetch(probe, nullptr, 0);
        if (status == 200) {
            version = n;
        } else if (status == 0 || (status != 404 && status != 200)) {
            if (version == 0) return result;  // 网络故障：不改动任何文件
            break;
        }
    }
    const int useVersion = version > 0 ? version : kFallbackResourceVersion;

    // 2) 下载资源包（纯清单 tar，实测 ~3MB，上限 64MB）
    wchar_t url[128];
    swprintf_s(url, L"%sresources-%d.tar", kCatalogBase, useVersion);
    std::string tarBytes;
    if (HttpFetch(url, &tarBytes, kMaxTarBytes) != 200 || tarBytes.empty()) {
        return result;  // 网络失败静默回退现有清单
    }
    result.source = url;

    // 3) 解出 entries.json + 中英文案（本地化缺失不致命，缺文案条目照常同步）
    const std::vector<char> tarBlob(tarBytes.begin(), tarBytes.end());
    std::string entries;
    if (!tar_extract(tarBlob, "entries.json", entries)) return result;
    std::string zhBytes, enBytes;
    std::map<std::wstring, std::wstring> zh, en;
    tar_extract(tarBlob,
                "TVIdleScreenStrings.bundle/zh_CN.lproj/Localizable.nocache.strings",
                zhBytes);
    tar_extract(tarBlob,
                "TVIdleScreenStrings.bundle/en.lproj/Localizable.nocache.strings",
                enBytes);
    if (!zhBytes.empty()) parse_strings(zhBytes, zh);
    if (!enBytes.empty()) parse_strings(enBytes, en);

    // 4) 映射 + 合并（底本取全部候选 union，按 id 去重首现胜出）
    std::vector<AppleClip> fresh;
    if (!clips_from_entries(entries, zh, en, fresh)) return result;

    const std::vector<std::filesystem::path> bases =
        basePaths.empty() ? DefaultBasePaths(output) : basePaths;
    std::vector<std::string> baseJsons;
    for (const auto& p : bases) {
        std::string s;
        if (ReadWholeFile(p, s)) baseJsons.push_back(std::move(s));
    }

    size_t total = 0, added = 0;
    std::string merged = merge_catalog(baseJsons, fresh, result.source, total, added);
    result.total = total;
    result.added = added;

    // 5) 无新条目不写文件；有则原子替换，引擎经 LoadCatalog 重载生效
    if (added == 0) {
        result.outcome = SyncOutcome::unchanged;
        return result;
    }
    if (!WriteAtomic(output, merged)) return result;  // 写失败 = failed
    result.outcome = SyncOutcome::updated;
    return result;
}

}  // namespace apple_sync
