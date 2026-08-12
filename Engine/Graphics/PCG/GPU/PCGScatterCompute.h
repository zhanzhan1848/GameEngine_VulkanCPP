#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/PCG/PCGTypes.h"
#include "Utilities/Math.h"
#include <vector>
#include <fstream>
#include <sstream>

namespace primal::graphics::pcg {

// GPU-accelerated scatter pass for PCG point generation.
// Dispatches a compute shader that evaluates noise density per grid cell
// and outputs accepted positions to a GPU buffer.
//
// Usage:
//   PCGScatterCompute gpu_scatter;
//   gpu_scatter.Initialize(device);
//   gpu_scatter.SetParams({...});
//   gpu_scatter.DispatchAndReadback(cmd);
class PCGScatterCompute {
public:
    struct ScatterParams {
        math::v3 bounds_min{-50, 0, -50};
        math::v3 bounds_max{50, 5, 50};
        f32 target_count{1000};
        u32 seed{42};
        f32 density_scale{1.0f};
        bool use_noise{true};
    };

    bool Initialize(rhi::RHIDeviceBase* device) {
        device_ = device;

        // Load scatter compute shader
        const char* shader_path =
            "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/PCG/Scatter.metal";
        std::string source = ReadFileToString(shader_path);
        if (source.empty()) return false;

        auto code = std::vector<u8>(source.begin(), source.end());
        auto shader = device_->CreateShader(code.data(), code.size(),
                                            rhi::ShaderStage::Compute, "pcg_scatter");
        if (shader == rhi::handles::INVALID_SHADER) return false;

        // Create descriptor set layout (2 storage buffers for positions + counter)
        rhi::DescriptorSetLayoutBinding bindings[] = {
            {0, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr},
            {1, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr},
        };
        rhi::DescriptorSetLayoutDesc layoutDesc{2, bindings};
        auto set_layout = device_->CreateDescriptorSetLayout(layoutDesc);

        rhi::PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &set_layout;
        auto layout = device_->CreatePipelineLayout(plDesc);

        // Create compute pipeline
        rhi::ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = shader;
        pipeDesc.layout = layout;
        pipeDesc.threadGroupSize = {256, 1, 1};
        pipeline_ = device_->CreateComputePipeline(pipeDesc);

        if (pipeline_ == rhi::handles::INVALID_PIPELINE) return false;

        initialized_ = true;
        return true;
    }

    void SetParams(const ScatterParams& params) { params_ = params; }

