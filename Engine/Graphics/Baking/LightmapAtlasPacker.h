#pragma once

// 场景级 lightmap 矩形装箱（2026-10-19 lightmap 计划，M1 契约冻结）。
//
// MaxRects（Best-Short-Side-Fit + 空闲矩形分裂），单页方形 bin。确定性：
// 输入按（面积降序, id 升序）稳定排序后装箱，同输入必同输出。纯 std 实现
// ——单测与 CLI 共用。纹素需求（面积×密度²×利用率系数）由调用方折算成
// 矩形尺寸后进入本装箱器；装不下的 mesh 标记 placed=false（v1 不分页，
// 由上层报错或放大 atlas 重试）。

#include <cstdint>
#include <vector>

namespace primal::graphics::baking {

struct PackRequest {
    std::uint32_t id = 0;
    std::uint32_t w = 0;  // >0
    std::uint32_t h = 0;  // >0
};

struct PackResult {
    std::uint32_t id = 0;
    bool placed = false;
    std::uint32_t x = 0, y = 0, w = 0, h = 0;
};

class MaxRectsPacker {
public:
    explicit MaxRectsPacker(std::uint32_t bin_size);

    // 装箱全部请求；返回结果顺序与输入无关（按 id 对应）。
    // margin：矩形间留白（texel，双侧各 margin/2 由调用方折算——本层直接
    // 给 w/h 加 margin 后装箱，结果矩形为扩张后尺寸）。
    std::vector<PackResult> Pack(const std::vector<PackRequest>& requests,
                                 std::uint32_t margin = 0);

    // 诊断：已放置面积占比。
    float Utilization() const { return utilization_; }

private:
    bool PlaceOne(std::uint32_t id, std::uint32_t w, std::uint32_t h, PackResult& out);
    void SplitFreeRects(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h);
    void PruneFreeRects();

    std::uint32_t bin_;
    std::uint64_t used_area_ = 0;
    float utilization_ = 0.0f;
    struct FreeRect {
        std::uint32_t x, y, w, h;
    };
    std::vector<FreeRect> free_;
};

} // namespace primal::graphics::baking
