// Engine/Graphics/WFC/WFCMinEntropyObserver.h
//
// Min-entropy cell selector. Migrated from WFCObserver.cpp when WFCObserver
// became an abstract base. See WFCObserver.h for the strategy interface.
#pragma once

#include "WFCObserver.h"
#include "WFCTypes.h"

#include <vector>
#include <utility>

namespace primal::graphics::wfc {

class WaveGrid;

class WFCMinEntropyObserver : public WFCObserver {
public:
    void Initialize(const WaveGrid& grid) override;
    void OnCellChanged(WFCGridCoord c) override;
    WFCGridCoord PickNextCollapse(const WaveGrid& grid) override;
    bool Empty() const override { return heap_.empty(); }

private:
    std::vector<std::pair<u8, WFCGridCoord>> heap_;
};

} // namespace primal::graphics::wfc
