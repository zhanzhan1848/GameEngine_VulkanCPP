#pragma once

// PfxSystem.h — PFX 系统层（架构文档 §3）。
// 双模式发射器：
//   N0 直接 spawn 模式 —— 服务测试与兼容桥验证；
//   N1 程序模式 —— emitter_program（JSON 编译产物）的阶段化模块堆栈驱动。
// 模块堆栈语义见 §3.2；事件消费者（RigidBodySpawner 等）N3 接入。

#include "../PfxTypes.h"
#include "AttributeStore.h"
#include "EventBus.h"
#include "EmitterProgram.h"
#if !defined(DISABLE_PARTICLE_SYSTEM)
#include "Particles/ParticleTypes.h"   // 渲染兼容桥：particle_data
#endif

#include <functional>
#include <string>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx {

struct system_config {
    u32 max_emitters{ 64 };       // 上限只约束并发存活数；id 单调递增不复用
    u32 event_capacity{ 4096 };   // 系统级事件环形缓冲（单帧消费）
};

// N0 直接 spawn 模式的发射器初始化。user_channels 透传给 attribute_store。
struct emitter_init {
    u32 capacity{ 1024 };
    u32 seed{ 0 };   // 确定性 RNG 域（架构 §3.2）
    u32 user_channel_count{ 0 };
    const attr_desc* user_channels{ nullptr };
};

class pfx_emitter {
public:
    pfx_emitter(emitter_id id, const emitter_init& init);
    pfx_emitter(emitter_id id, const emitter_program& program);   // N1 程序模式（持有副本）

    emitter_id get_id() const { return _id; }
    u32 alive_count() const { return _store.alive_count(); }
    u32 capacity() const { return _store.capacity(); }
    f32 elapsed() const { return _elapsed; }
    bool has_program() const { return _has_program; }
    const emitter_program& program() const { return _program; }

    attribute_store& store() { return _store; }
    const attribute_store& store() const { return _store; }

    // N0 直接 spawn：写标准通道 + birth 事件。池满返回无效句柄。
    particle_handle spawn(math::v3 position, math::v3 velocity, math::v4 color, math::v2 scale, f32 lifetime);

    // 热重载（§6 PfxEmitterReloadFromMemory 语义）：同一 emitter id 重绑程序并重启
    // （清空现有粒子与模块状态）。失败返回 false，原状保留。
    bool rebind_program(const emitter_program& program);

    // age 推进 + 到期回收（到期即 free ⇒ death 事件经 store 发往系统总线）。
    void tick(f32 dt);

    pcg32& rng() { return _rng; }

    // 发射器世界位置：init 阶段结束后叠加到新粒子（跟随宿主实体，渲染桥/同步回填）。
    // v1 仅平移（旋转让随 N 系列后续批次）。
    void set_position(const math::v3& position) { _position = position; }
    const math::v3& position() const { return _position; }

    // 编辑器私有预览标记：带标记的发射器不进世界场景渲染（渲染桥 / mesh /
    // light 收集路径跳过）。PFX 编辑器的私有预览发射器与场景实体无关，
    // 泄漏进主视口会表现为"场景原点凭空出现粒子发射器"。
    void set_editor_preview(bool p) { _editor_preview = p; }
    bool editor_preview() const { return _editor_preview; }

    // 暴露参数值（按编译期名册下标；调用方先查 program().exposed）
    void set_exposed_value(size_t index, f32 value) {
        if (index < _exposed_values.size()) _exposed_values[index] = value;
    }
    f32 exposed_value(size_t index) const {
        return index < _exposed_values.size() ? _exposed_values[index] : 0.f;
    }

private:
    void tick_legacy(f32 dt);
    void tick_program(f32 dt);

    emitter_id      _id{ invalid_emitter };
    attribute_store _store;
    pcg32           _rng;
    f32             _elapsed{ 0.f };
    math::v3        _position{ 0.f, 0.f, 0.f };

    // 程序模式状态
    bool               _has_program{ false };
    emitter_program    _program;
    std::vector<u8>    _module_state;      // 每实例状态块（零初始化）
    std::vector<f32>   _exposed_values;    // 运行期暴露参数值
    f32                _spawn_accum{ 0.f };
    bool               _editor_preview{ false };
};

// ---- 系统级 API（模式沿 primal::particles 的自由函数门面）------------------

bool initialize(const system_config& config = {});
void shutdown();
bool is_initialized();

// 幂等 ensure（沿 ParticleEnsureSystems 先例）：未初始化时以默认配置初始化。
// 生产接线：PipelineTick（EngineDLL/PhysicsAPI.cpp）每帧调用，渲染帧之前。
void tick(f32 dt);

emitter_id    emitter_create(const emitter_init& init);
emitter_id    emitter_create_from_program(const emitter_program& program);
emitter_id    emitter_create_from_asset(const char* json_path, std::string* err = nullptr);
void          emitter_destroy(emitter_id id);   // 未知 id 安全无操作
pfx_emitter*  emitter_get(emitter_id id);       // 未知 id 返回 nullptr
u32           emitter_count();
void          emitter_set_position(emitter_id id, const math::v3& position);   // 实体同步回填
void          emitter_set_editor_preview(emitter_id id, bool preview);          // 编辑器私有预览标记

// 热重载：同 id 重绑程序并重启（失败返回 false，原状保留）。
bool emitter_reload_from_memory(emitter_id id, const char* json_text, u32 length, std::string* err = nullptr);

// 暴露参数（§6 PfxSetExposedParam 语义；未知名返回 false）。
bool emitter_set_exposed_param(emitter_id id, u32 name_hash, f32 value);
bool emitter_get_exposed_param(emitter_id id, u32 name_hash, f32& out_value);

// schema 反射（§6 PfxGetEmitterSchema 内容；编辑器表单与 CLI 校验的单一事实源）。
std::string emitter_schema_json();

// 全局 LOD 因子（N6）：宿主按相机距离/重要性设置 [0,1]，作用于 spawn 数量。
// v1 为系统级因子（所有发射器）；按发射器距离的逐发射器 LOD 随场景集成落地。
void set_global_lod(f32 factor);
f32  get_global_lod();

// 消费者单帧入口（ADR4）：取出全部挂起事件（FIFO），返回实际取出数。
u32 drain_events(pfx_event* out, u32 cap);
u32 pending_events();

// 遍历全部存活发射器（渲染桥 / 统计）。单线程帧管线语义。
void for_each_emitter(const std::function<void(pfx_emitter&)>& fn);

#if !defined(DISABLE_PARTICLE_SYSTEM)
// 渲染兼容桥 v2（§7-N2 前置）：把全部 PFX 发射器的活粒子打包成按混合模式
// 分桶的紧凑 particle_data 缓冲（标准通道与 particle_data 逐字段一致，
// TestParticleFX 锁定）。ParticlePass 逐桶 draw（4 条混合管线就绪），
// 不再共享 legacy 帧池（解除 10k 容量耦合与自由表索引纠纷）。
// 须在 pfx::tick 之后、渲染之前由渲染端调用。
const primal::particles::particle_data* packed_frame_pool(
    u32 frame_index, u32* out_count, u32 blend_counts_out[4]);
#endif

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
