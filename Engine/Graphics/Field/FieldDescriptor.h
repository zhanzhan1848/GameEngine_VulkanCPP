#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::field {

// ---- 枚举定义 ----

enum class FieldType : u32 {
    SDF,
    Density,
    Irradiance,
    Velocity,
    Temperature,
};

// 语义标识 — 替代粗粒度 FieldType 做查询
// 多个同类型场（GlobalSDF / TerrainSDF）需要不同语义
enum class FieldSemantic : u32 {
    GlobalSDF,
    TerrainSDF,
    CharacterSDF,
    LocalSDF,
    GlobalDensity,
    CloudDensity,
    DDGIIrradiance,
    DDGIDepth,
    FroxelDensity,
    FroxelLighting,
    ParticleVelocity,
    CADGeometry,
};

// 动态属性键 — 新场类型只需扩展此枚举，不改 FieldDescriptor
enum class FieldAttr : u16 {
    // Grid
    VoxelSize,
    Resolution,
    MipLevels,
    // Cascade
    CascadeCount,
    CascadeIndex,
    CascadeScale,
    // Density
    DensityThreshold,
    DensityFadeRange,
    NoiseOctaves,
    NoiseFrequency,
    // Irradiance
    SHOrder,
    ProbeSpacing,
    ProbeCountX,
    ProbeCountY,
    ProbeCountZ,
    // Velocity
    TimeStep,
};

// 资源槽 — 命名式，不同场类型使用不同槽位组合
enum class FieldResourceSlot : u8 {
    Primary,
    Secondary,
    Noise,
    History,
    Auxiliary0,
    Auxiliary1,
    Auxiliary2,
    Auxiliary3,
};

// ---- 属性存储（8 bytes per slot）----

struct AttrSlot {
    FieldAttr id;   // 2 bytes
    u16 _pad{0};
    u32 value{0};   // f32 和 u32 共用，由 FieldAttr schema 隐式决定类型
};
static_assert(sizeof(AttrSlot) == 8, "AttrSlot must be 8 bytes");

// ---- 资源存储 ----

struct ResourceSlot {
    FieldResourceSlot slot{FieldResourceSlot::Primary};
    rhi::ResourceHandle resource{rhi::handles::INVALID_RESOURCE};
};

// ---- FieldDescriptor ----

struct FieldDescriptor {
    // Identity
    FieldType type{FieldType::SDF};
    FieldSemantic semantic{FieldSemantic::GlobalSDF};
    math::v3 origin{};
    math::v3 extent{};
    bool is_valid{false};

    // Dynamic Attributes
    static constexpr u32 MAX_ATTRS = 12;
    AttrSlot attrs[MAX_ATTRS];
    u32 attr_count{0};

    // Dynamic Resources
    static constexpr u32 MAX_RESOURCES = 8;
    ResourceSlot resources[MAX_RESOURCES];
    u32 resource_count{0};

    // ---- Attribute API ----

    void Set(FieldAttr attr, f32 value) {
        u32 raw;
        memcpy(&raw, &value, sizeof(f32));
        SetRaw(attr, raw);
    }

    void Set(FieldAttr attr, u32 value) {
        SetRaw(attr, value);
    }

    bool Has(FieldAttr attr) const {
        for (u32 i = 0; i < attr_count; i++)
            if (attrs[i].id == attr) return true;
        return false;
    }

    f32 GetFloat(FieldAttr attr, f32 fallback = 0.f) const {
        for (u32 i = 0; i < attr_count; i++) {
            if (attrs[i].id == attr) {
                f32 result;
                memcpy(&result, &attrs[i].value, sizeof(f32));
                return result;
            }
        }
        return fallback;
    }

    u32 GetUInt(FieldAttr attr, u32 fallback = 0) const {
        for (u32 i = 0; i < attr_count; i++) {
            if (attrs[i].id == attr) return attrs[i].value;
        }
        return fallback;
    }

    // ---- Resource API ----

    void SetResource(FieldResourceSlot slot, rhi::ResourceHandle handle) {
        for (u32 i = 0; i < resource_count; i++) {
            if (resources[i].slot == slot) {
                resources[i].resource = handle;
                return;
            }
        }
        if (resource_count < MAX_RESOURCES) {
            resources[resource_count].slot = slot;
            resources[resource_count].resource = handle;
            resource_count++;
        }
    }

    rhi::ResourceHandle GetResource(FieldResourceSlot slot,
                                     rhi::ResourceHandle fallback = rhi::handles::INVALID_RESOURCE) const {
        for (u32 i = 0; i < resource_count; i++)
            if (resources[i].slot == slot) return resources[i].resource;
        return fallback;
    }

private:
    void SetRaw(FieldAttr attr, u32 value) {
        for (u32 i = 0; i < attr_count; i++) {
            if (attrs[i].id == attr) {
                attrs[i].value = value;
                return;
            }
        }
        if (attr_count < MAX_ATTRS) {
            attrs[attr_count].id = attr;
            attrs[attr_count]._pad = 0;
            attrs[attr_count].value = value;
            attr_count++;
        }
    }
};

} // namespace primal::graphics::field
