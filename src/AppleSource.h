// ============================================================
// AppleSource —— apples.json 片源清单管理
//
// 清单结构（来自 apple-tv）：
//   { "source": "...", "videos": [ {
//       "id": "...", "label": "Seals", "category": "underwater",
//       "url":  "...2K_AVC.mov",     // 1080p H.264 保底档
//       "hevc": "...2K_HEVC.mov",    // 1080p HEVC
//       "uhd":  "...4K_HEVC.mov",    // 4K HEVC SDR
//       "dv":   "...HDR_4K_HEVC.mov",// 4K HEVC HDR（Dolby Vision）
//       "captions": [[0, "中文地名", "English"]] } ] }
// ============================================================

#pragma once

#include <string>
#include <vector>

// 字幕时间点：captions 数组元素 [秒, 中文, 英文]
struct CaptionCue {
    double timeSec = 0.0;
    std::wstring zh;
    std::wstring en;
};

// 一个航拍片源条目
struct AppleClip {
    std::wstring id;
    std::wstring label;
    std::wstring category;   // underwater / space / landscapes / cities
    std::wstring url;        // 各画质档 URL（ASCII，直接可转 BSTR）
    std::wstring hevc;
    std::wstring uhd;
    std::wstring dv;
    std::vector<CaptionCue> captions;
};

class AppleCatalog {
public:
    // 解析 apples.json（UTF-8 文件）；失败返回 false 且不改动已有内容
    bool LoadFromFile(const std::wstring& path);

    // 内置兜底清单（apples.json 缺失/损坏时，少量片源保证可用）
    void LoadBuiltIn();

    const std::vector<AppleClip>& clips() const { return clips_; }

    // 清单中出现过的全部分类（去重、保序）
    std::vector<std::wstring> Categories() const;

    // 按分类筛选；categories 为空 = 全部
    std::vector<const AppleClip*> Filter(const std::vector<std::wstring>& categories) const;

private:
    std::vector<AppleClip> clips_;
};
