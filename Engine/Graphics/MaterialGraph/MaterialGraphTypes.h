#pragma once

#include "CommonHeaders.h"
#include "Utilities/Math.h"
#include "Id.h"
#include <vector>
#include <memory>
#include <string>
#include <cassert>

namespace primal::graphics::material_graph {

enum class MaterialDataType : u8 {
    Float,
    Float2,
    Float3,
    Float4,
    Texture2D,
    Bool
};

struct MaterialData {
    MaterialDataType type;
    virtual ~MaterialData() = default;
protected:
    MaterialData() = default;
};

struct MaterialFloatData : MaterialData {
    f32 value{0.0f};
    MaterialFloatData() { type = MaterialDataType::Float; }
};

struct MaterialFloat2Data : MaterialData {
    math::v2 value{0.0f, 0.0f};
    MaterialFloat2Data() { type = MaterialDataType::Float2; }
};

struct MaterialFloat3Data : MaterialData {
    math::v3 value{0.0f, 0.0f, 0.0f};
    MaterialFloat3Data() { type = MaterialDataType::Float3; }
};

struct MaterialFloat4Data : MaterialData {
    math::v4 value{0.0f, 0.0f, 0.0f, 0.0f};
    MaterialFloat4Data() { type = MaterialDataType::Float4; }
};

struct MaterialTextureData : MaterialData {
    id::id_type texture_id{id::invalid_id};
    std::string asset_path;
    MaterialTextureData() { type = MaterialDataType::Texture2D; }
};

struct MaterialBoolData : MaterialData {
    bool value{false};
    MaterialBoolData() { type = MaterialDataType::Bool; }
};

struct MaterialPin {
    MaterialDataType expected_type{MaterialDataType::Float};
    MaterialData* data{nullptr};
    u32 index{0};

    MaterialFloatData* AsFloat() const {
        return data && data->type == MaterialDataType::Float
            ? static_cast<MaterialFloatData*>(data) : nullptr;
    }
    MaterialFloat2Data* AsFloat2() const {
        return data && data->type == MaterialDataType::Float2
            ? static_cast<MaterialFloat2Data*>(data) : nullptr;
    }
    MaterialFloat3Data* AsFloat3() const {
        return data && data->type == MaterialDataType::Float3
            ? static_cast<MaterialFloat3Data*>(data) : nullptr;
    }
    MaterialFloat4Data* AsFloat4() const {
        return data && data->type == MaterialDataType::Float4
            ? static_cast<MaterialFloat4Data*>(data) : nullptr;
    }
    MaterialTextureData* AsTexture() const {
        return data && data->type == MaterialDataType::Texture2D
            ? static_cast<MaterialTextureData*>(data) : nullptr;
    }
    MaterialBoolData* AsBool() const {
        return data && data->type == MaterialDataType::Bool
            ? static_cast<MaterialBoolData*>(data) : nullptr;
    }
};

} // namespace primal::graphics::material_graph
