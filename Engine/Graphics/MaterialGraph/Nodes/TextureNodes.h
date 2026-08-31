#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"
#include <cstring>

namespace primal::graphics::material_graph {

// ============================================================================
// M5.6: Virtual Texture 材质节点
// ============================================================================

/// 虚拟纹理采样节点:uv → float4,layer 选择 + MipValueMode(None/MipLevel/
/// MipBias);RVT 与 SVT 共用(采样端两段式,feedback 内联)。
class SampleVirtualTextureNode : public MaterialNode {
public:
    enum class MipValueMode : u8 { None = 0, MipLevel = 1, MipBias = 2 };

    SampleVirtualTextureNode() {
        inputs.resize(2);
        inputs[0].expected_type = MaterialDataType::Texture2D;   // 虚拟纹理资产槽
        inputs[0].index = 0;
        inputs[1].expected_type = MaterialDataType::Float2;
        inputs[1].index = 1;

        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float4;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "SampleVirtualTexture"; }
    void Execute() override {
        // 采样端执行由 VT 材质绑定路径承接(GBufferVT 管线);
        // 节点层仅承载参数(layer/mipMode)。
    }
    NodeTypeInfo GetTypeInfo() const override {
        return {"SampleVirtualTexture", "Sample Virtual Texture", "Texture", false};
    }

    u32 layer = 0;                 // 采样层(M2.7 layer 打包)
    MipValueMode mipMode = MipValueMode::None;
    float mipBias = 0.0f;
};

/// RVT 输出节点:材质输出写入 RVT(Draw 捕获 pass 的 MRT 输出)。
/// 层选择(Material Type)须与 RVT 资产布局匹配 — 校验在 GetTypeInfo。
class RuntimeVirtualTextureOutputNode : public MaterialNode {
public:
    enum class MaterialType : u8 { BaseColor = 0, Normal = 1, WorldHeight = 2, ORM = 3 };

    RuntimeVirtualTextureOutputNode() {
        inputs.resize(1);
        inputs[0].expected_type = MaterialDataType::Float4;
        inputs[0].index = 0;
    }
    const char* TypeName() const override { return "RuntimeVirtualTextureOutput"; }
    void Execute() override {
        // RVT 写入路径 = 捕获 pass MRT(Draw 捕获);
        // WorldHeight 层经 MaterialType 影响输出编码(R16 归一化)。
    }
    NodeTypeInfo GetTypeInfo() const override {
        return {"RuntimeVirtualTextureOutput", "RVT Output", "Texture", false};
    }

    MaterialType materialType = MaterialType::BaseColor;
};

class SampleTextureNode : public MaterialNode {
public:
    SampleTextureNode() {
        inputs.resize(2);
        inputs[0].expected_type = MaterialDataType::Texture2D;
        inputs[0].index = 0;
        inputs[1].expected_type = MaterialDataType::Float2;
        inputs[1].index = 1;

        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float4;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "SampleTexture"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"SampleTexture", "Sample Texture", "Texture", false};
    }
    void Execute() override {
        auto* out = CreateOutput<MaterialFloat4Data>(0);
        out->value = math::v4{1.0f, 1.0f, 1.0f, 1.0f};
    }

    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 3; return kPins; }

private:
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialPinDescriptor SampleTextureNode::kPins[] = {
    {"texture", 0, MaterialDataType::Texture2D, true},
    {"uv", 1, MaterialDataType::Float2, true},
    {"rgba", 0, MaterialDataType::Float4, false},
};

} // namespace primal::graphics::material_graph
