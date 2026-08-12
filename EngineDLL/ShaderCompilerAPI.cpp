#if defined(_MSC_VER)
#include "Common.h"
#include "CommonHeaders.h"
#include "Graphics/ShaderIR/ShaderCompiler.h"
#include "Graphics/MaterialGraph/MaterialGraph.h"
#include "Graphics/RenderPipeline/RenderPipeline.h"
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include <cstring>

#pragma comment(lib, "Engine.lib")

#elif defined(__clang__)
#include "Common.h"
#include "CommonHeaders.h"
#include "Graphics/ShaderIR/ShaderCompiler.h"
#include "Graphics/MaterialGraph/MaterialGraph.h"
#include "Graphics/RenderPipeline/RenderPipeline.h"
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include <cstring>

#endif

using namespace primal;
using namespace primal::graphics;

namespace {

StandardRenderPipeline* GetStdPipeline() {
    auto* p = RenderPipeline::Get();
    return p ? static_cast<StandardRenderPipeline*>(p) : nullptr;
}

} // anonymous namespace

// ============================================================================
// Shader Compilation
// ============================================================================

EDITOR_INTERFACE u32 CompileMaterialShader(
    const void* graph_ptr,
    char* out_source, u32* io_source_size,
    char* out_error, u32 error_buf_size)
{
    if (!graph_ptr || !io_source_size) return 0;

    auto& graph = *static_cast<const material_graph::MaterialGraph*>(graph_ptr);
    auto result = shader_ir::ShaderCompiler::CompileSync(graph);

    if (!result.success) {
        if (out_error && error_buf_size > 0) {
            u32 len = (u32)result.error.size();
            u32 copy = len < error_buf_size - 1 ? len : error_buf_size - 1;
            std::memcpy(out_error, result.error.c_str(), copy);
            out_error[copy] = '\0';
        }
        if (io_source_size) *io_source_size = 0;
        return 2; // compile error
    }

    u32 src_len = (u32)result.source.size();
    if (out_source && *io_source_size > 0) {
        u32 copy = src_len < *io_source_size - 1 ? src_len : *io_source_size - 1;
        std::memcpy(out_source, result.source.c_str(), copy);
        out_source[copy] = '\0';
    }
    *io_source_size = src_len + 1; // include null terminator
    return 1; // success
}

// ============================================================================
// Shader Hot-Reload
// ============================================================================

EDITOR_INTERFACE u32 ApplyMaterialShader(
    u64 shader_handle,
    const char* source, u32 source_size)
{
    if (!source || source_size == 0) return 0;
    auto* p = GetStdPipeline();
    if (!p) return 0;
    return p->ReloadShader(
        static_cast<rhi::ShaderHandle>(shader_handle), source, source_size) ? 1u : 0u;
}
