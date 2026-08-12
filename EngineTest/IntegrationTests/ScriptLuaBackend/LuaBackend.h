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
    std::string file_path;      // === Phase 2b.3 === path for reload_type re-read
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

// Per-subscription record (Phase 2b.2). One per bus.on() call.
// user_data passed to script_event_subscribe points here.
struct LuaSubRecord {
    int lua_fn_ref;              // LUA_REGISTRYINDEX ref to handler function
    void* lua_state;             // lua_State* — type->lua_state, cached for cleanup
    u64 sub_id;                  // engine-returned subscription id (map key)
};

// === Phase 2b.3 ===
// Captured pre-destroy state. Ownership transferred from instance to this
// struct in capture_state_for_reload. Freed by delete_captured_state after
// on_reload consumes it.
struct LuaReloadState {
    void* lua_state;            // lua_State* (back-pointer for luaL_unref on delete)
    int captured_table_ref;     // old instance_table_ref, owned by this struct
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

    // === Phase 2b.3 ===
    // Re-read the .lua file, replace script_table_ref in LuaScriptType.
    // Existing instances keep old behavior until each is reloaded via
    // script::reload(eid). Returns type_id on success, u64_invalid_id on failure.
    u64 reload_type(u64 type_id);

    // Erase an instance from the map. Called by my_destroy after Lua cleanup.
    // The LuaScriptInstance* passed to the engine becomes dangling after this.
    void erase_instance(u64 script_id);

    // === Phase 2b.3 ===
    // Called by lua_recreate_instance_user_data_for_reload after my_destroy
    // erased the old map entry. Allocates a fresh LuaScriptInstance and
    // re-inserts under the same script_id (engine reuses script_id on reload).
    // Returns raw pointer to the new instance (ownership stays in instances_).
    LuaScriptInstance* recreate_instance_for_reload(
        LuaScriptType* type, u64 entity_id, u64 script_id);

    // Test-only: directly invoke the adapter's reflect callback for the
    // given instance.
    void invoke_reflect_for_test(LuaScriptInstance* inst, property_visitor_c visitor);

    // === Phase 2b.2: Event Bus ===
    // Per-instance subscription tracking for cleanup on my_destroy.
    std::unordered_map<LuaScriptInstance*, std::vector<LuaSubRecord*>> inst_subs_;
    std::unordered_map<u64, LuaSubRecord*> sub_to_rec_;

    // Per-call "current instance" context — set by adapter hooks so bus.on
    // (a Lua global function) knows which instance is subscribing.
    // Single-threaded engine → safe under the Meyers singleton.
    LuaScriptInstance* current_instance_ = nullptr;

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
