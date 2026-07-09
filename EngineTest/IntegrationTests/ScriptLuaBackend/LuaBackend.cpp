#include "LuaBackend.h"
#include "Components/ScriptExternal.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace primal::lua_backend {

// === Adapter callbacks (forward decls; definitions below) ===
static void my_begin_play(void* user_data);
static void my_update(void* user_data, float dt);
static void my_destroy(void* user_data);
static void my_reflect(void* user_data, property_visitor_c visitor);

LuaBackend& LuaBackend::instance() {
    static LuaBackend inst;
    return inst;
}

void LuaBackend::initialize() {
    initialized_ = true;
}

void LuaBackend::shutdown() {
    for (auto& t : types_) {
        if (t.lua_state) {
            lua_State* L = (lua_State*)t.lua_state;
            lua_close(L);
            t.lua_state = nullptr;
        }
    }
    types_.clear();
    // instances_ cleared by unique_ptr destructors. Lua tables already
    // unref'd in my_destroy; clearing the map drops the LuaScriptInstance
    // structs themselves.
    instances_.clear();
    initialized_ = false;
}

u64 LuaBackend::register_type(const char* type_name, const char* lua_file_path) {
    if (!initialized_) {
        std::fprintf(stderr, "LuaBackend::register_type called before initialize()\n");
        return u64_invalid_id;
    }

    // 1. Create Lua state + open libs.
    lua_State* L = luaL_newstate();
    if (!L) {
        std::fprintf(stderr, "luaL_newstate failed for %s\n", type_name);
        return u64_invalid_id;
    }
    luaL_openlibs(L);

    // 2. Load + execute the Lua file. The file should return a table.
    int rc = luaL_dofile(L, lua_file_path);
    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "Lua load error in %s: %s\n", lua_file_path,
                     err ? err : "(unknown)");
        lua_close(L);
        return u64_invalid_id;
    }

    // 3. The file's return value (table) is on top of stack. Verify it's a table.
    if (lua_type(L, -1) != LUA_TTABLE) {
        std::fprintf(stderr, "Lua script %s did not return a table\n", lua_file_path);
        lua_pop(L, 1);
        lua_close(L);
        return u64_invalid_id;
    }

    // 4. Store the table in the registry, get a ref handle.
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops the table

    // 5. Build LuaScriptType entry. Push to types_ BEFORE registering so we
    //    can hand the engine a stable pointer to the slot. types_ does not
    //    grow after startup registration (spec §9.1), so &types_.back() is
    //    safe for the engine's lifetime of this type_id.
    LuaScriptType type{};
    type.type_name = type_name;
    type.lua_state = L;
    type.script_table_ref = ref;
    type.type_id = u64_invalid_id;  // filled below

    types_.push_back(type);
    LuaScriptType* raw_type = &types_.back();

    // 6. Register with engine via Phase 1 C ABI. Type-level user_data is the
    //    LuaScriptType* (preserved for future type-level queries; not used
    //    by current adapters because instance_user_data is always non-NULL
    //    for Lua instances — effective_user_data() never falls back).
    script_external_callbacks cbs{};
    cbs.begin_play = &my_begin_play;
    cbs.update     = &my_update;
    cbs.destroy    = &my_destroy;
    cbs.reflect    = &my_reflect;
    // fixed_update / late_update / on_reload left NULL for MVP (Phase 2b.3).

    u64 type_id = script_register_external(type_name,
                                           /*user_data=*/(void*)raw_type, &cbs);
    if (type_id == u64_invalid_id) {
        std::fprintf(stderr, "script_register_external failed for %s\n", type_name);
        types_.pop_back();  // roll back the push
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua_close(L);
        return u64_invalid_id;
    }

    raw_type->type_id = type_id;
    return type_id;
}

