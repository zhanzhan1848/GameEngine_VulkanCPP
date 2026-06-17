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
        if (data) {
            assert(data->type == MaterialDataType::Float && "Pin type mismatch: expected Float");
            return static_cast<MaterialFloatData*>(data);
        }
        return nullptr;
    }
    MaterialFloat2Data* AsFloat2() const {
        if (data) {
            assert(data->type == MaterialDataType::Float2 && "Pin type mismatch: expected Float2");
            return static_cast<MaterialFloat2Data*>(data);
        }
        return nullptr;
    }
    MaterialFloat3Data* AsFloat3() const {
        if (data) {
            assert(data->type == MaterialDataType::Float3 && "Pin type mismatch: expected Float3");
            return static_cast<MaterialFloat3Data*>(data);
        }
        return nullptr;
    }
    MaterialFloat4Data* AsFloat4() const {
        if (data) {
            assert(data->type == MaterialDataType::Float4 && "Pin type mismatch: expected Float4");
            return static_cast<MaterialFloat4Data*>(data);
        }
        return nullptr;
    }
    MaterialTextureData* AsTexture() const {
        if (data) {
            assert(data->type == MaterialDataType::Texture2D && "Pin type mismatch: expected Texture2D");
            return static_cast<MaterialTextureData*>(data);
        }
        return nullptr;
    }
    MaterialBoolData* AsBool() const {
        if (data) {
            assert(data->type == MaterialDataType::Bool && "Pin type mismatch: expected Bool");
            return static_cast<MaterialBoolData*>(data);
        }
        return nullptr;
    }
};

} // namespace primal::graphics::material_graph
