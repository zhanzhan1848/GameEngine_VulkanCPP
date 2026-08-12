#include "Engine/Graphics/Nanite/Providers/AnalyticSDFProvider.h"
#include "Engine/Graphics/Nanite/GlobalSDF.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace primal::graphics::nanite {

namespace {

const std::string SHADER_DIR =
    "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/Nanite/";

std::vector<u8> LoadShaderSource(const char* name) {
    std::string path = SHADER_DIR + std::string(name) + ".metal";
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[AnalyticSDFProvider] Failed to load shader: " << path << std::endl;
        return {};
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string s = ss.str();
    return std::vector<u8>(s.begin(), s.end());
}

// Metal CascadeUniforms mirror — keep C++ & shader layouts byte-identical.
// 32 bytes, alignas(16) for constant buffer binding.
struct alignas(16) AnalyticSDFParamsCB {
    f32 center_x, center_y, center_z;   // offset 0-11
    f32 radius;                         // offset 12-15
    f32 origin_x, origin_y, origin_z;   // offset 16-27
    f32 voxel_size;                     // offset 28-31
};
static_assert(sizeof(AnalyticSDFParamsCB) == 32, "CB layout drift");

struct DescriptorData {
    u32 binding;
    rhi::DescriptorType type;
    rhi::ResourceHandle resource;
};

void UpdateDescriptorSet(rhi::RHIDeviceBase* device, rhi::DescriptorSetHandle set,
                          const DescriptorData* params, u32 count) {
    std::vector<rhi::WriteDescriptorSet> writes(count);
    std::vector<rhi::DescriptorBufferInfo> bufferInfos(count);
    std::vector<rhi::DescriptorImageInfo> imageInfos(count);

    for (u32 i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = params[i].type;

        if (params[i].type == rhi::DescriptorType::UniformBuffer ||
            params[i].type == rhi::DescriptorType::StorageBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = ~0ull;
            writes[i].bufferInfo = &bufferInfos[i];
        } else if (params[i].type == rhi::DescriptorType::StorageImage) {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = rhi::ResourceState::UnorderedAccess;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

} // anonymous namespace

AnalyticSDFProvider::~AnalyticSDFProvider() {
    if (!device_) return;

    for (int i = 0; i < 3; ++i) {
        if (ds_[i] != rhi::handles::INVALID_DESCRIPTOR_SET)
            device_->DestroyDescriptorSet(ds_[i]);
        if (params_cb_[i] != rhi::handles::INVALID_RESOURCE)
            device_->DestroyBuffer(params_cb_[i]);
    }
    if (pipeline_     != rhi::handles::INVALID_PIPELINE)        device_->DestroyPipeline(pipeline_);
    if (layout_       != rhi::handles::INVALID_PIPELINE_LAYOUT)  device_->DestroyPipelineLayout(layout_);
    if (set_layout_   != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        device_->DestroyDescriptorSetLayout(set_layout_);
    if (shader_       != rhi::handles::INVALID_SHADER)          device_->DestroyShader(shader_);
}

bool AnalyticSDFProvider::Initialize(rhi::RHIDeviceBase* device) {
    if (!device) return false;
    device_ = device;

    auto code = LoadShaderSource("AnalyticSDF");
    if (code.empty()) return false;

    shader_ = device_->CreateShader(code.data(), code.size(),
                                     rhi::ShaderStage::Compute, "analytic_sdf_fill");
    if (shader_ == rhi::handles::INVALID_SHADER) {
        std::cerr << "[AnalyticSDFProvider] Shader compile failed" << std::endl;
        return false;
    }

    {
        rhi::DescriptorSetLayoutBinding bindings[] = {
            {0, rhi::DescriptorType::StorageImage,  1, rhi::ShaderStage::Compute, nullptr},
            {0, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Compute, nullptr},
        };
        rhi::DescriptorSetLayoutDesc layoutDesc{2, bindings};
        set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    {
        rhi::PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &set_layout_;
        layout_ = device_->CreatePipelineLayout(plDesc);
    }

    {
        rhi::ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = shader_;
        pipeDesc.layout = layout_;
        pipeDesc.threadGroupSize = {4, 4, 4};
        pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    if (pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[AnalyticSDFProvider] Pipeline creation failed" << std::endl;
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        rhi::BufferDesc cbDesc{};
        cbDesc.size = sizeof(AnalyticSDFParamsCB);
        cbDesc.type = rhi::BufferType::Constant;
        cbDesc.usage = rhi::GPUMemoryUsage::Dynamic;
        cbDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        params_cb_[i] = device_->CreateBuffer(cbDesc);

        rhi::DescriptorSetDesc dsDesc{set_layout_};
        ds_[i] = device_->CreateDescriptorSet(dsDesc);
    }

    std::cout << "[AnalyticSDFProvider] Initialized (sphere center="
              << sphere_.center.x << "," << sphere_.center.y << "," << sphere_.center.z
              << " radius=" << sphere_.radius << ")" << std::endl;
    return true;
}

void AnalyticSDFProvider::DispatchCascade(rhi::RHICommandBuffer* cmd,
                                           u32 frame_index,
                                           const SDFCascade& cascade) {
    if (!IsReady() || !cmd) return;
    if (cascade.sdf_texture == rhi::handles::INVALID_RESOURCE) return;

    // Transition SRV → UAV: the previous frame's DispatchCascade ended with a
    // UAV→SRV barrier (line below), leaving the texture in ShaderResource state.
    // Without this transition, binding as StorageImage and writing is a state
    // hazard — Metal's hazard tracking may or may not catch it depending on
    // whether the texture is heap-tracked. With shared (non-ping-pong) texture
    // across frames, an in-flight SRV read from the previous frame can race
    // against this frame's UAV write, producing torn reads that surface as NaN
    // in SurfaceNets scalar buffer around frame ~55 once in-flight queue depth
    // stabilizes.
    {
        rhi::ResourceBarrier barrier{};
        barrier.resource     = cascade.sdf_texture;
        barrier.beforeState  = rhi::ResourceState::ShaderResource;
        barrier.afterState   = rhi::ResourceState::UnorderedAccess;
        barrier.subresource  = 0xFFFFFFFF;
        cmd->InsertBarrier(&barrier, 1);
    }

    static int s_call_count = 0;
    if (s_call_count < 3) {
        std::cout << "[AnalyticSDFProvider] Dispatch #" << s_call_count
                  << " frame=" << frame_index
                  << " cascade.origin=(" << cascade.origin.x << "," << cascade.origin.y << "," << cascade.origin.z << ")"
                  << " res=" << cascade.resolution
                  << " voxel=" << cascade.voxel_size
                  << std::endl;
        ++s_call_count;
    }

    const u32 frameIdx = frame_index % 3;

    {
        auto* mapped = static_cast<AnalyticSDFParamsCB*>(
            device_->MapBuffer(params_cb_[frameIdx]));
        if (mapped) {
            mapped->center_x   = sphere_.center.x;
            mapped->center_y   = sphere_.center.y;
            mapped->center_z   = sphere_.center.z;
            mapped->radius     = sphere_.radius;
            mapped->origin_x   = cascade.origin.x;
            mapped->origin_y   = cascade.origin.y;
            mapped->origin_z   = cascade.origin.z;
            mapped->voxel_size = cascade.voxel_size;
            device_->UnmapBuffer(params_cb_[frameIdx]);
        }
    }

    {
        DescriptorData params[] = {
            {0, rhi::DescriptorType::StorageImage,  cascade.sdf_texture},
            {0, rhi::DescriptorType::UniformBuffer, params_cb_[frameIdx]},
        };
        UpdateDescriptorSet(device_, ds_[frameIdx], params, 2);
    }

    cmd->BindComputePipeline(pipeline_);
    const rhi::DescriptorSetHandle sets[] = { ds_[frameIdx] };
    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, layout_, 0, 1, sets, 0, nullptr);

    const u32 res = cascade.resolution;
    const u32 gx = (res + 3) / 4;
    const u32 gy = (res + 3) / 4;
    const u32 gz = (res + 3) / 4;
    cmd->Dispatch(gx, gy, gz);

    rhi::ResourceBarrier barrier{};
    barrier.resource = cascade.sdf_texture;
    barrier.beforeState = rhi::ResourceState::UnorderedAccess;
    barrier.afterState = rhi::ResourceState::ShaderResource;
    barrier.subresource = 0xFFFFFFFF;
    cmd->InsertBarrier(&barrier, 1);
}

} // namespace primal::graphics::nanite
