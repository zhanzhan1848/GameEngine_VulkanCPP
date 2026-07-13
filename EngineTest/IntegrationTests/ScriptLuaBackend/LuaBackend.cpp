#include "LuaBackend.h"
#include "Components/Script.h"
#include "Components/ScriptExternal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace primal::lua_backend {

// === Adapter callbacks (forward decls; definitions below) ===
static void my_begin_play(void* user_data);
static void my_update(void* user_data, float dt);
static void my_fixed_update(void* user_data, float dt);
static void my_late_update(void* user_data, float dt);
static void my_destroy(void* user_data);
static void my_reflect(void* user_data, property_visitor_c visitor);
// === Phase 2b.3: reload hooks ===
static void my_on_reload(void* user_data, void* old_state);
static void* lua_capture_state_for_reload(void* user_data);
static void lua_delete_captured_state(void* state);
static void* lua_recreate_instance_user_data_for_reload(
    void* type_user_data, u64 entity_id, u64 script_id);

// Phase 2b.1/2b.3: instance table factory (used by create_instance + my_begin_play).
static int make_instance_table(LuaScriptType* type);

// Phase 2b.2 forward decls (bus trampolines + Lua C functions; defined below).
static void lua_event_trampoline(void* user_data, const void* payload, u64 size);
static void lua_payload_deleter(void* raw);
static int lua_bus_on(lua_State* L);
static int lua_bus_off(lua_State* L);
static int lua_bus_emit(lua_State* L);
static void register_bus_table(lua_State* L);
static void release_sub_record(LuaSubRecord* rec);

LuaBackend& LuaBackend::instance() {
    static LuaBackend inst;
    return inst;
}

void LuaBackend::initialize() {
    initialized_ = true;
}

void LuaBackend::shutdown() {
    if (!initialized_) return;
    initialized_ = false;

    // Phase 2b.2: clear bus state. Any remaining subscriptions are leaks
    // (my_destroy should have cleaned them; this is a safety net for tests
    // that don't remove_for_entity before shutdown).
    for (auto& kv : inst_subs_) {
        for (auto* rec : kv.second) {
            release_sub_record(rec);
        }
    }
    inst_subs_.clear();
    sub_to_rec_.clear();
    current_instance_ = nullptr;

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
    type.file_path = lua_file_path;  // === Phase 2b.3 === for reload_type
    type.lua_state = L;
    type.script_table_ref = ref;
    type.type_id = u64_invalid_id;  // filled below

    types_.push_back(type);
    LuaScriptType* raw_type = &types_.back();

    // Phase 2b.2: expose the `bus` global on this type's lua_State before
    // any script hook (begin_play) can fire. register_bus_table must run
    // before script_register_external, which may dispatch begin_play.
    register_bus_table(L);

    // 6. Register with engine via Phase 1 C ABI. Type-level user_data is the
    //    LuaScriptType* (preserved for future type-level queries; not used
    //    by current adapters because instance_user_data is always non-NULL
    //    for Lua instances — effective_user_data() never falls back).
    script_external_callbacks cbs{};
    cbs.begin_play    = &my_begin_play;
    cbs.update        = &my_update;
    cbs.fixed_update  = &my_fixed_update;
    cbs.late_update   = &my_late_update;
    cbs.destroy       = &my_destroy;
    cbs.on_reload     = &my_on_reload;
    cbs.reflect       = &my_reflect;
    // === Phase 2b.3 ===
    cbs.capture_state_for_reload               = &lua_capture_state_for_reload;
    cbs.delete_captured_state                  = &lua_delete_captured_state;
    cbs.recreate_instance_user_data_for_reload = &lua_recreate_instance_user_data_for_reload;

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
    int inst_ref = make_instance_table(type);
    if (inst_ref == LUA_NOREF) {
        std::fprintf(stderr, "create_instance: failed to make instance table\n");
        return u64_invalid_id;
    }

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

// === Phase 2b.3: reload_type ===
// Re-read the .lua source file. The script_table_ref in LuaScriptType is
// replaced; existing instances keep their cached instance tables (shallow
// copies of the old script — they are independent). New create_instance
// calls use the new script_table_ref. Decoupled from per-entity
// script::reload(eid) — caller triggers that separately for each entity
// they want migrated.
u64 LuaBackend::reload_type(u64 type_id) {
    LuaScriptType* type = find_type(type_id);
    if (!type) {
        std::fprintf(stderr, "reload_type: type_id %llu not found\n",
                     (unsigned long long)type_id);
        return u64_invalid_id;
    }
    lua_State* L = (lua_State*)type->lua_state;
    if (!L) {
        std::fprintf(stderr, "reload_type: null lua_State\n");
        return u64_invalid_id;
    }
    if (type->file_path.empty()) {
        std::fprintf(stderr, "reload_type: empty file_path\n");
        return u64_invalid_id;
    }

    // Re-run luaL_dofile. File may have changed on disk since register_type.
    int rc = luaL_dofile(L, type->file_path.c_str());
    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "reload_type: luaL_dofile failed: %s\n",
                     err ? err : "(unknown)");
        lua_pop(L, 1);
        return u64_invalid_id;
    }

    // Stack: returned script table (or other value if file didn't return one)
    if (!lua_istable(L, -1)) {
        std::fprintf(stderr, "reload_type: script did not return a table\n");
        lua_pop(L, 1);
        return u64_invalid_id;
    }

    // Establish new ref BEFORE unreffing old — luaL_ref may reuse freed ref
    // IDs, so unref-then-ref could leak the old table.
    int new_ref = luaL_ref(L, LUA_REGISTRYINDEX);  // pops table
    luaL_unref(L, LUA_REGISTRYINDEX, type->script_table_ref);
    type->script_table_ref = new_ref;

    return type_id;
}

