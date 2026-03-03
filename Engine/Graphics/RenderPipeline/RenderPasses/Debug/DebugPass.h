#pragma once
#include "Graphics/RenderGraph/RenderGraph.h"
#include <vector>
#include <string>

namespace primal::graphics {

struct DebugResource {
    std::string name;
    rendergraph::RGResourceHandle handle;
};

struct DebugPassData {
    rendergraph::RGResourceHandle target;
    utl::vector<DebugResource> debugResources;
};

const DebugPassData& AddDebugPass(rendergraph::RenderGraph& graph, rendergraph::RGResourceHandle target, const utl::vector<DebugResource>& debugResources = {});

// Manually shutdown the debug pass renderer (to handle static lifetime issues)
void ShutdownDebugPass();

}
