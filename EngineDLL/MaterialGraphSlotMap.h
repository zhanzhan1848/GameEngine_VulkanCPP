#pragma once

// Shared internal header for cross-module MaterialGraph lookup.
// The actual slot map implementation lives in MaterialGraphAPI.cpp (Agent 1's file).
// This file is included by both MaterialGraphAPI.cpp and MaterialGraphReflectionAPI.cpp
// so they share the same slot map accessor without duplicating state.

#include "CommonHeaders.h"

namespace primal::graphics::material_graph {
    class MaterialGraph;
}

// Returns the MaterialGraph* for the given 1-based graph_id, or nullptr if invalid.
// Agent 1 (MaterialGraphAPI.cpp) MUST implement this function and expose it here.
// Until that integration happens, a stub is provided in MaterialGraphReflectionAPI.cpp.
primal::graphics::material_graph::MaterialGraph* GetMaterialGraphById(u32 graph_id);