void LuaBackend::erase_instance(u64 script_id) {
    instances_.erase(script_id);
}

LuaScriptInstance* LuaBackend::recreate_instance_for_reload(
    LuaScriptType* type, u64 entity_id, u64 script_id
) {
    if (!type || script_id == u64_invalid_id) return nullptr;

    // Defensive idempotent erase — no-op if my_destroy already erased.
    // Guards against double-insertion if a future engine change reorders.
    instances_.erase(script_id);

    auto fresh = std::make_unique<LuaScriptInstance>();
    fresh->type = type;
    fresh->instance_table_ref = LUA_NOREF;  // begin_play will fill this
    fresh->entity_id = entity_id;
    fresh->script_id = script_id;

    auto* raw = fresh.get();
    instances_[script_id] = std::move(fresh);
    return raw;
}

void LuaBackend::invoke_reflect_for_test(LuaScriptInstance* inst,
                                         property_visitor_c visitor) {
    my_reflect(inst, visitor);
}

// === Phase 2b.2: Event Bus trampolines + Lua C functions ===

// Payload format for Lua-emitted events. 16 bytes; opaque to engine.
//
// Single-state contract: `lua_state` must be the SAME lua_State* that owns
// both the emitter's table_ref AND the subscriber's lua_fn_ref. Phase 2b.2
// only exercises same-type subscriptions (each type has its own L), so the
// trampoline early-returns on state mismatch instead of attempting a
// cross-state registry lookup (which would silently corrupt one of the L's).
struct LuaEventPayload {
    void* lua_state;       // lua_State* — which L owns the table ref
    int table_ref;         // LUA_REGISTRYINDEX ref to event table
    int _pad;              // explicit alignment (avoid compiler-dependent padding)
};
static_assert(sizeof(LuaEventPayload) == 16,
              "LuaEventPayload must stay 16 bytes — engine payload_size contract");

