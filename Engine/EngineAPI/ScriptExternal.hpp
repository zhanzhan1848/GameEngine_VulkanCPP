// === Phase 1 Task 6: register_external C++ convenience wrapper ===
//
// Thin header-only C++ wrapper around the C ABI in Components/ScriptExternal.h.
//
// Phase 1 deliberately keeps this minimal — no template metaprogramming,
// no automatic property extraction. External backends (Lua/Python/C#, mods)
// will call script_register_external / script_create_external directly.
// This wrapper exists so C++ code that wants type-erased registration can
// stay in C++ without manually declaring extern "C" prototypes.
//
// The ambitious template version (automatic property extraction via
// if constexpr / requires) is deferred to Phase 2 when there is real demand.

#pragma once

#include "Components/ScriptExternal.h"

namespace primal::script {

// Convenience: register an external script type from C++.
// Returns type_id or (u64)-1 on failure.
inline u64 register_external(
    const char* type_name,
    void* user_data,
    const script_external_callbacks* callbacks)
{
    return script_register_external(type_name, user_data, callbacks);
}

// Convenience: create an instance of a previously registered external type.
// Returns script_id (as u64) or (u64)-1 on failure.
inline u64 create_external(u64 type_id, u64 entity_id)
{
    return script_create_external(type_id, entity_id);
}

} // namespace primal::script
