#include "Components/ScriptState.h"

#include <stdexcept>

namespace primal::script {

ScriptState& ScriptState::instance() {
    static ScriptState s;
    return s;
}

ScriptState& state() { return ScriptState::instance(); }

void ScriptState::initialize() {
    std::lock_guard<std::mutex> lk(mutex_);
    initialized_ = true;
}

void ScriptState::shutdown() {
    std::lock_guard<std::mutex> lk(mutex_);
    game_store_.clear();
    type_store_.clear();
    type_name_to_id_.clear();
    entity_store_.clear();
    engine_getters_.clear();
    initialized_ = false;
}

bool ScriptState::is_registered_type(const std::string& type_name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return type_name_to_id_.find(type_name) != type_name_to_id_.end();
}

void ScriptState::register_type(u64 type_id, const std::string& type_name) {
    std::lock_guard<std::mutex> lk(mutex_);
    type_name_to_id_[type_name] = type_id;
    type_store_[type_id];  // create empty sub-map if not present
}

void ScriptState::unregister_type(u64 type_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = type_name_to_id_.begin(); it != type_name_to_id_.end(); ++it) {
        if (it->second == type_id) { type_name_to_id_.erase(it); break; }
    }
    type_store_.erase(type_id);
}

void ScriptState::clear_entity(u64 entity_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    entity_store_.erase(entity_id);
}

void ScriptState::register_engine_getter(const std::string& key, std::function<StateValue()> getter) {
    std::lock_guard<std::mutex> lk(mutex_);
    engine_getters_[key] = std::move(getter);
}

void ScriptState::unregister_engine_getter(const std::string& key) {
    std::lock_guard<std::mutex> lk(mutex_);
    engine_getters_.erase(key);
}

bool ScriptState::has_engine_getter(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return engine_getters_.find(key) != engine_getters_.end();
}

StateValue ScriptState::call_engine_getter(const std::string& key) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = engine_getters_.find(key);
    if (it == engine_getters_.end()) return StateValue::make_nil();
    return it->second();
}

StateValue ScriptState::lookup_in_map(
    const std::unordered_map<std::string, StateValue>& store,
    const std::vector<std::string>& path, size_t start) {
    const StateValue* current = nullptr;
    for (size_t i = start; i < path.size(); ++i) {
        if (current == nullptr) {
            auto it = store.find(path[i]);
            if (it == store.end()) return StateValue::make_nil();
            current = &it->second;
        } else {
            if (current->tag != StateValue::Tag::Table) return StateValue::make_nil();
            auto it = current->t.find(path[i]);
            if (it == current->t.end()) return StateValue::make_nil();
            current = &it->second;
        }
    }
    if (current == nullptr) return StateValue::make_nil();
    return *current;
}

void ScriptState::assign_in_map(
    std::unordered_map<std::string, StateValue>& store,
    const std::vector<std::string>& path, size_t start,
    const StateValue& v) {
    if (path.size() == start) return;  // nothing to assign
    if (path.size() == start + 1) {
        if (v.tag == StateValue::Tag::Nil) store.erase(path[start]);
        else store[path[start]] = v;
        return;
    }
    // Walk / create intermediate tables
    auto* current = &store;
    for (size_t i = start; i < path.size() - 1; ++i) {
        auto it = current->find(path[i]);
        if (it == current->end() || it->second.tag != StateValue::Tag::Table) {
            (*current)[path[i]] = StateValue::make_table();
            current = &(*current)[path[i]].t;
        } else {
            current = &it->second.t;
        }
    }
    const std::string& leaf = path.back();
    if (v.tag == StateValue::Tag::Nil) current->erase(leaf);
    else (*current)[leaf] = v;
}

StateValue ScriptState::get(const std::vector<std::string>& path) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (path.empty()) return StateValue::make_nil();
    const std::string& ns = path[0];
    if (ns == "game") {
        return lookup_in_map(game_store_, path, 1);
    } else if (ns == "engine") {
        if (path.size() != 2) return StateValue::make_nil();
        auto it = engine_getters_.find(path[1]);
        if (it == engine_getters_.end()) return StateValue::make_nil();
        return it->second();
    } else if (ns == "types") {
        if (path.size() < 2) return StateValue::make_nil();
        auto tit = type_name_to_id_.find(path[1]);
        if (tit == type_name_to_id_.end()) return StateValue::make_nil();
        auto store_it = type_store_.find(tit->second);
        if (store_it == type_store_.end()) return StateValue::make_nil();
        return lookup_in_map(store_it->second, path, 2);
    } else if (ns == "entities") {
        if (path.size() < 2) return StateValue::make_nil();
        try {
            u64 eid = std::stoull(path[1]);
            auto store_it = entity_store_.find(eid);
            if (store_it == entity_store_.end()) return StateValue::make_nil();
            return lookup_in_map(store_it->second, path, 2);
        } catch (...) { return StateValue::make_nil(); }
    }
    return StateValue::make_nil();
}

void ScriptState::set(const std::vector<std::string>& path, const StateValue& v, u64 writer_entity_id) {
    if (path.empty()) throw std::runtime_error("state.set: empty path");
    std::lock_guard<std::mutex> lk(mutex_);
    const std::string& ns = path[0];
    if (ns == "game") {
        assign_in_map(game_store_, path, 1, v);
    } else if (ns == "engine") {
        throw std::runtime_error("state.engine.* is read-only (C++-owned)");
    } else if (ns == "types") {
        if (path.size() < 2) throw std::runtime_error("state.types: missing type name");
        auto tit = type_name_to_id_.find(path[1]);
        if (tit == type_name_to_id_.end())
            throw std::runtime_error("state.types." + path[1] + ": type not registered");
        assign_in_map(type_store_[tit->second], path, 2, v);
    } else if (ns == "entities") {
        if (path.size() < 2) throw std::runtime_error("state.entities: missing entity id");
        u64 eid = 0;
        try { eid = std::stoull(path[1]); }
        catch (...) { throw std::runtime_error("state.entities: invalid entity id"); }
        if (writer_entity_id != eid) {
            throw std::runtime_error("state.entities." + path[1] + ".* is write-protected");
        }
        assign_in_map(entity_store_[eid], path, 2, v);
    } else {
        throw std::runtime_error("state: unknown namespace '" + ns + "'");
    }
}

}  // namespace primal::script
