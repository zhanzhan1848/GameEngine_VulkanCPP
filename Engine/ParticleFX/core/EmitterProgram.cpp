// EmitterProgram.cpp — 发射器程序编译器（见 EmitterProgram.h）。

#include "EmitterProgram.h"
#include "ModuleRegistry.h"

#ifndef DISABLE_PARTICLE_FX

#include <cstdio>
#include <cstring>

namespace primal::pfx {

namespace {

// ---- 带路径的错误拼装 -------------------------------------------------------

struct compile_ctx {
    std::string& err;      // 直接写调用方缓冲（修复：原内部缓冲从不回传）
    bool failed{ false };

    explicit compile_ctx(std::string& sink) : err(sink) {}

    bool fail(const std::string& path, const std::string& message, u32 line) {
        if (!failed) {
            failed = true;
            char linebuf[16];
            std::snprintf(linebuf, sizeof(linebuf), "%u", line);
            err = path + ": " + message + " (line " + linebuf + ")";
        }
        return false;
    }
};

bool get_number(const json::value& obj, const char* key, f32& out, compile_ctx& c, const std::string& path) {
    const json::value* v = obj.find(key);
    if (!v) return c.fail(path, std::string("missing field '") + key + "'", obj.line());
    if (!v->is_number()) return c.fail(path, std::string("'") + key + "' must be a number", v->line());
    out = v->as_f32();
    return true;
}

bool get_u32_field(const json::value& obj, const char* key, u32& out, compile_ctx& c, const std::string& path) {
    const json::value* v = obj.find(key);
    if (!v) return c.fail(path, std::string("missing field '") + key + "'", obj.line());
    if (!v->is_number()) return c.fail(path, std::string("'") + key + "' must be a number", v->line());
    out = v->as_u32();
    return true;
}

bool get_string(const json::value& obj, const char* key, std::string& out, compile_ctx& c,
                const std::string& path) {
    const json::value* v = obj.find(key);
    if (!v) return c.fail(path, std::string("missing field '") + key + "'", obj.line());
    if (!v->is_string()) return c.fail(path, std::string("'") + key + "' must be a string", v->line());
    out = v->as_string();
    return true;
}

bool get_bool_field(const json::value& obj, const char* key, bool& out, compile_ctx& c,
                    const std::string& path) {
    const json::value* v = obj.find(key);
    if (!v) return c.fail(path, std::string("missing field '") + key + "'", obj.line());
    if (!v->is_bool()) return c.fail(path, std::string("'") + key + "' must be a bool", v->line());
    out = v->as_bool();
    return true;
}

// 可绑定暴露参数的 f32：数字，或 {"param":"Name","default":x}。
// exposed 名册须先于 stages 编译（绑定按名查下标）。
bool get_param_ref(const json::value& obj, const char* key, const std::vector<exposed_param>& exposed,
                   param_ref& out, compile_ctx& c, const std::string& path) {
    const json::value* v = obj.find(key);
    if (!v) return c.fail(path, std::string("missing field '") + key + "'", obj.line());
    if (v->is_number()) {
        out = param_ref{ u32_invalid_id, v->as_f32() };
        return true;
    }
    if (v->is_object()) {
        const json::value* pname = v->find("param");
        if (!pname || !pname->is_string())
            return c.fail(path + "." + key, "'param' binding requires string 'param'", v->line());
        const u32 h = hash_name(pname->as_string().c_str());
        for (u32 i = 0; i < exposed.size(); ++i) {
            if (exposed[i].name_hash == h) {
                param_ref ref;
                ref.override_idx = i;
                if (const json::value* d = v->find("default"); d && d->is_number()) ref.value = d->as_f32();
                out = ref;
                return true;
            }
        }
        return c.fail(path + "." + key, "unknown exposed param '" + pname->as_string() + "'", v->line());
    }
    return c.fail(path, std::string("'") + key + "' must be a number or param binding", v->line());
}

bool get_v3(const json::value& obj, const char* key, math::v3& out, compile_ctx& c, const std::string& path) {
    const json::value* v = obj.find(key);
    if (!v) return c.fail(path, std::string("missing field '") + key + "'", obj.line());
    if (!v->is_array() || v->array_size() != 3)
        return c.fail(path, std::string("'") + key + "' must be [x,y,z]", v->line());
    f32 tmp[3];
    for (int i = 0; i < 3; ++i) {
        const json::value* e = v->array_at(i);
        if (!e->is_number()) return c.fail(path, std::string("'") + key + "' elements must be numbers", e->line());
        tmp[i] = e->as_f32();
    }
    out = math::v3{ tmp[0], tmp[1], tmp[2] };
    return true;
}

} // namespace

// ---- 编译器 -----------------------------------------------------------------

bool compile_emitter(const json::value& root, emitter_program& out, std::string& err) {
    out = emitter_program{};
    register_builtin_modules();
    module_registry& reg = module_registry::get();
    compile_ctx c{err};

    if (!root.is_object()) return c.fail("root", "emitter asset must be a JSON object", root.line());
    const std::string path;

    // 元信息（全部可选）
    if (const json::value* v = root.find("name"); v && v->is_string()) out.name = v->as_string();
    if (const json::value* v = root.find("blend"); v && v->is_string()) {
        const char* names[] = { "additive", "alpha", "multiply", "premultiplied" };
        out.blend = 0;
        for (u8 i = 0; i < 4; ++i) {
            if (v->as_string() == names[i]) { out.blend = i; break; }
        }
    }
    if (const json::value* v = root.find("renderer"); v && v->is_string()) {
        // §7-N2：渲染器类型声明。v1：billboard（sprite 桥）/ mesh（立方体实例）。
        if (v->as_string() == "mesh") out.renderer_type = emitter_program::renderer_mesh;
    }
    if (const json::value* v = root.find("capacity"); v) {
        if (!v->is_number() || v->as_u32() == 0) return c.fail("capacity", "must be positive number", v->line());
        out.capacity = v->as_u32();
    }
    if (const json::value* v = root.find("seed"); v) {
        if (!v->is_number()) return c.fail("seed", "must be a number", v->line());
        out.seed = v->as_u32();
    }
    if (const json::value* v = root.find("source_velocity"); v) {
        get_v3(root, "source_velocity", out.source_velocity, c, path);   // 存在则必合法
        if (c.failed) return false;
    }

    // 用户属性（先于 stages：模块参数可引用？v1 不引用，但顺序上保持先注册）
    if (const json::value* attrs = root.find("attributes"); attrs) {
        if (!attrs->is_array()) return c.fail("attributes", "must be an array", attrs->line());
        if (attrs->array_size() > attribute_store::max_channels - attr::standard_count)
            return c.fail("attributes", "too many user attributes (max 11)", attrs->line());
        for (size_t i = 0; i < attrs->array_size(); ++i) {
            const json::value* a = attrs->array_at(i);
            if (!a->is_object()) return c.fail("attributes[i]", "must be an object", a->line());
            std::string name;
            if (!get_string(*a, "name", name, c, "attributes[i]")) return false;
            if (name.empty()) return c.fail("attributes[i].name", "must be non-empty", a->line());
            f32 stride = attr::standard_stride;
            if (const json::value* s = a->find("stride"); s) {
                if (!s->is_number() || s->as_u32() == 0 || s->as_u32() > 256)
                    return c.fail("attributes[i].stride", "must be 1..256", s->line());
                stride = s->as_f32();
            }
            user_attr ua{ hash_name(name.c_str()), (u32)stride };
            for (const auto& existing : out.user_attrs) {
                if (existing.name_hash == ua.name_hash)
                    return c.fail("attributes[i].name", "duplicate attribute name", a->line());
            }
            out.user_attrs.push_back(ua);
        }
    }

    // 暴露参数（先于 stages：模块 param_ref 按名绑定下标）
    if (const json::value* exposed = root.find("exposed"); exposed) {
        if (!exposed->is_array()) return c.fail("exposed", "must be an array", exposed->line());
        for (size_t i = 0; i < exposed->array_size(); ++i) {
            const json::value* e = exposed->array_at(i);
            if (!e->is_object()) return c.fail("exposed[i]", "must be an object", e->line());
            std::string name;
            if (!get_string(*e, "name", name, c, "exposed[i]")) return false;
            f32 def = 0.f;
            if (const json::value* d = e->find("default"); d) {
                if (!d->is_number()) return c.fail("exposed[i].default", "must be a number", d->line());
                def = d->as_f32();
            }
            const u32 h = hash_name(name.c_str());
            for (const auto& existing : out.exposed) {
                if (existing.name_hash == h)
                    return c.fail("exposed[i].name", "duplicate exposed param name", e->line());
            }
            out.exposed.push_back(exposed_param{ h, def });
        }
    }

    // 场表（N4）：.pgrid 文件加载，先于 stages（模块按名绑定下标）
    std::vector<std::string> field_names;
    if (const json::value* fields = root.find("fields"); fields) {
        if (!fields->is_array()) return c.fail("fields", "must be an array", fields->line());
        for (size_t i = 0; i < fields->array_size(); ++i) {
            const json::value* e = fields->array_at(i);
            if (!e->is_object()) return c.fail("fields[i]", "must be an object", e->line());
            std::string fname, ffile;
            if (!get_string(*e, "name", fname, c, "fields[i]")) return false;
            if (!get_string(*e, "file", ffile, c, "fields[i]")) return false;
            for (const auto& existing : field_names) {
                if (existing == fname) return c.fail("fields[i].name", "duplicate field name", e->line());
            }
            auto grid = std::make_shared<volume_grid>();
            std::string load_err;
            if (!grid->read_pgrid(ffile.c_str(), load_err))
                return c.fail("fields[i].file", load_err, e->line());
            field_names.push_back(fname);
            out.fields.push_back(emitter_program::field_entry{ fname, grid });
            out.field_grids.push_back(std::move(grid));
        }
    }

    // 编译环境：暴露参数绑定（RAII 保证任何错误路径都清空，防悬挂指针）
    struct parse_env_guard {
        explicit parse_env_guard(emitter_program& p) { set_compile_env_exposed(&p.exposed); }
        ~parse_env_guard() { set_compile_env_exposed(nullptr); }
    } env_guard{out};
    set_compile_env_fields(&field_names);
    struct fields_guard {
        ~fields_guard() { set_compile_env_fields(nullptr); }
    } fields_guard_;

    // 阶段模块
    const json::value* stages = root.find("stages");
    if (!stages)
        return c.fail("stages", "missing 'stages' object", root.line());
    if (!stages->is_object())
        return c.fail("stages", "must be an object", stages->line());

    for (u8 sk = 0; sk < 3; ++sk) {
        const char* sname = stage_name((stage_kind)sk);
        const json::value* arr = stages->find(sname);
        if (!arr) continue;
        if (!arr->is_array()) return c.fail(std::string("stages.") + sname, "must be an array", arr->line());
        for (size_t i = 0; i < arr->array_size(); ++i) {
            const json::value* m = arr->array_at(i);
            const std::string mpath = std::string("stages.") + sname + "[" + std::to_string(i) + "]";
            if (!m->is_object()) return c.fail(mpath, "module entry must be an object", m->line());
            std::string mod_name;
            if (!get_string(*m, "module", mod_name, c, mpath)) return false;
            const u32 h = hash_name(mod_name.c_str());
            const module_def* def = reg.find(h);
            if (!def) return c.fail(mpath, "unknown module '" + mod_name + "'", m->line());
            if (def->stage != (stage_kind)sk)
                return c.fail(mpath,
                              "module '" + mod_name + "' belongs to stage '" + stage_name(def->stage) + "'",
                              m->line());

            module_instance inst;
            inst.def_index = (u32)(def - reg.all().data());
            inst.params.resize(def->params_size);
            if (def->init_defaults) def->init_defaults(inst.params.data());
            if (def->parse && !def->parse(*m, inst.params.data(), c.err)) {
                if (!c.failed) c.fail(mpath, "parameter error: " + c.err, m->line());
                return false;
            }
            out.stages[sk].push_back(std::move(inst));
        }
    }

    // 状态块偏移分配
    u32 state_off = 0;
    for (u8 sk = 0; sk < 3; ++sk) {
        for (auto& inst : out.stages[sk]) {
            inst.state_offset = state_off;
            state_off += (u32)reg.all()[inst.def_index].state_size;
        }
    }
    out.state_bytes = state_off;
    return true;
}

bool compile_emitter_from_text(const char* text, size_t length, emitter_program& out, std::string& err) {
    json::parse_result parsed = json::parse(text, length);
    if (!parsed.ok) {
        char linebuf[16], colbuf[16];
        std::snprintf(linebuf, sizeof(linebuf), "%u", parsed.line);
        std::snprintf(colbuf, sizeof(colbuf), "%u", parsed.column);
        err = "JSON parse error at line " + std::string(linebuf) + " column " + std::string(colbuf) + ": " +
              parsed.error;
        return false;
    }
    return compile_emitter(*parsed.root, out, err);
}

bool load_emitter_asset(const char* path, emitter_program& out, std::string& err) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        err = std::string("cannot open '") + path + "'";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) {
        std::fclose(f);
        err = std::string("invalid asset size for '") + path + "'";
        return false;
    }
    std::string text(size, '\0');
    const size_t read = std::fread(text.data(), 1, (size_t)size, f);
    std::fclose(f);
    if (read != (size_t)size) {
        err = std::string("short read on '") + path + "'";
        return false;
    }
    return compile_emitter_from_text(text.data(), text.size(), out, err);
}

