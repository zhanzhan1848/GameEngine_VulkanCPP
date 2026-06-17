#pragma once

#include "CommonHeaders.h"
#include "Graphics/MaterialGraph/MaterialGraph.h"
#include <functional>
#include <mutex>
#include <string>

namespace primal::graphics::shader_ir {

struct ShaderCompileResult {
    std::string source;
    std::string error;
    bool success{false};
};

class ShaderCompiler {
public:
    using Callback = std::function<void(u32 request_id, ShaderCompileResult result)>;

    static ShaderCompileResult CompileSync(const material_graph::MaterialGraph& graph);

    static u32 CompileAsync(const material_graph::MaterialGraph& graph, Callback callback);

    static void Tick();

private:
    struct PendingResult {
        u32 request_id;
        ShaderCompileResult result;
        Callback callback;
    };

    static u32 next_id_;
    static std::mutex mutex_;
    static utl::vector<PendingResult> completed_;
};

} // namespace primal::graphics::shader_ir