u64 LuaBackend::create_instance(u64 type_id, u64 entity_id) {
    LuaScriptType* type = find_type(type_id);
    if (!type) {
        std::fprintf(stderr, "create_instance: type_id %llu not found\n",
                     (unsigned long long)type_id);
        return u64_invalid_id;
    }

    lua_State* L = (lua_State*)type->lua_state;
    if (!L) {
        std::fprintf(stderr, "create_instance: lua_state null for type %llu\n",
                     (unsigned long long)type_id);
        return u64_invalid_id;
    }

    // === Phase 2b.1: per-instance table ===
    // Shallow-copy the root script table into a new instance table. The
    // instance table holds this instance's fields (e.g. update_count) so
    // multiple instances of the same type don't share state.
    //
    // Stack: (empty)
    lua_rawgeti(L, LUA_REGISTRYINDEX, type->script_table_ref);
    // Stack: root_table
    if (!lua_istable(L, -1)) {
        std::fprintf(stderr, "create_instance: root script table missing\n");
        lua_pop(L, 1);
        return u64_invalid_id;
    }

    lua_newtable(L);  // instance table
    // Stack: root(-2) instance(-1)
    int root_idx = lua_absindex(L, -2);

    lua_pushnil(L);   // first key
    while (lua_next(L, root_idx) != 0) {
        // Stack: root instance key(-2) value(-1)
        lua_pushvalue(L, -2);  // copy key
        lua_pushvalue(L, -2);  // copy value
        // Stack: root instance key value key_copy value_copy
        lua_settable(L, -5);   // instance[key_copy] = value_copy (pops top 2)
        // Stack: root instance key value
        lua_pop(L, 1);         // drop value (lua_next wants key on top)
        // Stack: root instance key
    }
    // Stack: root instance
    lua_remove(L, -2);  // drop root, leave instance
    // Stack: instance(-1)

    int inst_ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops instance table
    // Stack: (empty)

    // === Allocate LuaScriptInstance (heap, pointer-stable) ===
    auto inst = std::make_unique<LuaScriptInstance>();
    inst->type = type;
    inst->instance_table_ref = inst_ref;
    inst->entity_id = entity_id;
    inst->script_id = u64_invalid_id;  // filled after engine returns it

    LuaScriptInstance* raw = inst.get();

    // === Register with engine. Pass raw as instance_user_data so every
    // callback dispatch receives the per-instance pointer. ===
    u64 script_id = script_create_external(type_id, entity_id, /*instance_user_data=*/(void*)raw);
    if (script_id == u64_invalid_id) {
        std::fprintf(stderr, "create_instance: script_create_external failed\n");
        luaL_unref(L, LUA_REGISTRYINDEX, inst_ref);
        return u64_invalid_id;
    }

    inst->script_id = script_id;
    instances_[script_id] = std::move(inst);
    return script_id;
}

LuaScriptType* LuaBackend::find_type(u64 type_id) {
    for (auto& t : types_) {
        if (t.type_id == type_id) return &t;
    }
    return nullptr;
}

LuaScriptInstance* LuaBackend::find_instance(u64 script_id) {
    auto it = instances_.find(script_id);
    return it == instances_.end() ? nullptr : it->second.get();
}

void LuaBackend::erase_instance(u64 script_id) {
    instances_.erase(script_id);
}

void LuaBackend::invoke_reflect_for_test(LuaScriptInstance* inst,
                                         property_visitor_c visitor) {
    my_reflect(inst, visitor);
}

// === Adapter callbacks ===

// Invoke a no-arg Lua hook on the instance table: function self:hook_name().
// Silently skips missing hooks (Phase 2a §6.3). Stack-safe on every return path.
static void invoke_lua_hook(LuaScriptInstance* inst, const char* hook_name) {
    if (!inst || !inst->type || !inst->type->lua_state) return;
    lua_State* L = (lua_State*)inst->type->lua_state;

    // Stack: (empty)
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->instance_table_ref);
    // Stack: instance_table
    if (!lua_istable(L, -1)) {
        std::fprintf(stderr, "Lua %s: instance table missing\n", hook_name);
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, hook_name);
    // Stack: instance_table hook_fn
    if (!lua_isfunction(L, -1)) {
        // Hook not defined — silently skip.
        lua_pop(L, 2);
        return;
    }

    lua_pushvalue(L, -2);  // self (instance table)
    // Stack: instance_table hook_fn self

    int rc = lua_pcall(L, 1, 0, 0);
    // Stack: instance_table [err?]
    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "Lua error in %s: %s\n", hook_name,
                     err ? err : "(unknown)");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);  // instance_table
    // Stack: (empty)
}

static void my_begin_play(void* user_data) {
    auto* inst = static_cast<LuaScriptInstance*>(user_data);
    invoke_lua_hook(inst, "begin_play");
}

static void my_update(void* user_data, float dt) {
    auto* inst = static_cast<LuaScriptInstance*>(user_data);
    if (!inst || !inst->type || !inst->type->lua_state) return;
    lua_State* L = (lua_State*)inst->type->lua_state;

    // Stack: (empty)
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->instance_table_ref);
    // Stack: instance_table
    if (!lua_istable(L, -1)) {
        std::fprintf(stderr, "Lua update: instance table missing\n");
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, "update");
    // Stack: instance_table update_fn
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }

    lua_pushvalue(L, -2);  // self
    lua_pushnumber(L, dt);
    // Stack: instance_table update_fn self dt

    int rc = lua_pcall(L, 2, 0, 0);
    // Stack: instance_table [err?]
    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "Lua error in update: %s\n", err ? err : "(unknown)");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);  // instance_table
    // Stack: (empty)
}

