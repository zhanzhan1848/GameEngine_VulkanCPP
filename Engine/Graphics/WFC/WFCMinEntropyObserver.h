// Engine/Graphics/WFC/WFCMinEntropyObserver.h
//
// Min-entropy cell selector. Migrated from WFCObserver.cpp when WFCObserver
// became an abstract base. See WFCObserver.h for the strategy interface.
#pragma once

#include "WFCTypes.h"

#include <vector>
#include <utility>

namespace primal::graphics::wfc {

class WaveGrid;

class WFCMinEntropyObserver {
public:
    void Initialize(const class WaveGrid& grid);
    void OnCellChanged(WFCGridCoord c);
    WFCGridCoord PickNextCollapse(const class WaveGrid& grid);
    bool Empty() const { return heap_.empty(); }

private:
    std::vector<std::pair<u8, WFCGridCoord>> heap_;
};

} // namespace primal::graphics::wfc
