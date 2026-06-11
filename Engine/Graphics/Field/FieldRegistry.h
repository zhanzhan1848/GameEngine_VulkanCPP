#pragma once

#include "CommonHeaders.h"
#include "Graphics/Field/FieldDescriptor.h"

namespace primal::graphics::field {

class FieldRegistry {
public:
    static FieldRegistry& Get();

    FieldRegistry(const FieldRegistry&) = delete;
    FieldRegistry& operator=(const FieldRegistry&) = delete;

    void Register(const FieldDescriptor& desc);
    void Unregister(FieldSemantic semantic);
    void Update(FieldSemantic semantic, const FieldDescriptor& desc);

    const FieldDescriptor* Find(FieldSemantic semantic) const;

    // 查询级联场：查找 semantic 基础的多个级联
    // 返回实际找到的级联数，写入 out 数组
    u32 FindCascaded(FieldSemantic base_semantic,
                      FieldDescriptor* out,
                      u32 max_count) const;

    void Clear();

private:
    FieldRegistry() = default;

    static constexpr u32 MAX_FIELDS = 32;

    struct Entry {
        FieldSemantic semantic{static_cast<FieldSemantic>(0)};
        FieldDescriptor desc;
        bool active{false};
    };

    Entry entries_[MAX_FIELDS];
    u32 count_{0};
};

} // namespace primal::graphics::field
