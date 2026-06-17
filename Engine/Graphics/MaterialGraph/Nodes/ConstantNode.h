#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"
#include <cstring>

namespace primal::graphics::material_graph {

class ConstantFloatNode : public MaterialNode {
public:
    f32 value{0.0f};

    ConstantFloatNode() {
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "ConstantFloat"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"ConstantFloat", "Constant Float", "Constants", false};
    }
    void Execute() override {
        auto* out = CreateOutput<MaterialFloatData>(0);
        out->value = value;
    }

    const MaterialParamDescriptor* GetParamDescriptors(u32& c) const override { c = 1; return kParams; }
    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 1; return kPins; }
    bool SetParamByName(const char* n, f32 v) override {
        if (std::strcmp(n, "value") == 0) { value = v; return true; }
        return false;
    }

private:
    static const MaterialParamDescriptor kParams[];
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialParamDescriptor ConstantFloatNode::kParams[] = {
    {"value", "Constant", MaterialParamType::Float, {-1e6f, 1e6f, 0.01f},
     MAT_OFFSETOF(ConstantFloatNode, value), sizeof(value), nullptr},
};
inline const MaterialPinDescriptor ConstantFloatNode::kPins[] = {
    {"value", 0, MaterialDataType::Float, false},
};

class ConstantFloat3Node : public MaterialNode {
public:
    math::v3 value{0.0f, 0.0f, 0.0f};

    ConstantFloat3Node() {
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float3;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "ConstantFloat3"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"ConstantFloat3", "Constant Float3", "Constants", false};
    }
    void Execute() override {
        auto* out = CreateOutput<MaterialFloat3Data>(0);
        out->value = value;
    }

    const MaterialParamDescriptor* GetParamDescriptors(u32& c) const override { c = 1; return kParams; }
    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 1; return kPins; }
    bool SetParamByName(const char* n, math::v3 v) override {
        if (std::strcmp(n, "value") == 0) { value = v; return true; }
        return false;
    }

private:
    static const MaterialParamDescriptor kParams[];
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialParamDescriptor ConstantFloat3Node::kParams[] = {
    {"value", "Constant", MaterialParamType::Float3, {-1e6f, 1e6f, 0.01f},
     MAT_OFFSETOF(ConstantFloat3Node, value), sizeof(value), nullptr},
};
inline const MaterialPinDescriptor ConstantFloat3Node::kPins[] = {
    {"value", 0, MaterialDataType::Float3, false},
};

class ConstantFloat2Node : public MaterialNode {
public:
    math::v2 value{0.0f, 0.0f};

    ConstantFloat2Node() {
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float2;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "ConstantFloat2"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"ConstantFloat2", "Constant Float2", "Constants", false};
    }
    void Execute() override {
        auto* out = CreateOutput<MaterialFloat2Data>(0);
        out->value = value;
    }

    const MaterialParamDescriptor* GetParamDescriptors(u32& c) const override { c = 1; return kParams; }
    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 1; return kPins; }

private:
    static const MaterialParamDescriptor kParams[];
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialParamDescriptor ConstantFloat2Node::kParams[] = {
    {"value", "Constant", MaterialParamType::Float2, {-1e6f, 1e6f, 0.01f},
     MAT_OFFSETOF(ConstantFloat2Node, value), sizeof(value), nullptr},
};
inline const MaterialPinDescriptor ConstantFloat2Node::kPins[] = {
    {"value", 0, MaterialDataType::Float2, false},
};

class ConstantFloat4Node : public MaterialNode {
public:
    math::v4 value{0.0f, 0.0f, 0.0f, 1.0f};

    ConstantFloat4Node() {
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float4;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "ConstantFloat4"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"ConstantFloat4", "Constant Float4", "Constants", false};
    }
    void Execute() override {
        auto* out = CreateOutput<MaterialFloat4Data>(0);
        out->value = value;
    }

    const MaterialParamDescriptor* GetParamDescriptors(u32& c) const override { c = 1; return kParams; }
    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 1; return kPins; }
    bool SetParamByName(const char* n, math::v4 v) override {
        if (std::strcmp(n, "value") == 0) { value = v; return true; }
        return false;
    }

private:
    static const MaterialParamDescriptor kParams[];
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialParamDescriptor ConstantFloat4Node::kParams[] = {
    {"value", "Constant", MaterialParamType::Float4, {-1e6f, 1e6f, 0.01f},
     MAT_OFFSETOF(ConstantFloat4Node, value), sizeof(value), nullptr},
};
inline const MaterialPinDescriptor ConstantFloat4Node::kPins[] = {
    {"value", 0, MaterialDataType::Float4, false},
};

class ConstantTextureNode : public MaterialNode {
public:
    std::string asset_path;

    ConstantTextureNode() {
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Texture2D;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "ConstantTexture"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"ConstantTexture", "Constant Texture", "Constants", false};
    }
    void Execute() override {
        auto* out = CreateOutput<MaterialTextureData>(0);
        out->asset_path = asset_path;
    }

    const MaterialParamDescriptor* GetParamDescriptors(u32& c) const override { c = 1; return kParams; }
    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 1; return kPins; }
    bool SetParamByName(const char* n, const char* v) override {
        if (std::strcmp(n, "asset_path") == 0) { asset_path = v; return true; }
        return false;
    }

private:
    static const MaterialParamDescriptor kParams[];
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialParamDescriptor ConstantTextureNode::kParams[] = {
    {"asset_path", "Texture", MaterialParamType::Texture, {},
     0, 0, nullptr},
};
inline const MaterialPinDescriptor ConstantTextureNode::kPins[] = {
    {"texture", 0, MaterialDataType::Texture2D, false},
};

} // namespace primal::graphics::material_graph
