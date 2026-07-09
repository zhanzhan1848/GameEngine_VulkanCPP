#pragma once

#include "Common/PrimitiveTypes.h"
#include "Components/ScriptExternal.h"  // for property_visitor_c

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace primal::lua_backend {

// Registered Lua script type. Created once at startup via register_type().
// Holds type-level Lua state shared across all instances of this type.
// Phase 2b.1: instance_table_ref moved to LuaScriptInstance.
struct LuaScriptType {
    u64 type_id;                // returned by script_register_external
    void* lua_state;            // lua_State* (opaque to header consumers)
    int script_table_ref;       // root script table (LUA_REGISTRYINDEX ref)
    std::string type_name;
};

// Per-instance Lua state. One per create_instance() call. Stored as
// unique_ptr in LuaBackend::instances_ so the pointer handed to the engine
// (as instance_user_data) is stable across map rehashing.
struct LuaScriptInstance {
    LuaScriptType* type;        // back-pointer for lua_State + root table
    int instance_table_ref;     // this instance's table (LUA_REGISTRYINDEX ref)
    u64 entity_id;
    u64 script_id;              // engine-returned id; key in instances_ map
};

class LuaBackend {
public:
    static LuaBackend& instance();

    void initialize();
    void shutdown();

    // Load a Lua file, register it as an external script type.
    // Returns type_id or u64_invalid_id on failure.
    u64 register_type(const char* type_name, const char* lua_file_path);

    // Create an instance of the registered type, attached to entity_id.
    // Returns script_id or u64_invalid_id on failure.
    u64 create_instance(u64 type_id, u64 entity_id);

    // Lookup a registered type by type_id. Returns nullptr if not found.
    LuaScriptType* find_type(u64 type_id);

    // Lookup a live instance by script_id. Returns nullptr if not found
    // (already destroyed or never created).
    LuaScriptInstance* find_instance(u64 script_id);

    // Erase an instance from the map. Called by my_destroy after Lua cleanup.
    // The LuaScriptInstance* passed to the engine becomes dangling after this.
    void erase_instance(u64 script_id);

    // Test-only: directly invoke the adapter's reflect callback for the
    // given instance.
    void invoke_reflect_for_test(LuaScriptInstance* inst, property_visitor_c visitor);

private:
    LuaBackend() = default;
    ~LuaBackend() = default;
    LuaBackend(const LuaBackend&) = delete;
    LuaBackend& operator=(const LuaBackend&) = delete;

    std::vector<LuaScriptType> types_;
    std::unordered_map<u64, std::unique_ptr<LuaScriptInstance>> instances_;
    bool initialized_ = false;
};

} // namespace primal::lua_backend
