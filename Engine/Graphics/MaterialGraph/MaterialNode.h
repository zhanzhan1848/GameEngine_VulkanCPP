#pragma once

#include "Graphics/MaterialGraph/MaterialGraphTypes.h"
#include "Graphics/MaterialGraph/MaterialGraphReflection.h"
#include <vector>
#include <memory>
#include <cstring>

namespace primal::graphics::material_graph {

struct NodeTypeInfo {
    const char* type_name;       // e.g. "ConstantFloat4"
    const char* display_name;    // e.g. "Constant Float4"
    const char* category;        // e.g. "Constants"
    bool        is_terminal;     // true only for MaterialOutput
};

class MaterialNode {
public:
    virtual ~MaterialNode() = default;
    virtual const char* TypeName() const = 0;
    virtual void Execute() = 0;
    virtual NodeTypeInfo GetTypeInfo() const = 0;

    std::vector<MaterialPin> inputs;
    std::vector<MaterialPin> outputs;

    virtual const MaterialParamDescriptor* GetParamDescriptors(u32& out_count) const {
        out_count = 0; return nullptr;
    }
    virtual const MaterialPinDescriptor* GetPinDescriptors(u32& out_count) const {
        out_count = 0; return nullptr;
    }
    virtual bool SetParamByName(const char* name, f32 value) {
        (void)name; (void)value; return false;
    }
    virtual bool SetParamByName(const char* name, math::v3 value) {
        (void)name; (void)value; return false;
    }
    virtual bool SetParamByName(const char* name, math::v4 value) {
        (void)name; (void)value; return false;
    }
    virtual bool SetParamByName(const char* name, const char* value) {
        (void)name; (void)value; return false;
    }

protected:
    std::vector<std::unique_ptr<MaterialData>> owned_data_;

    template<typename T, typename... Args>
    T* CreateOutput(u32 pin_index, Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = ptr.get();
        owned_data_.push_back(std::move(ptr));
        if (pin_index < outputs.size()) {
            outputs[pin_index].data = raw;
        }
        return raw;
    }
};

} // namespace primal::graphics::material_graph