// Engine deleter for Lua-emitted events. Called after drain dispatch.
// Releases the Lua table ref + frees the payload buffer.
static void lua_payload_deleter(void* raw) {
    auto* p = static_cast<LuaEventPayload*>(raw);
    if (p->lua_state && p->table_ref != LUA_NOREF) {
        luaL_unref((lua_State*)p->lua_state, LUA_REGISTRYINDEX, p->table_ref);
    }
    delete p;
}

// Trampoline: called by engine when a string-keyed event fires for a
// subscription whose user_data is LuaSubRecord*.
static void lua_event_trampoline(void* user_data, const void* payload, u64 size) {
    auto* rec = static_cast<LuaSubRecord*>(user_data);
    if (!rec || !rec->lua_state || size != sizeof(LuaEventPayload)) return;

    auto* p = static_cast<const LuaEventPayload*>(payload);
    // Same-state contract: see LuaEventPayload comment above.
    if (p->lua_state != rec->lua_state) {
        std::fprintf(stderr,
            "lua_event_trampoline: cross-state event dropped (sub L=%p emit L=%p)\n",
            rec->lua_state, p->lua_state);
        return;
    }
    lua_State* L = (lua_State*)rec->lua_state;

    // Push handler function
    lua_rawgeti(L, LUA_REGISTRYINDEX, rec->lua_fn_ref);
    if (!lua_isfunction(L, -1)) {
        std::fprintf(stderr, "lua_event_trampoline: fn ref not a function\n");
        lua_pop(L, 1);
        return;
    }

    // Push event table (dereference table_ref)
    lua_rawgeti(L, LUA_REGISTRYINDEX, p->table_ref);
    if (!lua_istable(L, -1)) {
        std::fprintf(stderr, "lua_event_trampoline: table ref not a table\n");
        lua_pop(L, 2);
        return;
    }

    // pcall(handler, table)
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "Lua event handler error: %s\n", err ? err : "(unknown)");
        lua_pop(L, 1);
    }
}

// Release a LuaSubRecord: unsubscribe from engine, unref the Lua handler
// using the L that OWNS the ref (not the caller's L — Lua refs are per-state),
// erase from sub_to_rec_, and free the record. Does NOT touch inst_subs_;
// caller handles that (lua_bus_off removes from the vector, my_destroy /
// shutdown clear the whole map entry).
//
// Engine unsubscribe is skipped when script::is_initialized() is false —
// happens if LuaBackend::shutdown() runs after script::shutdown() (engine
// bus already destroyed; the subscription id is meaningless). Lua-side
// cleanup MUST still run to avoid leaking the luaL_ref.
static void release_sub_record(LuaSubRecord* rec) {
    if (!rec) return;
    if (primal::script::is_initialized()) {
        script_event_unsubscribe(rec->sub_id);
    }
    if (rec->lua_state) {
        luaL_unref((lua_State*)rec->lua_state, LUA_REGISTRYINDEX, rec->lua_fn_ref);
    }
    LuaBackend::instance().sub_to_rec_.erase(rec->sub_id);
    delete rec;
}

// bus.on(name, fn) → lightuserdata(sub_id)
static int lua_bus_on(lua_State* L) {
    auto& backend = LuaBackend::instance();
    if (!backend.current_instance_) {
        return luaL_error(L, "bus.on called outside script hook");
    }
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // Ref the Lua function
    lua_pushvalue(L, 2);
    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    auto* rec = new LuaSubRecord{ fn_ref, (void*)L, 0 };
    u64 sub_id = script_event_subscribe(name, (void*)rec, &lua_event_trampoline);
    if (sub_id == 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, fn_ref);
        delete rec;
        return luaL_error(L, "script_event_subscribe failed");
    }
    rec->sub_id = sub_id;
    backend.inst_subs_[backend.current_instance_].push_back(rec);
    backend.sub_to_rec_[sub_id] = rec;

    lua_pushlightuserdata(L, (void*)sub_id);
    return 1;
}

