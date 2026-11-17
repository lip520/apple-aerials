// ============================================================
// AppleSync —— Apple 航拍片源清单同步器
//
// 拉取 Apple 资源包 resources-N.tar（纯清单 tar，不含视频），解出
// entries.json（结构化片源清单）与 TVIdleScreenStrings.bundle 中英文案，
// 映射为 apples.json 片源格式，按 id 合并进现有清单：
//   旧条目原样保留、新条目追加；合并结果写入用户数据目录 apples.json
//   （LoadCatalog 用户目录优先），网络失败静默回退现有清单。
// ============================================================

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "AppleSource.h"

namespace apple_sync {

// 同步结果
enum class SyncOutcome {
    updated,    // 新增片源，清单已合并写入（引擎重载后生效）
    unchanged,  // 无新片源（或仅资源包版本推进），播放列表未变
    failed,     // 网络/解析失败，未改动任何文件
};

struct SyncResult {
    SyncOutcome outcome = SyncOutcome::failed;
    size_t total = 0;     // 合并后清单片源总数
    size_t added = 0;     // 本次新增条数
    std::wstring source;  // 生效的资源包 URL
};

// —— 纯函数（离线可测） ——

// 从内存 tar（512B 头 + 八进制长度，块对齐）中按路径取条目内容；
// name 与条目全等或为其 /后缀时命中
bool tar_extract(const std::vector<char>& tar, const std::string& name,
                 std::string& out);

// 便捷重载：接受 std::string 内容的 tar（测试/文本场景）
inline bool tar_extract(const std::string& tar, const std::string& name,
                        std::string& out) {
    return tar_extract(std::vector<char>(tar.begin(), tar.end()), name, out);
}

// 解析 .strings 二进制 plist（bplist00 的 dict<string,string> 子集）
bool parse_strings(const std::string& bytes,
                   std::map<std::wstring, std::wstring>& out);

// entries.json + 中英文案表 → apples.json 片源列表
// （id/label/category/URL 档位映射 + pointsOfInterest→captions[秒,中,英]）
bool clips_from_entries(const std::string& entriesJson,
                        const std::map<std::wstring, std::wstring>& zh,
                        const std::map<std::wstring, std::wstring>& en,
                        std::vector<AppleClip>& out);

// 内置 extras：macOS 专有片源（Tahoe/Goa/喜马拉雅/喀拉拉茶田 共 15 条）。
// 这些片源走 WallpaperFramework 自有分发，官方 resources-N.tar 清单永远
// 不会包含它们；同步时按 id 去重自动补齐，重新生成清单不丢失。
std::vector<AppleClip> builtin_extras();

// 按 id 合并：baseJsons 按清单加载优先级排列（旧条目原样保留，首个
// 出现者胜出），fresh 中的新 id 追加到尾部；返回合并后清单 JSON 文本
// （UTF-8），total = 合并后总数，added = 新增条数
std::string merge_catalog(const std::vector<std::string>& baseJsons,
                          const std::vector<AppleClip>& fresh,
                          const std::wstring& sourceUrl,
                          size_t& total, size_t& added);

// —— 编排（联网） ——
// basePaths：现有清单候选（按加载优先级排列，均作为合并底本），
// output：合并结果写入路径（.tmp + 原子替换）。
// 向上探测 resources-N.tar 新版本；无新版本或网络失败不写文件。
SyncResult sync_catalog(const std::vector<std::filesystem::path>& basePaths,
                        const std::filesystem::path& output);

}  // namespace apple_sync
