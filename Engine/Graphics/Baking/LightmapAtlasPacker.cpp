// MaxRects 装箱（BSSF）。经典 Jukka Jylänki 算法的直接实现，确定性排序
// 见头注释。

#include "LightmapAtlasPacker.h"

#include <algorithm>

namespace primal::graphics::baking {

MaxRectsPacker::MaxRectsPacker(std::uint32_t bin_size) : bin_(bin_size) {
    free_.push_back({0, 0, bin_, bin_});
}

std::vector<PackResult> MaxRectsPacker::Pack(const std::vector<PackRequest>& requests,
                                             std::uint32_t margin) {
    std::vector<PackRequest> sorted = requests;
    std::stable_sort(sorted.begin(), sorted.end(), [](const PackRequest& a, const PackRequest& b) {
        const std::uint64_t areaA = (std::uint64_t)a.w * a.h;
        const std::uint64_t areaB = (std::uint64_t)b.w * b.h;
        if (areaA != areaB) return areaA > areaB;  // 面积降序
        return a.id < b.id;                        // 平手按 id——确定性
    });

    std::vector<PackResult> results;
    results.reserve(requests.size());
    for (const auto& r : sorted) {
        PackResult out;
        out.id = r.id;
        if (r.w == 0 || r.h == 0) {
            results.push_back(out);
            continue;
        }
        const std::uint64_t gw = (std::uint64_t)r.w + margin;
        const std::uint64_t gh = (std::uint64_t)r.h + margin;
        if (gw > bin_ || gh > bin_) {
            results.push_back(out);
            continue;
        }
        PlaceOne(r.id, static_cast<std::uint32_t>(gw), static_cast<std::uint32_t>(gh), out);
        results.push_back(out);
    }
    utilization_ = (std::uint64_t)bin_ * bin_
                       ? static_cast<float>((double)used_area_ / ((double)bin_ * bin_))
                       : 0.0f;
    return results;
}

bool MaxRectsPacker::PlaceOne(std::uint32_t id, std::uint32_t w, std::uint32_t h,
                              PackResult& out) {
    // Best-Short-Side-Fit：遍历所有能容纳的空闲矩形，取短边剩余最小者。
    int best = -1;
    std::uint32_t bestShort = 0xFFFFFFFFu, bestLong = 0xFFFFFFFFu;
    for (size_t i = 0; i < free_.size(); ++i) {
        const FreeRect& fr = free_[i];
        if (fr.w < w || fr.h < h) continue;
        const std::uint32_t leftoverHoriz = std::abs(static_cast<int>(fr.w) - static_cast<int>(w));
        const std::uint32_t leftoverVert = std::abs(static_cast<int>(fr.h) - static_cast<int>(h));
        const std::uint32_t shortSide = std::min(leftoverHoriz, leftoverVert);
        const std::uint32_t longSide = std::max(leftoverHoriz, leftoverVert);
        if (shortSide < bestShort || (shortSide == bestShort && longSide < bestLong)) {
            best = static_cast<int>(i);
            bestShort = shortSide;
            bestLong = longSide;
        }
    }
    if (best < 0) return false;

    const FreeRect target = free_[static_cast<size_t>(best)];
    out.placed = true;
    out.x = target.x;
    out.y = target.y;
    out.w = w;
    out.h = h;
    used_area_ += (std::uint64_t)w * h;
    SplitFreeRects(target.x, target.y, w, h);
    PruneFreeRects();
    return true;
}

void MaxRectsPacker::SplitFreeRects(std::uint32_t x, std::uint32_t y,
                                    std::uint32_t w, std::uint32_t h) {
    // 对每个与放置矩形相交的空闲矩形做至多 4 向分裂。
    std::vector<FreeRect> add;
    for (size_t i = 0; i < free_.size();) {
        FreeRect& fr = free_[i];
        const bool overlapX = x < fr.x + fr.w && x + w > fr.x;
        const bool overlapY = y < fr.y + fr.h && y + h > fr.y;
        if (!(overlapX && overlapY)) { ++i; continue; }

        if (x < fr.x + fr.w && x + w > fr.x) {
            if (y > fr.y) add.push_back({fr.x, fr.y, fr.w, y - fr.y});
            if (y + h < fr.y + fr.h)
                add.push_back({fr.x, y + h, fr.w, fr.y + fr.h - (y + h)});
        }
        if (y < fr.y + fr.h && y + h > fr.y) {
            if (x > fr.x) add.push_back({fr.x, fr.y, x - fr.x, fr.h});
            if (x + w < fr.x + fr.w)
                add.push_back({x + w, fr.y, fr.x + fr.w - (x + w), fr.h});
        }
        // 该空闲矩形被完全消耗/分裂——移除。
        fr = free_.back();
        free_.pop_back();
    }
    free_.insert(free_.end(), add.begin(), add.end());
}

void MaxRectsPacker::PruneFreeRects() {
    // 去包含：被其他空闲矩形完全包含的是冗余。
    for (size_t i = 0; i < free_.size(); ++i) {
        for (size_t j = i + 1; j < free_.size();) {
            const FreeRect& a = free_[i];
            const FreeRect& b = free_[j];
            const bool bInA = a.x <= b.x && a.y <= b.y &&
                              a.x + a.w >= b.x + b.w && a.y + a.h >= b.y + b.h;
            const bool aInB = b.x <= a.x && b.y <= a.y &&
                              b.x + b.w >= a.x + a.w && b.y + b.h >= a.y + a.h;
            if (bInA) {
                free_[j] = free_.back();
                free_.pop_back();
            } else if (aInB) {
                free_[i] = free_.back();
                free_.pop_back();
                j = i + 1;  // a 被移除，内层重新对齐
            } else {
                ++j;
            }
        }
    }
}

} // namespace primal::graphics::baking
