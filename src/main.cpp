// ============================================================
// apple-aerials CLI —— Apple 航拍片源清单同步工具
//
// 用法：
//   aerialsync [输出路径]        （默认写 ./apples.json）
//
// 拉取 Apple 官方 resources-N.tar，解析 entries.json + 中英文案，
// 映射为 apples.json 片源格式，按 id 合并进现有清单后原子写回。
// ============================================================

#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <filesystem>

#include "AppleSync.h"

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);  // 控制台按 UTF-8 输出中文

    std::filesystem::path output = L"apples.json";
    if (argc > 1) output = argv[1];

    wprintf(L"正在同步 Apple 航拍片源清单…\n");
    const apple_sync::SyncResult r = apple_sync::sync_catalog({}, output);

    switch (r.outcome) {
    case apple_sync::SyncOutcome::updated:
        wprintf(L"完成：清单已更新 → %s\n", output.c_str());
        wprintf(L"  资源包：%s\n", r.source.c_str());
        wprintf(L"  片源总数：%zu（本次新增 %zu 条）\n", r.total, r.added);
        return 0;
    case apple_sync::SyncOutcome::unchanged:
        wprintf(L"无新片源，清单未变化（当前共 %zu 条）。\n", r.total);
        return 0;
    default:
        wprintf(L"同步失败：网络或解析出错，未改动任何文件。\n");
        return 1;
    }
}
