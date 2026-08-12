#pragma once
#include "ComponentsCommon.h"
#include <vector>
#include <cstddef>

namespace primal::script {

// 属性类型枚举(Phase 1 内建类型 + 预留扩展)
// 10-255 reserved for future built-in types
enum class property_type : u8 {
    boolean = 0,
    int32,
    float32,
    float3,
    float4,
    quaternion,
    string,
    asset_ref,
    enum_ = 8,
    custom = 9,
};

// 属性描述符:reflect() 收集到的单个属性的元信息。
// offset 字段用于直接通过 byte offset 访问实例内存;getter/setter 可选用于
// 覆盖 offset 访问(例如计算属性、需要验证的 setter)。
//
// Invariant: when getter != nullptr, offset MUST be ignored (it is left as 0).
// Reflection consumers should check getter != nullptr before falling back to offset.
// property_with_accessor() enforces this by leaving offset at its default (0).
// property() and property_enum() always set offset; their getter/setter remain nullptr.
struct property_descriptor {
    const char* name = nullptr;
    property_type type = property_type::boolean;
    u32 offset = 0;

    // enum 类型用:可选 enum_names[count] 查表
    u32 enum_count = 0;
    const char** enum_names = nullptr;

    // 可选 getter/setter(覆盖 offset 访问)
    void (*getter)(void* instance, void* out) = nullptr;
    void (*setter)(void* instance, const void* in) = nullptr;

    // custom 类型用(Phase 1 预留)
    const char* custom_type_name = nullptr;
    u32 custom_size = 0;
};

// 属性反射器:visitor 抽象基类。
// 脚本子类重写 entity_script::reflect(property_reflector&) 时,
// 通过 r.property(...) 三次声明自己的属性。
class property_reflector {
public:
    virtual ~property_reflector() = default;
    virtual void property(const char* name, property_type type, u32 offset) = 0;
    virtual void property_enum(const char* name, u32 offset,
                               u32 count, const char** names) = 0;
    virtual void property_with_accessor(
        const char* name, property_type type,
        void(*getter)(void*, void*),
        void(*setter)(void*, const void*)) = 0;
};

// 收集型反射器:把所有 property 描述符收进 vector,供调用方遍历。
// 用于编辑器 Inspector、序列化器、C ABI 导出 — 任何需要知道脚本属性 schema 的场景。
class property_collector final : public property_reflector {
public:
    void property(const char* name, property_type type, u32 offset) override {
        property_descriptor d;
        d.name = name; d.type = type; d.offset = offset;
        descriptors_.push_back(d);
    }

    void property_enum(const char* name, u32 offset,
                       u32 count, const char** names) override {
        property_descriptor d;
        d.name = name; d.type = property_type::enum_;
        d.offset = offset; d.enum_count = count; d.enum_names = names;
        descriptors_.push_back(d);
    }

    void property_with_accessor(
        const char* name, property_type type,
        void(*getter)(void*, void*),
        void(*setter)(void*, const void*)) override {
        property_descriptor d;
        d.name = name; d.type = type;
        d.getter = getter; d.setter = setter;
        descriptors_.push_back(d);
    }

    const std::vector<property_descriptor>& descriptors() const { return descriptors_; }

private:
    std::vector<property_descriptor> descriptors_;
};

} // namespace primal::script
