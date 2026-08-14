// Engine/Graphics/WFC/WFCObserver.h
//
// Strategy interface for picking the next cell to collapse. Concrete
// implementations: WFCMinEntropyObserver (default), WFCDistanceObserver.
// Applications can also subclass this directly to inject custom strategies
// into WFCSolver via SetObserver.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

class WaveGrid;

class WFCObserver {
public:
    virtual ~WFCObserver() = default;

    // Populate internal state from all uncollapsed cells. Called once per
    // solve at Initialize time, and again on each restart.
    virtual void Initialize(const WaveGrid& grid) = 0;

    // Notify that cell c's candidate set changed. Stateful strategies
    // (e.g. min-entropy heap) update here; stateless strategies no-op.
    virtual void OnCellChanged(WFCGridCoord c) = 0;

    // Returns the next cell to collapse, or {-1,-1,-1} when no uncollapsed
    // cell remains (or all remaining are contradictions with entropy 0).
    virtual WFCGridCoord PickNextCollapse(const WaveGrid& grid) = 0;

    // True when the observer has no candidate to propose. Used by the solver
    // to short-circuit "done before next Pick" on the same Step that collapses
    // the final cell.
    virtual bool Empty() const = 0;
};

} // namespace primal::graphics::wfc
