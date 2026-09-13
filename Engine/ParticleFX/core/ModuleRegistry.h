#pragma once

// ModuleRegistry.h — PFX 模块注册表（架构文档 §3.2，Niagara 阶段堆栈的原生 C++ 对应物）。
// 模块 = 静态注册的 (名字, 阶段, 参数 POD, 解析器, 执行器)。
// 参数解析错误经 std::string 返回（无异常纪律），带行号由编译器拼接。

#include "../PfxTypes.h"
#include "AttributeStore.h"
#include "FieldSystem.h"
#include "PfxJson.h"

#include <memory>
#include <vector>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx {

struct exposed_param;   // EmitterProgram.h（此处仅指针形参，前置声明足够）

enum class stage_kind : u8 { spawn = 0, init = 1, update = 2 };

inline const char* stage_name(stage_kind s) {
    static const char* names[3] = { "spawn", "init", "update" };
    return names[(u8)s];
}

// 模块执行上下文：执行器每帧/每行填充。
struct sim_context {
    attribute_store* store{ nullptr };
    pcg32* rng{ nullptr };
    const f32* exposed{ nullptr };      // 暴露参数值表（param_ref 解析用）
    void* state{ nullptr };             // 当前模块实例的运行期状态块（每实例独立，零初始化）
    f32* spawn_accum{ nullptr };        // 发射器级 spawn 累积器（spawn 阶段读写）
    f32 dt{ 0.f };
    f32 elapsed{ 0.f };
    f32 lod_factor{ 1.f };              // N6：宿主按距离/重要性注入的 spawn 缩放
    emitter_id source{ invalid_emitter };
    math::v3 source_velocity{};         // 继承源速度（N3 起由组件回填；N1 为程序常量）
    math::v3 last_normal{ 0.f, 0.f, 1.f }; // init 模块契约：ShapeLocation 写、AddVelocity(normal) 读
    const std::vector<std::shared_ptr<volume_grid>>* fields{ nullptr }; // 发射器场表（field_id 索引）
    u32 row{ u32_invalid_id };          // init/update 阶段：当前行
    particle_handle handle{};
};

// 可绑定暴露参数的 f32：override_idx 无效时用 value。
struct param_ref {
    u32 override_idx{ u32_invalid_id };
    f32 value{ 0.f };
};

inline f32 resolve_param(const param_ref& ref, const sim_context& ctx) {
    return ref.override_idx != u32_invalid_id ? ctx.exposed[ref.override_idx] : ref.value;
}

// spawn 阶段：返回本帧贡献的生成数。
using spawn_exec_fn = u32 (*)(sim_context& ctx, const void* params);
// init/update 阶段：作用于 ctx.row 一行。
using row_exec_fn = void (*)(sim_context& ctx, const void* params);
// 参数解析：从模块 JSON 对象（扁平键值）读入 POD。失败写 err。
using param_parse_fn = bool (*)(const json::value& module_obj, void* out_params, std::string& err);

struct param_hint {
    const char* name;
    const char* type;    // "f32"|"u32"|"v3"|"v2"|"enum"|"bool"|"param_ref"
    const char* note;    // 可为 ""
};

struct module_def {
    const char* name{ nullptr };
    u32 name_hash{ 0 };              // 注册表统一按 hash_name(name) 填写/查找
    stage_kind stage{ stage_kind::update };
    size_t params_size{ 0 };
    size_t state_size{ 0 };          // 每实例运行期状态字节数（零初始化，随发射器存活）
    // 参数默认值构造（placement new T{}；编译器在 parse 之前调用——
    // params blob 是零填充的，POD 默认成员值必须经此恢复）
    void (*init_defaults)(void* out){ nullptr };
    spawn_exec_fn spawn_exec{ nullptr };
    row_exec_fn row_exec{ nullptr };
    param_parse_fn parse{ nullptr };
    const param_hint* hints{ nullptr };
    u32 hint_count{ 0 };
};

class module_registry {
public:
    static module_registry& get();

    // 重复名注册覆盖（测试可重入）。
    void register_module(module_def def);
    const module_def* find(u32 name_hash) const;
    const std::vector<module_def>& all() const;

private:
    std::vector<module_def> defs_;
};

// 注册全部内置模块（幂等；在编译器首次使用前由 system ensure 调用）。
void register_builtin_modules();

// N4 场模块（KillVolume/CurlNoiseForce/AdvectByField/SDFCollision）。
// 由 register_builtin_modules 内部调用；单独导出仅供测试。
void register_field_modules();

// 编译期场名册（编译器解析 "fields" 声明后设置；模块 "field" 名 → 下标绑定）。
void set_compile_env_fields(const std::vector<std::string>* names);

// 编译期环境：暴露参数名册。编译器在解析 stages 前设置，结束后置 nullptr
// （模块 param_ref 的 {"param":"Name"} 绑定按名查下标）。
void set_compile_env_exposed(std::vector<exposed_param>* exposed);

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
