#include "LumenSSAOPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <set>

namespace primal::graphics::lumen {

using namespace rhi;
using namespace rendergraph;

namespace {

// ============================================================================
// Descriptor update helper
// ============================================================================

struct DescriptorData {
    uint32_t binding;
    DescriptorType type;
    ResourceHandle resource;
    uint32_t count = 1;
};

static void UpdateDescriptorSet(RHIDeviceBase* device, DescriptorSetHandle set,
                                const DescriptorData* params, uint32_t count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);

    for (uint32_t i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;

        if (params[i].type == DescriptorType::UniformBuffer ||
            params[i].type == DescriptorType::StorageBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = ~0ull;
            writes[i].bufferInfo = &bufferInfos[i];
        } else if (params[i].type == DescriptorType::SampledImage ||
                   params[i].type == DescriptorType::StorageImage ||
                   params[i].type == DescriptorType::CombinedImageSampler ||
                   params[i].type == DescriptorType::Sampler) {
            if (params[i].type == DescriptorType::Sampler) {
                imageInfos[i].sampler = static_cast<SamplerHandle>(params[i].resource);
            } else {
                imageInfos[i].imageView = params[i].resource;
                imageInfos[i].imageLayout = ResourceState::ShaderResource;
            }
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

// ============================================================================
// Shader source loader with #include resolution
// ============================================================================

static const std::string SHADER_BASE_DIR = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/";
static const std::string LUMEN_SHADER_DIR = SHADER_BASE_DIR + "Lumen/";

static std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static std::string ResolveIncludes(const std::string& source, const std::string& baseDir,
                                   std::set<std::string>& included) {
    std::istringstream in(source);
    std::ostringstream out;
    std::string line;

    while (std::getline(in, line)) {
        std::string trimmed = line;
        size_t firstNonSpace = trimmed.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos) trimmed = trimmed.substr(firstNonSpace);

        if (trimmed.find("#include \"") == 0) {
            size_t start = trimmed.find('"') + 1;
            size_t end = trimmed.find('"', start);
            if (start != std::string::npos && end != std::string::npos) {
                std::string includeFile = trimmed.substr(start, end - start);

                std::string fullPath = baseDir + includeFile;
                if (std::ifstream(fullPath).good() == false) {
                    fullPath = SHADER_BASE_DIR + includeFile;
                }

                if (included.find(fullPath) == included.end()) {
                    included.insert(fullPath);
                    std::string includedContent = ReadFileToString(fullPath);
                    if (!includedContent.empty()) {
                        std::string resolved = ResolveIncludes(includedContent,
                            fullPath.substr(0, fullPath.find_last_of('/') + 1), included);
                        out << resolved << "\n";
                    } else {
                        out << line << "\n";
                    }
                }
                continue;
            }
        }
        out << line << "\n";
    }
    return out.str();
}

static std::vector<u8> LoadShaderBytecode(const char* shaderName) {
    std::string shaderPath = LUMEN_SHADER_DIR + shaderName + ".metal";

    std::string source = ReadFileToString(shaderPath);
    if (source.empty()) {
        shaderPath = std::string("Engine/Graphics/Metal/shaders/Lumen/") + shaderName + ".metal";
        source = ReadFileToString(shaderPath);
    }

    if (source.empty()) {
        std::cerr << "[LumenSSAO] Failed to load shader: " << shaderName << std::endl;
        return {};
    }

    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, LUMEN_SHADER_DIR, included);

    return std::vector<u8>(resolved.begin(), resolved.end());
}

// Per-pass data for render graph
struct LumenSSAOData {
    RGResourceHandle ssao_trace;
    RGResourceHandle ssao_output;
};

} // anonymous namespace

// ============================================================================
// LumenSSAOPass Implementation
// ============================================================================

LumenSSAOPass::~LumenSSAOPass() {
    Shutdown();
}

bool LumenSSAOPass::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height,
                               const SSAOParams& params) {
    if (initialized_) return true;

    // T4.6.3: Lumen SSAO deferred on Vulkan — CreateDescriptorSetLayouts() uses
    // Metal's overlapping texture/buffer binding idiom (rejected by Vulkan), and
    // no SPIR-V ports of the SSAO shaders exist yet. See LumenDDGIPass::Initialize
    // for the full rationale.
    if (device && device->GetPlatform() == rhi::RHIPlatform::Vulkan) {
        std::cerr << "[LumenSSAO] Skipped on Vulkan (deferred — needs SPIR-V ports + "
                     "non-overlapping descriptor bindings)" << std::endl;
        return false;
    }

    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    params_ = params;

    u32 half_w = render_width / 2;
    u32 half_h = render_height / 2;

    CreateDescriptorSetLayouts();
    CreatePipelines();
    CreateConstantBuffers();
    CreatePersistentTextures();

    initialized_ = true;

    std::cout << "[LumenSSAO] Initialized (" << render_width << "x" << render_height
              << ", half-res: " << half_w << "x" << half_h << ")" << std::endl;
    return true;
}