// ---- schema 反射 -------------------------------------------------------------

std::string build_schema_json() {
    register_builtin_modules();
    module_registry& reg = module_registry::get();
    std::string s = "{\"version\":1,\"stages\":[\"spawn\",\"init\",\"update\"],\"standard_attributes\":"
                    "[\"position\",\"velocity\",\"color\",\"scale_rotation\",\"uv_params\"],\"modules\":[";
    bool first_mod = true;
    for (const auto& def : reg.all()) {
        if (!first_mod) s += ",";
        first_mod = false;
        s += "{\"name\":\"";
        s += def.name;
        s += "\",\"stage\":\"";
        s += stage_name(def.stage);
        s += "\",\"params\":[";
        for (u32 i = 0; i < def.hint_count; ++i) {
            if (i) s += ",";
            s += "{\"name\":\"";
            s += def.hints[i].name;
            s += "\",\"type\":\"";
            s += def.hints[i].type;
            s += "\"";
            // default 为 JSON 编码文本，原样内联（如 "100"、"[0,1,0]"）
            if (def.hints[i].def && def.hints[i].def[0]) {
                s += ",\"default\":";
                s += def.hints[i].def;
            }
            if (def.hints[i].note && def.hints[i].note[0]) {
                s += ",\"note\":\"";
                s += def.hints[i].note;
                s += "\"";
            }
            s += "}";
        }
        s += "]}";
    }
    s += "]}";
    return s;
}

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
