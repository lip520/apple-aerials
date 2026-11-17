// ============================================================
// AppleSource 实现 —— apples.json 解析 + 分类筛选 + 内置兜底
// ============================================================

#include "AppleSource.h"

#include <cwchar>
#include <fstream>
#include <iterator>

#include "MiniJson.h"

// ------------------------------------------------------------
// 从 JSON 值构造一条片源
// ------------------------------------------------------------
static bool ClipFromJson(const json::Value& v, AppleClip& out) {
    if (!v.IsObject()) return false;

    out.id = v.GetStr(L"id");
    out.label = v.GetStr(L"label");
    out.category = v.GetStr(L"category");
    out.url = v.GetStr(L"url");
    out.hevc = v.GetStr(L"hevc");
    out.uhd = v.GetStr(L"uhd");
    out.dv = v.GetStr(L"dv");

    // id + url 是可播的最低要求
    if (out.id.empty() || out.url.empty()) return false;

    out.captions.clear();
    if (const json::Value* caps = v.Find(L"captions"); caps && caps->IsArray()) {
        for (const json::Value& cue : caps->arr) {
            if (!cue.IsArray() || cue.arr.size() < 2) continue;
            CaptionCue c;
            if (cue.arr[0].type == json::Value::Type::Number) c.timeSec = cue.arr[0].num;
            if (cue.arr[1].IsString()) c.zh = cue.arr[1].str;
            if (cue.arr.size() >= 3 && cue.arr[2].IsString()) c.en = cue.arr[2].str;
            out.captions.push_back(std::move(c));
        }
    }
    return true;
}

// ------------------------------------------------------------
// 加载 apples.json
// ------------------------------------------------------------
bool AppleCatalog::LoadFromFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const std::string utf8((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());

    json::Value root;
    if (!json::Parse(utf8, root) || !root.IsObject()) return false;

    const json::Value* videos = root.Find(L"videos");
    if (!videos || !videos->IsArray()) return false;

    std::vector<AppleClip> parsed;
    parsed.reserve(videos->arr.size());
    for (const json::Value& v : videos->arr) {
        AppleClip clip;
        if (ClipFromJson(v, clip)) parsed.push_back(std::move(clip));
    }
    if (parsed.empty()) return false;

    clips_ = std::move(parsed);
    return true;
}

// ------------------------------------------------------------
// 内置兜底清单：取自 apples.json 的代表性片源
// （apples.json 缺失时保证程序仍可播放）
// ------------------------------------------------------------
void AppleCatalog::LoadBuiltIn() {
    clips_.clear();

    AppleClip a;
    a.id = L"83C65C90-270C-4490-9C69-F51FE03D7F06";
    a.label = L"Seals";
    a.category = L"underwater";
    a.url = L"https://sylvan.apple.com/Videos/SE_A016_C009_SDR_20190717_SDR_2K_AVC.mov";
    a.hevc = L"https://sylvan.apple.com/Aerials/2x/Videos/SE_A016_C009_SDR_20190717_SDR_2K_HEVC.mov";
    a.uhd = L"https://sylvan.apple.com/Aerials/2x/Videos/SE_A016_C009_SDR_20190717_SDR_4K_HEVC.mov";
    a.dv = L"https://sylvan.apple.com/Aerials/2x/Videos/SE_A016_C009_HDR_20190717_HDR_4K_HEVC.mov";
    a.captions.push_back({0.0, L"南非开普半岛海岸的非洲毛皮海狮",
                          L"Cape Fur Seals off the coast of Cape Peninsula, South Africa"});
    clips_.push_back(std::move(a));

    AppleClip b;
    b.id = L"7719B48A-2005-4011-9280-2F64EEC6FD91";
    b.label = L"Southern California to Baja";
    b.category = L"space";
    b.url = L"https://sylvan.apple.com/Videos/comp_A114_C001_0305OT_v10_SDR_FINAL_22062018_SDR_2K_AVC.mov";
    b.hevc = L"https://sylvan.apple.com/Aerials/2x/Videos/comp_A114_C001_0305OT_v10_SDR_FINAL_22062018_SDR_2K_HEVC.mov";
    b.uhd = L"https://sylvan.apple.com/Aerials/2x/Videos/comp_A114_C001_0305OT_v10_SDR_FINAL_22062018_SDR_4K_HEVC.mov";
    b.dv = L"https://sylvan.apple.com/Aerials/2x/Videos/comp_A114_C001_0305OT_v10_HDR_FINAL_22062018_Bert_HDR_4K_HEVC.mov";
    clips_.push_back(std::move(b));

    AppleClip c;
    c.id = L"2F52E34C-39D4-4AB1-9025-8F7141FAA720";
    c.label = L"Greenland";
    c.category = L"landscapes";
    c.url = L"https://sylvan.apple.com/Videos/comp_GL_G002_C002_PSNK_v03_SDR_PS_20180925_SDR_2K_AVC.mov";
    c.hevc = L"https://sylvan.apple.com/Aerials/2x/Videos/comp_GL_G002_C002_PSNK_v03_SDR_PS_20180925_SDR_2K_HEVC.mov";
    c.uhd = L"https://sylvan.apple.com/Aerials/2x/Videos/comp_GL_G002_C002_PSNK_v03_SDR_PS_20180925_SDR_4K_HEVC.mov";
    clips_.push_back(std::move(c));
}

std::vector<std::wstring> AppleCatalog::Categories() const {
    std::vector<std::wstring> out;
    for (const AppleClip& clip : clips_) {
        bool seen = false;
        for (const std::wstring& c : out) {
            if (c == clip.category) {
                seen = true;
                break;
            }
        }
        if (!seen) out.push_back(clip.category);
    }
    return out;
}

std::vector<const AppleClip*> AppleCatalog::Filter(
    const std::vector<std::wstring>& categories) const {
    std::vector<const AppleClip*> out;
    for (const AppleClip& clip : clips_) {
        if (categories.empty()) {
            out.push_back(&clip);
            continue;
        }
        for (const std::wstring& want : categories) {
            if (_wcsicmp(want.c_str(), clip.category.c_str()) == 0) {
                out.push_back(&clip);
                break;
            }
        }
    }
    return out;
}
