#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics {

namespace rhi {
    class RHIDeviceBase;
}

class Material {
public:
    using ShaderBytecode = utl::vector<u8>;

    Material();
    ~Material();

    DISABLE_COPY(Material);
    Material(Material&&) = delete;
    Material& operator=(Material&&) = delete;

    void SetShader(rhi::ShaderStage stage, const void* data, u64 size, const char* entryPoint = "main", u32 permutationId = 0);
    void SetShader(rhi::ShaderStage stage, const ShaderBytecode& bytecode, const char* entryPoint = "main", u32 permutationId = 0);

    void SetVertexAttributes(const utl::vector<rhi::VertexInputAttribute>& attributes);
    void SetVertexBindings(const utl::vector<rhi::VertexInputBinding>& bindings);

    void SetBlendState(const rhi::BlendState& state);
    void SetDepthStencilState(const rhi::DepthStencilState& state);
    void SetRasterizerState(const rhi::RasterizerState& state);
    void SetTopology(rhi::PrimitiveTopology topology);

    void SetPipelineLayout(rhi::PipelineLayoutHandle layout);
    void SetDescriptorSetLayout(rhi::DescriptorSetLayoutHandle layout);
    rhi::DescriptorSetLayoutHandle GetDescriptorSetLayout() const { return descriptorSetLayout_; }

    void SetUniformBlockSize(u32 size);
    u32 GetUniformBlockSize() const { return uniformBlockSize_; }

    void SetUniformBufferBinding(u32 binding);
    u32 GetUniformBufferBinding() const { return uniformBufferBinding_; }

    void SetRenderTargetFormats(const utl::vector<rhi::DataFormat>& formats, rhi::DataFormat depthStencilFormat = rhi::DataFormat::Unknown);

    rhi::PipelineHandle GetPipeline(rhi::RHIDeviceBase* device, rhi::RenderPassHandle renderPass, u32 permutationId = 0);
    void InvalidatePipelines();

private:
    struct ShaderEntry {
        ShaderBytecode bytecode;
        std::string entryPoint;
        rhi::ShaderHandle handle{rhi::handles::INVALID_SHADER};
    };

    std::unordered_map<u32, std::unordered_map<rhi::ShaderStage, ShaderEntry>> shaderVariants_;
    utl::vector<rhi::VertexInputAttribute> vertexAttributes_;
    utl::vector<rhi::VertexInputBinding> vertexBindings_;

    rhi::BlendState blendState_{};
    rhi::DepthStencilState depthStencilState_{};
    rhi::RasterizerState rasterizerState_{};
    rhi::PrimitiveTopology topology_{rhi::PrimitiveTopology::TriangleList};

    rhi::PipelineLayoutHandle layout_{rhi::handles::INVALID_RESOURCE};
    rhi::DescriptorSetLayoutHandle descriptorSetLayout_{rhi::handles::INVALID_RESOURCE};
    u32 uniformBlockSize_{0};
    u32 uniformBufferBinding_{0};
    utl::vector<rhi::DataFormat> renderTargetFormats_;
    rhi::DataFormat depthStencilFormat_{rhi::DataFormat::Unknown};

    struct PipelineKey {
        rhi::RenderPassHandle renderPass;
        u32 permutationId;
        bool operator==(const PipelineKey& other) const {
            return renderPass == other.renderPass && permutationId == other.permutationId;
        }
    };

    struct PipelineKeyHash {
        std::size_t operator()(const PipelineKey& k) const {
            std::size_t h1 = std::hash<u64>{}(k.renderPass);
            std::size_t h2 = std::hash<u32>{}(k.permutationId);
            return h1 ^ (h2 << 1);
        }
    };

    std::unordered_map<PipelineKey, rhi::PipelineHandle, PipelineKeyHash> pipelineCache_;
    std::mutex pipelineMutex_;
    rhi::RHIDeviceBase* device_{nullptr};

    void InvalidatePipelinesLocked();
};

} // namespace primal::graphics