// bus.off(sub_id) → nil
static int lua_bus_off(lua_State* L) {
    if (!lua_islightuserdata(L, 1)) {
        return luaL_error(L, "bus.off expects lightuserdata (sub_id)");
    }
    u64 sub_id = (u64)lua_touserdata(L, 1);
    if (sub_id == 0) return 0;

    auto& backend = LuaBackend::instance();
    auto it = backend.sub_to_rec_.find(sub_id);
    if (it == backend.sub_to_rec_.end()) {
        // Unknown id (e.g., already cleaned via my_destroy). Engine-side
        // unsubscribe is also a no-op for unknown ids.
        script_event_unsubscribe(sub_id);
        return 0;
    }
    LuaSubRecord* rec = it->second;
    // Remove from inst_subs_ vector before releasing (release_sub_record
    // frees rec, so we can't deref it afterward).
    for (auto& kv : backend.inst_subs_) {
        auto& vec = kv.second;
        for (size_t i = 0; i < vec.size(); ++i) {
            if (vec[i] == rec) {
                vec.erase(vec.begin() + i);
                break;
            }
        }
    }
    release_sub_record(rec);
    return 0;
}

// bus.emit(name, table) → nil
static int lua_bus_emit(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    // Ref the Lua table
    lua_pushvalue(L, 2);
    int table_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    auto* payload = new LuaEventPayload{ (void*)L, table_ref, 0 };
    script_event_emit(name, (void*)payload, sizeof(LuaEventPayload), &lua_payload_deleter);
    return 0;
}

// Register the `bus` global table on the given lua_State.
// Called once per type during register_type (each type has its own L).
static void register_bus_table(lua_State* L) {
    lua_newtable(L);                                          // bus = {}
    lua_pushcfunction(L, lua_bus_on);  lua_setfield(L, -2, "on");
    lua_pushcfunction(L, lua_bus_off); lua_setfield(L, -2, "off");
    lua_pushcfunction(L, lua_bus_emit);lua_setfield(L, -2, "emit");
    lua_setglobal(L, "bus");
}

// === Adapter callbacks ===

// Shallow-copy the root script table into a new instance table, ref it in
// the Lua registry, and return the ref. Used by create_instance (normal path)
// and my_begin_play (reload path, where instance_table_ref == LUA_NOREF).
// Returns LUA_NOREF on failure. Stack-neutral.
static int make_instance_table(LuaScriptType* type) {
    if (!type || !type->lua_state) return LUA_NOREF;
    lua_State* L = (lua_State*)type->lua_state;

    lua_rawgeti(L, LUA_REGISTRYINDEX, type->script_table_ref);
    if (!lua_istable(L, -1)) {
        std::fprintf(stderr, "make_instance_table: root script table missing\n");
        lua_pop(L, 1);
        return LUA_NOREF;
    }

    lua_newtable(L);  // instance table
    int root_idx = lua_absindex(L, -2);

    lua_pushnil(L);
    while (lua_next(L, root_idx) != 0) {
        lua_pushvalue(L, -2);  // copy key
        lua_pushvalue(L, -2);  // copy value
        lua_settable(L, -5);
        lua_pop(L, 1);  // drop value
    }
    lua_remove(L, -2);  // drop root, leave instance

    return luaL_ref(L, LUA_REGISTRYINDEX);  // pops instance table
}

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