static void my_destroy(void* user_data) {
    auto* inst = static_cast<LuaScriptInstance*>(user_data);
    if (!inst) return;

    // 1. Call Lua destroy hook first (script may want to read its fields
    //    before we release the table ref).
    invoke_lua_hook(inst, "destroy");

    // 2. Release the instance table registry ref.
    if (inst->type && inst->type->lua_state &&
        inst->instance_table_ref != LUA_NOREF) {
        lua_State* L = (lua_State*)inst->type->lua_state;
        luaL_unref(L, LUA_REGISTRYINDEX, inst->instance_table_ref);
        inst->instance_table_ref = LUA_NOREF;
    }

    // 3. Drop the LuaScriptInstance from the map. The `inst` pointer is now
    //    dangling — do not touch it after this call.
    LuaBackend::instance().erase_instance(inst->script_id);
}

// Lua-side visitor: visitor:property(name, type_str).
// Looks up the C visitor stashed in the Lua table, maps type_str to a
// SCRIPT_PROPERTY_TYPE_* constant, and forwards to cv->property().
static int lua_visitor_property(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);  // self (visitor table)
    const char* name = luaL_checkstring(L, 2);
    const char* type_str = luaL_checkstring(L, 3);

    lua_getfield(L, 1, "_c_visitor");
    property_visitor_c* cv = (property_visitor_c*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!cv || !cv->property) return 0;

    int ptype;
    if      (std::strcmp(type_str, "boolean") == 0)    ptype = SCRIPT_PROPERTY_TYPE_BOOLEAN;
    else if (std::strcmp(type_str, "int32") == 0)      ptype = SCRIPT_PROPERTY_TYPE_INT32;
    else if (std::strcmp(type_str, "float32") == 0)    ptype = SCRIPT_PROPERTY_TYPE_FLOAT32;
    else if (std::strcmp(type_str, "float3") == 0)     ptype = SCRIPT_PROPERTY_TYPE_FLOAT3;
    else if (std::strcmp(type_str, "float4") == 0)     ptype = SCRIPT_PROPERTY_TYPE_FLOAT4;
    else if (std::strcmp(type_str, "quaternion") == 0) ptype = SCRIPT_PROPERTY_TYPE_QUATERNION;
    else if (std::strcmp(type_str, "string") == 0)     ptype = SCRIPT_PROPERTY_TYPE_STRING;
    else if (std::strcmp(type_str, "asset_ref") == 0)  ptype = SCRIPT_PROPERTY_TYPE_ASSET_REF;
    else if (std::strcmp(type_str, "enum") == 0)       ptype = SCRIPT_PROPERTY_TYPE_ENUM;
    else                                                ptype = SCRIPT_PROPERTY_TYPE_CUSTOM;

    cv->property(cv->state, name, ptype, /*offset=*/0);
    return 0;
}

// Build the Lua visitor table and dispatch reflect(instance, visitor).
static void my_reflect(void* user_data, property_visitor_c visitor) {
    auto* inst = static_cast<LuaScriptInstance*>(user_data);
    if (!inst || !inst->type || !inst->type->lua_state) return;
    lua_State* L = (lua_State*)inst->type->lua_state;

    // Build visitor table.
    lua_newtable(L);
    int visitor_idx = lua_absindex(L, -1);

    lua_pushlightuserdata(L, &visitor);
    lua_setfield(L, visitor_idx, "_c_visitor");

    lua_pushcfunction(L, &lua_visitor_property);
    lua_setfield(L, visitor_idx, "property");

    // Fetch instance table.
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->instance_table_ref);
    if (!lua_istable(L, -1)) {
        std::fprintf(stderr, "Lua reflect: instance table missing\n");
        lua_pop(L, 2);
        return;
    }
    int instance_idx = lua_absindex(L, -1);

    lua_getfield(L, instance_idx, "reflect");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        return;
    }

    lua_pushvalue(L, instance_idx);
    lua_pushvalue(L, visitor_idx);
    int rc = lua_pcall(L, 2, 0, 0);
    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "Lua error in reflect: %s\n", err ? err : "(unknown)");
        lua_pop(L, 1);
    }

    lua_pop(L, 2);  // visitor + instance
}

} // namespace primal::lua_backend
