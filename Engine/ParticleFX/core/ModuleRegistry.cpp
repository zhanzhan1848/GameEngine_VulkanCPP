// ModuleRegistry.cpp — 见 ModuleRegistry.h。

#include "ModuleRegistry.h"

#ifndef DISABLE_PARTICLE_FX

#include <cstring>

namespace primal::pfx {

module_registry& module_registry::get() {
    static module_registry instance;
    return instance;
}

void module_registry::register_module(module_def def) {
    def.name_hash = hash_name(def.name);
    for (auto& existing : defs_) {
        if (existing.name_hash == def.name_hash) {
            existing = def;   // 覆盖（测试可重入）
            return;
        }
    }
    defs_.push_back(std::move(def));
}

const module_def* module_registry::find(u32 name_hash) const {
    for (const auto& def : defs_) {
        if (def.name_hash == name_hash) return &def;
    }
    return nullptr;
}

const std::vector<module_def>& module_registry::all() const { return defs_; }

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
