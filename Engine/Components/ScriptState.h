#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common/PrimitiveTypes.h"

// Forward decl to break include cycle: ScriptState.cpp uses Script's frame state
// via accessors; ScriptState.h itself only exposes the engine_getter registration
// so Script.cpp can register built-ins without circular includes.
namespace primal::script {

struct StateValue {
    enum class Tag { Nil, Bool, Number, String, Table } tag = Tag::Nil;
    bool           b = false;
    double         n = 0.0;
    std::string    s;
    std::unordered_map<std::string, StateValue> t;

    static StateValue make_nil()                { StateValue v; v.tag = Tag::Nil;    return v; }
    static StateValue make_bool(bool x)         { StateValue v; v.tag = Tag::Bool;   v.b = x; return v; }
    static StateValue make_number(double x)     { StateValue v; v.tag = Tag::Number; v.n = x; return v; }
    static StateValue make_string(std::string x){ StateValue v; v.tag = Tag::String; v.s = std::move(x); return v; }
    static StateValue make_table()              { StateValue v; v.tag = Tag::Table;  return v; }
};

class ScriptState {
public:
    static ScriptState& instance();

    // === Lua-owned stores ===
    // get() walks path segments; returns Nil if any segment is missing.
    // For "engine" namespace, returns the getter result (Nil if no getter registered).
    // For "types" namespace, returns Nil if path[1] is not a registered type name.
    // For "entities" namespace, returns Nil if path[1] is not a valid entity id.
    StateValue get(const std::vector<std::string>& path);
    // set() walks path segments; creates intermediate Table values as needed.
    // writer_entity_id is used for state.entities.<id>.* ownership check;
    // pass u64_invalid_id for non-entity writes (no ownership to enforce).
    // Throws std::runtime_error on:
    //   - path[0] == "engine" (read-only C++-owned)
    //   - path[0] == "entities" && path[1] parsed eid != writer_entity_id
    //   - path[0] == "types" && path[1] is not a registered type name
    void set(const std::vector<std::string>& path, const StateValue& v, u64 writer_entity_id);

    // === C++-owned registration ===
    void register_engine_getter(const std::string& key, std::function<StateValue()> getter);
    void unregister_engine_getter(const std::string& key);
    bool has_engine_getter(const std::string& key) const;
    StateValue call_engine_getter(const std::string& key);  // Nil if missing

    // === Type registration (called by LuaBackend::register_type) ===
    void register_type(u64 type_id, const std::string& type_name);
    void unregister_type(u64 type_id);
    bool is_registered_type(const std::string& type_name) const;

    // === Entity lifecycle ===
    void clear_entity(u64 entity_id);

    // === Singleton lifecycle ===
    void initialize();
    void shutdown();
    bool is_initialized() const { return initialized_; }

private:
    ScriptState() = default;
    ~ScriptState() = default;
    ScriptState(const ScriptState&) = delete;
    ScriptState& operator=(const ScriptState&) = delete;

    bool                                  initialized_ = false;
    mutable std::mutex                    mutex_;

    std::unordered_map<std::string, StateValue>                          game_store_;
    std::unordered_map<u64, std::unordered_map<std::string, StateValue>> type_store_;
    std::unordered_map<std::string, u64>                                 type_name_to_id_;
    std::unordered_map<u64, std::unordered_map<std::string, StateValue>> entity_store_;
    std::unordered_map<std::string, std::function<StateValue()>>         engine_getters_;

    // Internal: walk nested table within a single store's map.
    static StateValue lookup_in_map(
        const std::unordered_map<std::string, StateValue>& store,
        const std::vector<std::string>& path, size_t start);
    // Internal: write nested, creating intermediate Table values.
    static void assign_in_map(
        std::unordered_map<std::string, StateValue>& store,
        const std::vector<std::string>& path, size_t start,
        const StateValue& v);
};

// Singleton accessor (mirrors the post_to_main_thread family's free-function style).
ScriptState& state();

}  // namespace primal::script
