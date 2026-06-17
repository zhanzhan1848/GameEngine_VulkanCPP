#include "Graphics/ShaderIR/ShaderCompiler.h"
#include "Graphics/ShaderIR/MaterialGraphToIR.h"
#include "Graphics/ShaderIR/MetalEmitter.h"
#include "JobSystem/JobSystem.h"

namespace primal::graphics::shader_ir {

u32 ShaderCompiler::next_id_{0};
std::mutex ShaderCompiler::mutex_{};
utl::vector<ShaderCompiler::PendingResult> ShaderCompiler::completed_;

ShaderCompileResult ShaderCompiler::CompileSync(const material_graph::MaterialGraph& graph) {
    ShaderCompileResult result;

    MaterialGraphToIR translator;
    auto ir_result = translator.Translate(graph);
    if (!ir_result.success) {
        for (auto& err : ir_result.errors) {
            result.error += err.message + "\n";
        }
        result.success = false;
        return result;
    }

    MetalEmitter emitter;
    auto emit_result = emitter.Emit(ir_result.function);
    if (!emit_result.success) {
        result.error = emit_result.log;
        result.success = false;
        return result;
    }

    result.source = std::move(emit_result.source);
    result.success = true;
    return result;
}

u32 ShaderCompiler::CompileAsync(const material_graph::MaterialGraph& graph, Callback callback) {
    u32 id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_id_++;
    }

    jobsystem::JobSystem::ScheduleBackground([&graph, id, cb = std::move(callback)]() {
        auto result = CompileSync(graph);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed_.push_back({id, std::move(result), std::move(cb)});
        }
    });

    return id;
}

void ShaderCompiler::Tick() {
    utl::vector<PendingResult> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = std::move(completed_);
        completed_.clear();
    }

    for (auto& p : pending) {
        if (p.callback) {
            p.callback(p.request_id, std::move(p.result));
        }
    }
}

} // namespace primal::graphics::shader_ir
