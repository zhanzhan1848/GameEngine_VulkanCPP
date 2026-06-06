#include "Graphics/Field/FieldRegistry.h"

namespace primal::graphics::field {

FieldRegistry& FieldRegistry::Get() {
    static FieldRegistry instance;
    return instance;
}

void FieldRegistry::Register(const FieldDescriptor& desc) {
    // Check if already registered — update if so
    for (u32 i = 0; i < count_; i++) {
        if (entries_[i].active && entries_[i].semantic == desc.semantic) {
            entries_[i].desc = desc;
            return;
        }
    }
    // Add new entry
    if (count_ < MAX_FIELDS) {
        entries_[count_].semantic = desc.semantic;
        entries_[count_].desc = desc;
        entries_[count_].active = true;
        count_++;
    }
}

void FieldRegistry::Unregister(FieldSemantic semantic) {
    for (u32 i = 0; i < count_; i++) {
        if (entries_[i].active && entries_[i].semantic == semantic) {
            entries_[i].active = false;
            return;
        }
    }
}

void FieldRegistry::Update(FieldSemantic semantic, const FieldDescriptor& desc) {
    for (u32 i = 0; i < count_; i++) {
        if (entries_[i].active && entries_[i].semantic == semantic) {
            entries_[i].desc = desc;
            return;
        }
    }
}

const FieldDescriptor* FieldRegistry::Find(FieldSemantic semantic) const {
    for (u32 i = 0; i < count_; i++) {
        if (entries_[i].active && entries_[i].semantic == semantic)
            return &entries_[i].desc;
    }
    return nullptr;
}

u32 FieldRegistry::FindCascaded(FieldSemantic base_semantic,
                                  FieldDescriptor* out,
                                  u32 max_count) const {
    u32 found = 0;
    for (u32 i = 0; i < count_ && found < max_count; i++) {
        if (!entries_[i].active) continue;
        const auto& desc = entries_[i].desc;
        // Match by type + semantic prefix
        // Cascade entries share the same semantic base, differ by CascadeIndex attr
        if (desc.semantic == base_semantic ||
            (static_cast<u32>(desc.semantic) == static_cast<u32>(base_semantic) &&
             desc.Has(FieldAttr::CascadeIndex))) {
            out[found++] = desc;
        }
    }
    // Sort by cascade index
    for (u32 i = 0; i < found; i++) {
        for (u32 j = i + 1; j < found; j++) {
            if (out[i].GetUInt(FieldAttr::CascadeIndex, 0) >
                out[j].GetUInt(FieldAttr::CascadeIndex, 0)) {
                FieldDescriptor tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
    return found;
}

void FieldRegistry::Clear() {
    for (u32 i = 0; i < count_; i++)
        entries_[i].active = false;
    count_ = 0;
}

} // namespace primal::graphics::field
