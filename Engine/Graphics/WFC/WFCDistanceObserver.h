// Engine/Graphics/WFC/WFCDistanceObserver.h
//
// Radial-expansion observer. PickNextCollapse returns the uncollapsed cell
// closest (Euclidean) to a configured origin. Ties broken by row-major
// scan order. OnCellChanged is a no-op (distance doesn't change with
// candidate set). Complexity: O(N) per Pick.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"
#include "WFCObserver.h"

namespace primal::graphics::wfc {

class WFCDistanceObserver : public WFCObserver {
public:
    explicit WFCDistanceObserver(WFCGridCoord origin)
        : origin_(origin) {}

    void Initialize(const WaveGrid& grid) override;
    void OnCellChanged(WFCGridCoord c) override;  // no-op
    WFCGridCoord PickNextCollapse(const WaveGrid& grid) override;
    bool Empty() const override { return empty_; }

private:
    WFCGridCoord origin_;
    bool empty_{false};
};

} // namespace primal::graphics::wfc
