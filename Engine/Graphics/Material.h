#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics {

enum class PipelineFlags : u8 {
    None = 0,
    DepthOnly = 1 << 0,
    DepthEqual = 1 << 1,
    Shadow = 1 << 2,
    Reflection = 1 << 3
};
inline PipelineFlags operator|(PipelineFlags a, PipelineFlags b) { return static_cast<PipelineFlags>(static_cast<u8>(a) | static_cast<u8>(b)); }
inline PipelineFlags operator&(PipelineFlags a, PipelineFlags b) { return static_cast<PipelineFlags>(static_cast<u8>(a) & static_cast<u8>(b)); }

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
    const utl::vector<rhi::VertexInputAttribute>& GetVertexAttributes() const { return vertexAttributes_; }
    void SetVertexBindings(const utl::vector<rhi::VertexInputBinding>& bindings);
    const utl::vector<rhi::VertexInputBinding>& GetVertexBindings() const { return vertexBindings_; }

    void SetBlendState(const rhi::BlendState& state);
    const rhi::BlendState& GetBlendState() const { return blendState_; }
    void SetDepthStencilState(const rhi::DepthStencilState& state);
    const rhi::DepthStencilState& GetDepthStencilState() const { return depthStencilState_; }
    void SetRasterizerState(const rhi::RasterizerState& state);
    const rhi::RasterizerState& GetRasterizerState() const { return rasterizerState_; }
    void SetTopology(rhi::PrimitiveTopology topology);
    rhi::PrimitiveTopology GetTopology() const { return topology_; }

    void SetPipelineLayout(rhi::PipelineLayoutHandle layout);
    rhi::PipelineLayoutHandle GetPipelineLayout() const { return layout_; }
    void SetDescriptorSetLayout(rhi::DescriptorSetLayoutHandle layout);
    rhi::DescriptorSetLayoutHandle GetDescriptorSetLayout() const { return descriptorSetLayout_; }

    void SetUniformBlockSize(u32 size);
    u32 GetUniformBlockSize() const { return uniformBlockSize_; }

    void SetUniformBufferBinding(u32 binding);
    u32 GetUniformBufferBinding() const { return uniformBufferBinding_; }

    void SetRenderTargetFormats(const utl::vector<rhi::DataFormat>& formats, rhi::DataFormat depthStencilFormat = rhi::DataFormat::Unknown);
    
    rhi::ShaderHandle GetShader(rhi::ShaderStage stage, u32 permutationId = 0) const;

    rhi::PipelineHandle GetPipeline(rhi::RHIDeviceBase* device, rhi::RenderPassHandle renderPass, u32 permutationId = 0, PipelineFlags flags = PipelineFlags::None);
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
        PipelineFlags flags;
        bool operator==(const PipelineKey& other) const {
            return renderPass == other.renderPass && permutationId == other.permutationId && flags == other.flags;
        }
    };

    struct PipelineKeyHash {
        std::size_t operator()(const PipelineKey& k) const {
            std::size_t h1 = std::hash<u64>{}(k.renderPass);
            std::size_t h2 = std::hash<u32>{}(k.permutationId);
            std::size_t h3 = std::hash<u8>{}(static_cast<u8>(k.flags));
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    std::unordered_map<PipelineKey, rhi::PipelineHandle, PipelineKeyHash> pipelineCache_;
    std::mutex pipelineMutex_;
    rhi::RHIDeviceBase* device_{nullptr};

    void InvalidatePipelinesLocked();
};

} // namespace primal::graphics
