// PfxSystem.cpp — 见 PfxSystem.h。
//
// N0 实现注记：
// - 系统级单一事件总线（事件自带 source emitter id）；架构文档写的 per-emitter
//   环形缓冲简化为单总线 —— 消费端一次 drain 即全部，ADR4 语义不变。
// - emitter id 单调递增不复用，销毁后残留事件的 source 不会错绑新发射器。

#include "PfxSystem.h"
#include "Common/CoreLogStream.h"

#ifndef DISABLE_PARTICLE_FX

#include <unordered_map>

#include <unordered_map>

namespace primal::pfx {

namespace {

system_config g_config{};
std::unordered_map<emitter_id, std::unique_ptr<pfx_emitter>> g_emitters;
std::unique_ptr<event_bus> g_bus;
emitter_id g_next_emitter_id{ 1 };
bool g_initialized{ false };
f32 g_global_lod{ 1.f };

} // namespace

// ---- pfx_emitter -----------------------------------------------------------

pfx_emitter::pfx_emitter(emitter_id id, const emitter_init& init) : _id(id) {
    _store.initialize(init.capacity, init.user_channels, init.user_channel_count);
    _store.set_event_sink(g_bus.get(), id);
    _rng.seed(init.seed);
}

pfx_emitter::pfx_emitter(emitter_id id, const emitter_program& program) : _id(id), _has_program(true) {
    _program = program;
    _exposed_values.resize(_program.exposed.size());
    for (size_t i = 0; i < _program.exposed.size(); ++i) {
        _exposed_values[i] = _program.exposed[i].default_value;
    }
    std::vector<attr_desc> users(_program.user_attrs.size());
    for (size_t i = 0; i < users.size(); ++i) {
        users[i] = attr_desc{ _program.user_attrs[i].name_hash, _program.user_attrs[i].stride };
    }
    _store.initialize(_program.capacity, users.data(), (u32)users.size());
    _store.set_event_sink(g_bus.get(), id);
    _rng.seed(_program.seed);
    _module_state.resize(_program.state_bytes, 0);
}

bool pfx_emitter::rebind_program(const emitter_program& program) {
    if (!_has_program) return false;   // 直接 spawn 模式不支持热重载
    _program = program;
    _exposed_values.resize(_program.exposed.size());
    for (size_t i = 0; i < _program.exposed.size(); ++i) {
        _exposed_values[i] = _program.exposed[i].default_value;
    }
    std::vector<attr_desc> users(_program.user_attrs.size());
    for (size_t i = 0; i < users.size(); ++i) {
        users[i] = attr_desc{ _program.user_attrs[i].name_hash, _program.user_attrs[i].stride };
    }
    _store.initialize(_program.capacity, users.data(), (u32)users.size());
    _store.set_event_sink(g_bus.get(), _id);
    _rng.seed(_program.seed);
    _module_state.assign(_program.state_bytes, 0);
    _spawn_accum = 0.f;
    _elapsed = 0.f;
    return true;
}

particle_handle pfx_emitter::spawn(math::v3 position, math::v3 velocity, math::v4 color, math::v2 scale,
                                   f32 lifetime) {
    const particle_handle h = _store.allocate();
    if (!h.is_valid()) return h;

    math::v4* pos = _store.v4_channel(attr::position) + h.row;
    math::v4* vel = _store.v4_channel(attr::velocity) + h.row;
    math::v4* col = _store.v4_channel(attr::color) + h.row;
    math::v4* scl = _store.v4_channel(attr::scale_rotation) + h.row;
    math::v4* uv = _store.v4_channel(attr::uv_params) + h.row;

    *pos = math::v4{ position.x, position.y, position.z, 0.0f };   // w = age
    *vel = math::v4{ velocity.x, velocity.y, velocity.z, lifetime };
    *col = color;
    *scl = math::v4{ scale.x, scale.y, 0.0f, 0.0f };
    *uv = math::v4{ 0.0f, 0.0f, 0.0f, 0.0f };

    if (g_bus) {
        g_bus->push(pfx_event{ h, _id, event_type::birth, 0.0f });
    }
    return h;
}

void pfx_emitter::tick(f32 dt) {
    _elapsed += dt;
    if (_has_program) {
        tick_program(dt);
    } else {
        tick_legacy(dt);
    }
}

void pfx_emitter::tick_legacy(f32 dt) {
    const u32 cap = _store.capacity();
    math::v4* pos = _store.v4_channel(attr::position);
    math::v4* vel = _store.v4_channel(attr::velocity);

    for (u32 row = 0; row < cap; ++row) {
        if (!_store.is_row_alive(row)) continue;
        pos[row].w += dt;
        // 到期回收：free 内部发 death 事件并归还行。
        if (pos[row].w >= vel[row].w) {
            _store.free(_store.row_handle(row));
        }
    }
}

void pfx_emitter::tick_program(f32 dt) {
    module_registry& reg = module_registry::get();
    const std::vector<module_instance>* stage_lists = _program.stages;

    sim_context ctx;
    ctx.store = &_store;
    ctx.rng = &_rng;
    ctx.exposed = _exposed_values.data();
    ctx.dt = dt;
    ctx.elapsed = _elapsed;
    ctx.source = _id;
    ctx.source_velocity = _program.source_velocity;
    ctx.spawn_accum = &_spawn_accum;
    ctx.fields = &_program.field_grids;
    ctx.meshes = &_program.mesh_views;
    ctx.lod_factor = g_global_lod;

    // ---- spawn 阶段：各模块贡献生成数 ----
    u32 want = 0;
    for (const auto& inst : stage_lists[(u8)stage_kind::spawn]) {
        const module_def& def = reg.all()[inst.def_index];
        ctx.state = _module_state.data() + inst.state_offset;
        if (def.spawn_exec) want += def.spawn_exec(ctx, inst.params.data());
    }
    const u32 free_rows = _store.free_count();
    const u32 to_spawn = want < free_rows ? want : free_rows;

    // ---- init 阶段：逐新行执行 ----
    for (u32 i = 0; i < to_spawn; ++i) {
        const particle_handle h = _store.allocate();
        if (!h.is_valid()) break;

        // 标准通道默认值（模块按需覆盖）
        math::v4* pos = _store.v4_channel(attr::position) + h.row;
        math::v4* vel = _store.v4_channel(attr::velocity) + h.row;
        math::v4* col = _store.v4_channel(attr::color) + h.row;
        math::v4* scl = _store.v4_channel(attr::scale_rotation) + h.row;
        math::v4* uv = _store.v4_channel(attr::uv_params) + h.row;
        *pos = math::v4{ 0.f, 0.f, 0.f, 0.f };
        *vel = math::v4{ 0.f, 0.f, 0.f, 1.f };
        *col = math::v4{ 1.f, 1.f, 1.f, 1.f };
        *scl = math::v4{ 1.f, 1.f, 0.f, 0.f };
        *uv = math::v4{ 0.f, 0.f, 0.f, 0.f };

        ctx.row = h.row;
        ctx.handle = h;
        ctx.last_normal = math::v3{ 0.f, 0.f, 1.f };
        for (const auto& inst : stage_lists[(u8)stage_kind::init]) {
            const module_def& def = reg.all()[inst.def_index];
            ctx.state = _module_state.data() + inst.state_offset;
            if (def.row_exec) def.row_exec(ctx, inst.params.data());
            if (!_store.is_row_alive(h.row)) break;   // 防御：init 模块理论上不杀行
        }
        // 发射器世界位置：init 模块在发射器本地空间生成，出栈时叠加宿主实体偏移
        if (_position.x != 0.f || _position.y != 0.f || _position.z != 0.f) {
            math::v4* pos = _store.v4_channel(attr::position) + h.row;
            pos->x += _position.x;
            pos->y += _position.y;
            pos->z += _position.z;
        }
        if (g_bus) {
            g_bus->push(pfx_event{ h, _id, event_type::birth, 0.0f });
        }
    }

    // ---- update 阶段：逐活行执行，随后 age 推进与到期回收 ----
    const u32 cap = _store.capacity();
    math::v4* pos = _store.v4_channel(attr::position);
    math::v4* vel = _store.v4_channel(attr::velocity);

    for (u32 row = 0; row < cap; ++row) {
        if (!_store.is_row_alive(row)) continue;
        const particle_handle h = _store.row_handle(row);
        ctx.row = row;
        ctx.handle = h;

        bool died = false;
        for (const auto& inst : stage_lists[(u8)stage_kind::update]) {
            const module_def& def = reg.all()[inst.def_index];
            ctx.state = _module_state.data() + inst.state_offset;
            if (def.row_exec) def.row_exec(ctx, inst.params.data());
            if (!_store.is_row_alive(h.row)) {   // 模块杀行（KillAge 等）
                died = true;
                break;
            }
        }
        if (died) continue;

        pos[row].w += dt;
        if (pos[row].w >= vel[row].w) {
            _store.free(h);
        }
    }
}

// ---- 系统级 API ------------------------------------------------------------

bool initialize(const system_config& config) {
    if (g_initialized) return true;
    if (config.max_emitters == 0 || config.event_capacity == 0) return false;

    g_config = config;
    g_bus = std::make_unique<event_bus>();
    if (!g_bus->initialize(config.event_capacity)) {
        g_bus.reset();
        return false;
    }
    g_emitters.clear();
    g_next_emitter_id = 1;
    g_initialized = true;
    PRIMAL_LOG_STREAM(Info, "ParticleFX") << "initialized (event_capacity=" << config.event_capacity << ")"
                                          << std::endl;
    return true;
}

void shutdown() {
    if (!g_initialized) return;
    g_emitters.clear();
    g_bus.reset();
    g_initialized = false;
}

bool is_initialized() { return g_initialized; }

void tick(f32 dt) {
    // 幂等 ensure：生产宿主（PipelineTick）无需关心初始化时序。
    if (!g_initialized) {
        if (!initialize({})) return;
    }
    for (const auto& kv : g_emitters) {
        if (kv.second) kv.second->tick(dt);
    }
}

emitter_id emitter_create(const emitter_init& init) {
    if (!g_initialized) {
        if (!initialize({})) return invalid_emitter;
    }
    if (g_emitters.size() >= g_config.max_emitters) return invalid_emitter;
    if (init.capacity == 0) return invalid_emitter;

    const emitter_id id = g_next_emitter_id++;
    g_emitters.emplace(id, std::make_unique<pfx_emitter>(id, init));
    return id;
}

emitter_id emitter_create_from_program(const emitter_program& program) {
    if (!g_initialized) {
        if (!initialize({})) return invalid_emitter;
    }
    if (g_emitters.size() >= g_config.max_emitters) return invalid_emitter;
    if (program.capacity == 0) return invalid_emitter;

    const emitter_id id = g_next_emitter_id++;
    g_emitters.emplace(id, std::make_unique<pfx_emitter>(id, program));
    return id;
}

emitter_id emitter_create_from_asset(const char* json_path, std::string* err) {
    emitter_program program;
    std::string local_err;
    if (!load_emitter_asset(json_path, program, local_err)) {
        if (err) *err = local_err;
        PRIMAL_LOG_STREAM(Error, "ParticleFX") << "asset load failed: " << local_err << std::endl;
        return invalid_emitter;
    }
    const emitter_id id = emitter_create_from_program(program);
    if (err && id == invalid_emitter) *err = "emitter_create_from_program failed (limit reached?)";
    return id;
}

bool emitter_reload_from_memory(emitter_id id, const char* json_text, u32 length, std::string* err) {
    pfx_emitter* em = emitter_get(id);
    if (!em || !json_text || length == 0) return false;
    emitter_program program;
    std::string local_err;
    if (!compile_emitter_from_text(json_text, length, program, local_err)) {
        if (err) *err = local_err;
        return false;   // 原状保留
    }
    return em->rebind_program(program);
}

bool emitter_set_exposed_param(emitter_id id, u32 name_hash, f32 value) {
    pfx_emitter* em = emitter_get(id);
    if (!em || !em->has_program()) return false;
    const emitter_program& prog = em->program();
    for (size_t i = 0; i < prog.exposed.size(); ++i) {
        if (prog.exposed[i].name_hash == name_hash) {
            em->set_exposed_value(i, value);
            return true;
        }
    }
    return false;
}

bool emitter_get_exposed_param(emitter_id id, u32 name_hash, f32& out_value) {
    pfx_emitter* em = emitter_get(id);
    if (!em || !em->has_program()) return false;
    const emitter_program& prog = em->program();
    for (size_t i = 0; i < prog.exposed.size(); ++i) {
        if (prog.exposed[i].name_hash == name_hash) {
            out_value = em->exposed_value(i);
            return true;
        }
    }
    return false;
}

std::string emitter_schema_json() { return build_schema_json(); }

void set_global_lod(f32 factor) { g_global_lod = factor < 0.f ? 0.f : (factor > 1.f ? 1.f : factor); }
f32 get_global_lod() { return g_global_lod; }

void emitter_destroy(emitter_id id) { g_emitters.erase(id); }

pfx_emitter* emitter_get(emitter_id id) {
    const auto it = g_emitters.find(id);
    return it == g_emitters.end() ? nullptr : it->second.get();
}

u32 emitter_count() { return static_cast<u32>(g_emitters.size()); }

void emitter_set_position(emitter_id id, const math::v3& position) {
    if (pfx_emitter* em = emitter_get(id)) em->set_position(position);
}

void emitter_set_editor_preview(emitter_id id, bool preview) {
    if (pfx_emitter* em = emitter_get(id)) em->set_editor_preview(preview);
}

u32 drain_events(pfx_event* out, u32 cap) { return g_bus ? g_bus->drain(out, cap) : 0; }

void emit_event(const pfx_event& e) {
    if (g_bus) g_bus->push(e);
}

u32 pending_events() { return g_bus ? g_bus->pending() : 0; }

void for_each_emitter(const std::function<void(pfx_emitter&)>& fn) {
    if (!fn) return;
    for (auto& [id, em] : g_emitters) {
        if (em) fn(*em);
    }
}

#if !defined(DISABLE_PARTICLE_SYSTEM)

// ---- 渲染兼容桥 v2（§7-N2 前置）------------------------------------------
// 全部 PFX 发射器的活粒子 → 按混合模式分桶的紧凑 particle_data 缓冲。
// ParticlePass 每桶一次 draw（4 条混合管线），逐桶 push constants 在 Metal
// 上精确；标准通道与 particle_data 逐字段一致（TestParticleFX 锁定）。

const primal::particles::particle_data* packed_frame_pool(
    u32 frame_index, u32* out_count, u32 blend_counts_out[4])
{
    constexpr u32 kBridgeFrames = 3;   // = rhi::MAX_FRAMES_IN_FLIGHT（避免引 RHI 头）
    static std::vector<primal::particles::particle_data> packed[kBridgeFrames];
    static u32 packed_blend[kBridgeFrames][4] = {};
    if (frame_index >= kBridgeFrames || !out_count) return nullptr;

    auto& buf = packed[frame_index];
    buf.clear();
    for (u32 b = 0; b < 4; ++b) packed_blend[frame_index][b] = 0;

    static std::vector<primal::particles::particle_data> buckets[4];
    for (auto& b : buckets) b.clear();

    for_each_emitter([&](pfx_emitter& em) {
        if (em.editor_preview()) return;                                        // 编辑器私有预览不进世界场景
        if (em.has_program() &&
            em.program().renderer_type == emitter_program::renderer_mesh) return;   // mesh 型走 ParticleFXMeshPass
        auto& store = em.store();
        const u32 capacity = store.capacity();
        if (store.alive_count() == 0) return;
        const math::v4* pos = store.v4_channel(attr::position);
        const math::v4* vel = store.v4_channel(attr::velocity);
        const math::v4* col = store.v4_channel(attr::color);
        const math::v4* scr = store.v4_channel(attr::scale_rotation);
        const math::v4* uvp = store.v4_channel(attr::uv_params);
        if (!pos || !vel || !col || !scr || !uvp) return;

        const u32 blend = em.has_program()
            ? std::min<u32>(em.program().blend, 3u)
            : 0u;
        for (u32 row = 0; row < capacity; ++row) {
            if (!store.is_row_alive(row)) continue;
            primal::particles::particle_data p;
            p.position = pos[row];
            p.velocity = vel[row];
            p.color = col[row];
            p.scale_rotation = scr[row];
            p.uv_params = uvp[row];
            buckets[blend].push_back(p);
        }
    });

    for (u32 b = 0; b < 4; ++b) {
        blend_counts_out[b] = (u32)buckets[b].size();
        packed_blend[frame_index][b] = (u32)buckets[b].size();
        buf.insert(buf.end(), buckets[b].begin(), buckets[b].end());
    }
    *out_count = (u32)buf.size();
    return buf.empty() ? nullptr : buf.data();
}

#endif // !DISABLE_PARTICLE_SYSTEM

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