// Invoke a one-arg Lua hook (self, dt) on the instance table:
//   function self:hook_name(dt)
// Silently skips missing hooks (Phase 2a §6.3). Stack-safe on every return path.
// Used by my_update, my_fixed_update, my_late_update — DO NOT inline copies.
static void invoke_lua_hook_dt(LuaScriptInstance* inst, const char* hook_name, float dt) {
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

    lua_pushvalue(L, -2);    // self (instance table)
    lua_pushnumber(L, dt);   // dt
    // Stack: instance_table hook_fn self dt

    int rc = lua_pcall(L, 2, 0, 0);
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
    if (!inst || !inst->type) return;

    // Phase 2b.3: Reload path — recreate sets instance_table_ref = LUA_NOREF.
    // Lazily populate the instance table here (shallow-copy of root script
    // table). Normal path (create_instance) already set this before begin_play.
    if (inst->instance_table_ref == LUA_NOREF) {
        inst->instance_table_ref = make_instance_table(inst->type);
        if (inst->instance_table_ref == LUA_NOREF) return;  // error already logged
    }

    auto& backend = LuaBackend::instance();
    auto* prev = backend.current_instance_;
    backend.current_instance_ = inst;
    invoke_lua_hook(inst, "begin_play");
    backend.current_instance_ = prev;
}

static void my_update(void* user_data, float dt) {
    auto* inst = static_cast<LuaScriptInstance*>(user_data);
    if (!inst || !inst->type || !inst->type->lua_state) return;

    auto& backend = LuaBackend::instance();
    auto* prev = backend.current_instance_;
    backend.current_instance_ = inst;

    invoke_lua_hook_dt(inst, "update", dt);

    backend.current_instance_ = prev;
}

static void my_fixed_update(void* user_data, float dt) {
    auto* inst = static_cast<LuaScriptInstance*>(user_data);
    if (!inst || !inst->type || !inst->type->lua_state) return;

    auto& backend = LuaBackend::instance();
    auto* prev = backend.current_instance_;
    backend.current_instance_ = inst;

    invoke_lua_hook_dt(inst, "fixed_update", dt);

    backend.current_instance_ = prev;
}

static void my_late_update(void* user_data, float dt) {
    auto* inst = static_cast<LuaScriptInstance*>(user_data);
    if (!inst || !inst->type || !inst->type->lua_state) return;

    auto& backend = LuaBackend::instance();
    auto* prev = backend.current_instance_;
    backend.current_instance_ = inst;

    invoke_lua_hook_dt(inst, "late_update", dt);

    backend.current_instance_ = prev;
}

static void my_destroy(void* user_data) {
    auto* inst = static_cast<LuaScriptInstance*>(user_data);
    if (!inst) return;

    auto& backend = LuaBackend::instance();

    // Set current_instance_ so destroy hook can call bus.* if needed.
    auto* prev = backend.current_instance_;
    backend.current_instance_ = inst;

    // 1. Call Lua destroy hook first (script may want to read its fields
    //    before we release the table ref).
    // Phase 2b.3: In reload path, capture_state_for_reload already fired the
    // Lua destroy hook and set instance_table_ref = LUA_NOREF. Skip to avoid
    // double-fire + suppress the "instance table missing" warning noise.
    if (inst->instance_table_ref != LUA_NOREF) {
        invoke_lua_hook(inst, "destroy");
    }

    backend.current_instance_ = prev;

    // Phase 2b.2: cleanup all subscriptions owned by this instance.
    // Destroy hook may have bus.off'd some; whatever remains is a leak we
    // must untie so the engine doesn't dispatch into a dead lua_State.
    auto it = backend.inst_subs_.find(inst);
    if (it != backend.inst_subs_.end()) {
        auto recs = std::move(it->second);  // copy out so release_sub_record
        backend.inst_subs_.erase(it);        // can mutate maps safely
        for (auto* rec : recs) {
            release_sub_record(rec);
        }
    }

    // 2. Release the instance table registry ref.
    if (inst->type && inst->type->lua_state &&
        inst->instance_table_ref != LUA_NOREF) {
        lua_State* L = (lua_State*)inst->type->lua_state;
        luaL_unref(L, LUA_REGISTRYINDEX, inst->instance_table_ref);
        inst->instance_table_ref = LUA_NOREF;
    }

    // 3. Drop the LuaScriptInstance from the map. The `inst` pointer is now
    //    dangling — do not touch it after this call.
    backend.erase_instance(inst->script_id);
}