void LumenSSAOPass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    device_ = nullptr;
}

// ============================================================================
// Private helper methods
// ============================================================================

void LumenSSAOPass::CreateDescriptorSetLayouts() {
    // --- Trace: 2 sampled + 1 storage + 2 UBO ---
    {
        DescriptorSetLayoutBinding traceBindings[] = {
            // Textures (sampled)
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // normal
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // depth
            // Texture (storage)
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},   // output
            // Buffers (separate Metal namespace)
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // SSAOTraceParams
        };
        DescriptorSetLayoutDesc layoutDesc{5, traceBindings};
        trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Filter: 3 sampled + 1 storage + 2 UBO ---
    {
        DescriptorSetLayoutBinding filterBindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // ssao half-res
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // normal
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},   // depth
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},   // output
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // GlobalShaderData
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},   // FilterParams
        };
        DescriptorSetLayoutDesc layoutDesc{6, filterBindings};
        filter_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }
}

void LumenSSAOPass::CreatePipelines() {
    auto CompileShader = [&](const char* name, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(name);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    auto traceShader = CompileShader("SSAOTrace", "ssao_trace");
    auto filterShader = CompileShader("SSAOFilter", "ssao_filter");

    if (traceShader == handles::INVALID_SHADER ||
        filterShader == handles::INVALID_SHADER) {
        std::cerr << "[LumenSSAO] Shader compilation failed" << std::endl;
        return;
    }

    // Create pipeline layouts
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &trace_set_layout_;
        trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &filter_set_layout_;
        filter_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // Create compute pipelines
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = traceShader;
        pipeDesc.layout = trace_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = filterShader;
        pipeDesc.layout = filter_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        filter_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    // Create triple-buffered descriptor sets
    for (int i = 0; i < 3; i++) {
        {
            DescriptorSetDesc dsDesc{trace_set_layout_};
            trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{filter_set_layout_};
            filter_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
    }
}

void LumenSSAOPass::CreateConstantBuffers() {
    auto CreateCBs = [&](ResourceHandle (&cbs)[3], u64 size) {
        for (int i = 0; i < 3; i++) {
            BufferDesc desc{};
            desc.size = size;
            desc.type = BufferType::Constant;
            desc.usage = GPUMemoryUsage::Dynamic;
            desc.memoryUsage = GPUMemoryUsage::Dynamic;
            cbs[i] = device_->CreateBuffer(desc);
        }
    };

    CreateCBs(global_cb_, 512);           // GlobalShaderData
    CreateCBs(trace_params_cb_, 64);      // SSAOTraceParams
    CreateCBs(filter_params_cb_, 32);     // SSAOFilterParams
}

void LumenSSAOPass::CreatePersistentTextures() {
    u32 half_w = render_width_ / 2;
    u32 half_h = render_height_ / 2;

    // Half-res trace output (R16_Float)
    {
        TextureDesc desc{};
        desc.size = {half_w, half_h, 1};
        desc.format = DataFormat::R16_Float;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        trace_texture_ = device_->CreateTexture(desc);
    }

    // Full-res filter output (R16_Float)
    {
        TextureDesc desc{};
        desc.size = {render_width_, render_height_, 1};
        desc.format = DataFormat::R16_Float;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        filter_texture_ = device_->CreateTexture(desc);
    }
}

// ============================================================================
// AddPass — main entry point called per frame
// ============================================================================

LumenSSAOOutput LumenSSAOPass::AddPass(
    RenderGraph& graph,
    RGResourceHandle gbuffer_normal,
    RGResourceHandle gbuffer_depth,
    const SSAOCameraData& camera_data,
    u32 current_frame_index)
{
    LumenSSAOOutput output{};

    auto traceHandle = graph.ImportResource("LumenSSAO_Trace", trace_texture_);
    auto filterHandle = graph.ImportResource("LumenSSAO_Filter", filter_texture_);

    output.ssao_output = filterHandle;

    graph.AddPass<LumenSSAOData>("LumenSSAO",
        RGPassType::Compute, RGPassCategory::Lighting,

        // Setup lambda
        [gbuffer_normal, gbuffer_depth, traceHandle, filterHandle](
            LumenSSAOData& data, RenderGraphBuilder& builder) {
            builder.Read(gbuffer_normal, ResourceState::ShaderResource);
            builder.Read(gbuffer_depth, ResourceState::ShaderResource);

            data.ssao_trace = builder.Write(traceHandle, ResourceState::UnorderedAccess);
            data.ssao_output = builder.Write(filterHandle, ResourceState::UnorderedAccess);
        },

        // Execute lambda
        [this, camera_data, current_frame_index, gbuffer_normal, gbuffer_depth](
            const LumenSSAOData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            u32 frameIdx = current_frame_index % 3;
            u32 half_w = render_width_ / 2;
            u32 half_h = render_height_ / 2;
            constexpr u32 TG = 8;

            auto ResolveTexture = [&](RGResourceHandle handle) -> ResourceHandle {
                auto* res = context.graph->GetResource(handle);
                if (res) return res->GetPhysicalHandle();
                return handles::INVALID_RESOURCE;
            };

            ResourceHandle normalTex = ResolveTexture(gbuffer_normal);
            ResourceHandle depthTex  = ResolveTexture(gbuffer_depth);

            // ---- Upload GlobalShaderData ----
            {
                struct GlobalShaderData {
                    math::m4x4 View;
                    math::m4x4 Projection;
                    math::m4x4 InvProjection;
                    math::m4x4 ViewProjection;
                    math::m4x4 PreviousViewProjection;
                    math::m4x4 InvViewProjection;
                    math::v4   CameraPositionAndViewWidth;
                    math::v4   CameraDirectionAndViewHeight;
                    u32        NumDirectionalLights;
                    float      DeltaTime;
                    float      FrameCount;
                    float      padding;
                };

                auto* mapped = static_cast<GlobalShaderData*>(device_->MapBuffer(global_cb_[frameIdx]));
                if (mapped) {
                    mapped->View = camera_data.view_matrix;
                    mapped->Projection = camera_data.proj_matrix;
                    mapped->InvProjection = rhi::math::Inverse(camera_data.proj_matrix);
                    mapped->ViewProjection = camera_data.view_matrix * camera_data.proj_matrix;
                    mapped->PreviousViewProjection = camera_data.prev_view_matrix * camera_data.prev_proj_matrix;
                    mapped->InvViewProjection = rhi::math::Inverse(mapped->ViewProjection);

                    math::m4x4 invView = rhi::math::Inverse(camera_data.view_matrix);
                    mapped->CameraPositionAndViewWidth = {
                        invView.columns[3][0], invView.columns[3][1],
                        invView.columns[3][2], (float)render_width_
                    };

                    auto& vm = camera_data.view_matrix;
                    mapped->CameraDirectionAndViewHeight = {
                        -vm.columns[2][0], -vm.columns[2][1],
                        -vm.columns[2][2], (float)render_height_
                    };

                    mapped->NumDirectionalLights = 0;
                    mapped->DeltaTime = camera_data.delta_time;
                    mapped->FrameCount = (float)camera_data.frame_index;
                    device_->UnmapBuffer(global_cb_[frameIdx]);
                }
            }

            // ================================================================
            // Sub-pass 1: GTAO Trace (half-res)
            // ================================================================
            if (trace_pipeline_ != handles::INVALID_PIPELINE &&
                trace_texture_ != handles::INVALID_RESOURCE) {
                struct SSAOTraceParamsData {
                    float radius;
                    float power;
                    u32   direction_count;
                    u32   sample_count;
                    u32   frame_index;
                    u32   output_width;
                    u32   output_height;
                    float near_plane;
                    float far_plane;
                };
                auto* traceParams = static_cast<SSAOTraceParamsData*>(device_->MapBuffer(trace_params_cb_[frameIdx]));
                if (traceParams) {
                    traceParams->radius = params_.radius;
                    traceParams->power = params_.power;
                    traceParams->direction_count = params_.direction_count;
                    traceParams->sample_count = params_.sample_count;
                    traceParams->frame_index = camera_data.frame_index;
                    traceParams->output_width = half_w;
                    traceParams->output_height = half_h;
                    traceParams->near_plane = 0.1f;
                    traceParams->far_plane = 1000.0f;
                    device_->UnmapBuffer(trace_params_cb_[frameIdx]);
                }

                DescriptorData traceDesc[] = {
                    {0, DescriptorType::SampledImage,  normalTex},
                    {1, DescriptorType::SampledImage,  depthTex},
                    {2, DescriptorType::StorageImage,  trace_texture_},
                    {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    {1, DescriptorType::UniformBuffer, trace_params_cb_[frameIdx]},
                };
                UpdateDescriptorSet(device_, trace_ds_[frameIdx], traceDesc, 5);

                cmd->BindComputePipeline(trace_pipeline_);
                const DescriptorSetHandle sets[] = { trace_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, trace_layout_, 0, 1, sets, 0, nullptr);

                u32 gx = (half_w + TG - 1) / TG;
                u32 gy = (half_h + TG - 1) / TG;
                cmd->Dispatch(gx, gy, 1);
            }

            // Barrier: trace UAV -> SRV
            {
                ResourceBarrier barrier{};
                barrier.resource = trace_texture_;
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ================================================================
            // Sub-pass 2: Bilateral Filter (full-res)
            // ================================================================
            if (filter_pipeline_ != handles::INVALID_PIPELINE &&
                filter_texture_ != handles::INVALID_RESOURCE) {
                struct FilterParamsData {
                    float sigma_depth;
                    float sigma_normal;
                    u32   kernel_radius;
                    u32   full_width;
                    u32   full_height;
                    u32   half_width;
                    u32   half_height;
                };
                auto* filterParams = static_cast<FilterParamsData*>(device_->MapBuffer(filter_params_cb_[frameIdx]));
                if (filterParams) {
                    filterParams->sigma_depth = params_.filter_sigma_depth;
                    filterParams->sigma_normal = params_.filter_sigma_normal;
                    filterParams->kernel_radius = params_.filter_kernel_radius;
                    filterParams->full_width = render_width_;
                    filterParams->full_height = render_height_;
                    filterParams->half_width = half_w;
                    filterParams->half_height = half_h;
                    device_->UnmapBuffer(filter_params_cb_[frameIdx]);
                }

                DescriptorData filterDesc[] = {
                    {0, DescriptorType::SampledImage,  trace_texture_},
                    {1, DescriptorType::SampledImage,  normalTex},
                    {2, DescriptorType::SampledImage,  depthTex},
                    {3, DescriptorType::StorageImage,  filter_texture_},
                    {0, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    {1, DescriptorType::UniformBuffer, filter_params_cb_[frameIdx]},
                };
                UpdateDescriptorSet(device_, filter_ds_[frameIdx], filterDesc, 6);

                cmd->BindComputePipeline(filter_pipeline_);
                const DescriptorSetHandle filterSets[] = { filter_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, filter_layout_, 0, 1, filterSets, 0, nullptr);

                u32 gx = (render_width_ + TG - 1) / TG;
                u32 gy = (render_height_ + TG - 1) / TG;
                cmd->Dispatch(gx, gy, 1);
            }

        }
    );

    return output;
}

} // namespace primal::graphics::lumen