    // Dispatch the compute scatter and readback results synchronously.
    // Uses MetalDevice directly for command buffer creation and sync.
    PCGPointSet DispatchAndReadback() {
        PCGPointSet result;

        if (!device_ || !initialized_) return result;

        // Create internal command buffer via MetalDevice
        auto* metal_dev = static_cast<rhi::MetalDevice*>(device_);
        auto cmd_handle = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
        if (cmd_handle == rhi::handles::INVALID_COMMAND_BUFFER) return result;

        auto* cmd = metal_dev->GetCommandBuffer(cmd_handle);
        if (!cmd || !cmd->Initialize() || !cmd->Begin()) {
            device_->DestroyCommandBuffer(cmd_handle);
            return result;
        }

        auto& p = params_;
        f32 range_x = p.bounds_max.x - p.bounds_min.x;
        f32 range_z = p.bounds_max.z - p.bounds_min.z;
        u32 grid_x = static_cast<u32>(std::sqrt(p.target_count * range_x / range_z));
        u32 grid_z = static_cast<u32>(p.target_count / std::max(grid_x, 1u));
        if (grid_x == 0) grid_x = 1;
        if (grid_z == 0) grid_z = 1;
        u32 total_cells = grid_x * grid_z;

        f32 cell_x = range_x / grid_x;
        f32 cell_z = range_z / grid_z;
        f32 y = (p.bounds_min.y + p.bounds_max.y) * 0.5f;

        u32 max_output = total_cells;
        u64 positions_size = max_output * sizeof(math::v4);
        u64 counter_size = sizeof(u32);

        // Create buffers
        rhi::BufferDesc pos_desc{};
        pos_desc.size = positions_size;
        pos_desc.type = rhi::BufferType::Structured;
        pos_desc.usage = rhi::GPUMemoryUsage::Dynamic;
        pos_desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        auto positions_buf = device_->CreateBuffer(pos_desc);

        rhi::BufferDesc cnt_desc{};
        cnt_desc.size = counter_size;
        cnt_desc.usage = rhi::GPUMemoryUsage::Dynamic;
        cnt_desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        auto counter_buf = device_->CreateBuffer(cnt_desc);

        if (positions_buf == rhi::handles::INVALID_RESOURCE ||
            counter_buf == rhi::handles::INVALID_RESOURCE) {
            return result;
        }

        // Zero the counter
        void* cnt_ptr = device_->MapBuffer(counter_buf, 0, counter_size);
        if (cnt_ptr) { memset(cnt_ptr, 0, counter_size); device_->UnmapBuffer(counter_buf); }

        // Upload params
        struct GPUScatterParams {
            f32 bounds_min_x, bounds_min_y, bounds_min_z;
            f32 bounds_max_x, bounds_max_y, bounds_max_z;
            u32 grid_x, grid_z;
            f32 cell_x, cell_z;
            f32 y_position;
            u32 max_output;
            u32 seed;
            f32 density_threshold;
            u32 use_noise_field;
        };

        GPUScatterParams gpu_params{};
        gpu_params.bounds_min_x = p.bounds_min.x;
        gpu_params.bounds_min_y = p.bounds_min.y;
        gpu_params.bounds_min_z = p.bounds_min.z;
        gpu_params.bounds_max_x = p.bounds_max.x;
        gpu_params.bounds_max_y = p.bounds_max.y;
        gpu_params.bounds_max_z = p.bounds_max.z;
        gpu_params.grid_x = grid_x;
        gpu_params.grid_z = grid_z;
        gpu_params.cell_x = cell_x;
        gpu_params.cell_z = cell_z;
        gpu_params.y_position = y;
        gpu_params.max_output = max_output;
        gpu_params.seed = p.seed;
        gpu_params.density_threshold = p.density_scale;
        gpu_params.use_noise_field = p.use_noise ? 1 : 0;

        // Bind pipeline, buffers, params, dispatch
        cmd->BindComputePipeline(pipeline_);

        rhi::ResourceHandle buffers[] = {positions_buf, counter_buf};
        u64 offsets[] = {0, 0};
        auto* metal_cmd = static_cast<rhi::MetalCommandBuffer*>(cmd);
        metal_cmd->BindComputeBuffers(0, 2, buffers, offsets);
        cmd->SetComputeBytes(2, &gpu_params, sizeof(GPUScatterParams));

        u32 threadgroup_size = 256;
        u32 num_groups = (total_cells + threadgroup_size - 1) / threadgroup_size;
        cmd->Dispatch(num_groups, 1, 1);

        // Submit and wait for GPU to finish before readback
        cmd->End();

        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmd_handle;
        device_->Submit(submitInfo);

        // Wait for GPU completion
        cmd->WaitForCompletion();

        device_->DestroyCommandBuffer(cmd_handle);

        // Readback — shared memory is now coherent after GPU completion
        void* cnt_data = device_->MapBuffer(counter_buf, 0, counter_size);
        u32 count = 0;
        if (cnt_data) { count = *static_cast<u32*>(cnt_data); device_->UnmapBuffer(counter_buf); }

        if (count > 0) {
            count = std::min(count, max_output);
            void* pos_data = device_->MapBuffer(positions_buf, 0, count * sizeof(math::v4));
            if (pos_data) {
                result.Init(count);
                auto* positions = static_cast<math::v4*>(pos_data);
                for (u32 i = 0; i < count; ++i) {
                    result.positions[i] = math::v3{positions[i].x, positions[i].y, positions[i].z};
                    result.SetAttr(i, PCGAttr::Density, positions[i].w);
                }
                device_->UnmapBuffer(positions_buf);
            }
        }

        device_->DestroyBuffer(positions_buf);
        device_->DestroyBuffer(counter_buf);

        return result;
    }

    bool IsInitialized() const { return initialized_; }

private:
    static std::string ReadFileToString(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return {};
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    rhi::RHIDeviceBase* device_{nullptr};
    rhi::PipelineHandle pipeline_{rhi::handles::INVALID_PIPELINE};
    ScatterParams params_;
    bool initialized_{false};
};

} // namespace primal::graphics::pcg