// === Phase 2b.3: capture_state_for_reload ===
// Transfer ownership of instance_table_ref from LuaScriptInstance to a
// heap-allocated LuaReloadState. Sets instance->instance_table_ref = LUA_NOREF
// as a sentinel so my_destroy's luaL_unref is a no-op in the reload path.
static void* lua_capture_state_for_reload(void* user_data) {
    auto* inst = static_cast<LuaScriptInstance*>(user_data);
    if (!inst || !inst->type) return nullptr;

    // Phase 2b.3 fix: Fire the Lua destroy hook BEFORE transferring table
    // ownership. Engine's process_deferred_reloads_impl calls capture_state,
    // then later calls my_destroy which would normally invoke the Lua destroy
    // hook — but by then instance_table_ref has been moved to LUA_NOREF.
    // Calling it here preserves "destroy fires once per instance lifetime".
    invoke_lua_hook(inst, "destroy");

    auto* state = new LuaReloadState{};
    state->lua_state = inst->type->lua_state;
    state->captured_table_ref = inst->instance_table_ref;  // transfer ownership
    inst->instance_table_ref = LUA_NOREF;  // sentinel
    return state;
}

// === Phase 2b.3: delete_captured_state ===
// Called by engine after on_reload consumes the state (or on abort).
// Finally frees the old instance table's Lua ref + the C++ struct.
static void lua_delete_captured_state(void* state) {
    auto* s = static_cast<LuaReloadState*>(state);
    if (!s) return;
    if (s->lua_state && s->captured_table_ref != LUA_NOREF) {
        luaL_unref((lua_State*)s->lua_state, LUA_REGISTRYINDEX, s->captured_table_ref);
    }
    delete s;
}

// === Phase 2b.3: recreate_instance_user_data_for_reload ===
// Called AFTER my_destroy erased the old map entry. Delegates to
// LuaBackend::recreate_instance_for_reload which allocates a fresh
// LuaScriptInstance and re-inserts into instances_ under the same script_id
// (engine reuses script_id, doesn't re-assign).
static void* lua_recreate_instance_user_data_for_reload(
    void* type_user_data, u64 entity_id, u64 script_id
) {
    auto* type = static_cast<LuaScriptType*>(type_user_data);
    if (!type) return nullptr;
    return LuaBackend::instance().recreate_instance_for_reload(type, entity_id, script_id);
}

// === Phase 2b.3: my_on_reload ===
// Engine's external_script::on_reload dispatches here. We resolve the
// Lua-side on_reload function from the script table and call it with
// (self_instance_table, captured_old_state_table_or_nil).
static void my_on_reload(void* user_data, void* old_state) {
    auto* inst = static_cast<LuaScriptInstance*>(user_data);
    if (!inst || !inst->type) return;

    lua_State* L = (lua_State*)inst->type->lua_state;
    if (!L) return;

    // Phase 2b.3 fix: Set current_instance_ so on_reload hook can call
    // bus.on()/bus.off() consistently with other hooks.
    auto& backend = LuaBackend::instance();
    auto* prev = backend.current_instance_;
    backend.current_instance_ = inst;

    // Push script table, look up on_reload
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->type->script_table_ref);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        backend.current_instance_ = prev;
        return;
    }
    lua_getfield(L, -1, "on_reload");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);  // pop nil + script table
        backend.current_instance_ = prev;
        return;
    }

    // Push self (instance table)
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->instance_table_ref);

    // Push old_state (captured table) or nil
    if (old_state) {
        auto* state = static_cast<LuaReloadState*>(old_state);
        lua_rawgeti(L, LUA_REGISTRYINDEX, state->captured_table_ref);
    } else {
        lua_pushnil(L);
    }

    // pcall(on_reload, self, old_state)
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        std::fprintf(stderr, "lua on_reload: %s\n", err ? err : "(non-string error)");
        lua_pop(L, 1);
    }

    lua_pop(L, 1);  // pop script table

    backend.current_instance_ = prev;
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
