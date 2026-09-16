#pragma once

// EmitterProgram.h — 发射器程序：JSON 资产 → 编译产物（架构文档 §3.2）。
// 编译期校验（未知模块/缺字段/类型错）带 "path (line L)" 报错；运行期零 JSON 依赖。

#include "ModuleRegistry.h"

#include <string>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx {

struct user_attr {
    u32 name_hash{ 0 };
    u32 stride{ 16 };
};

struct exposed_param {
    u32 name_hash{ 0 };
    f32 default_value{ 0.f };
};

struct module_instance {
    u32 def_index{ 0 };              // registry->all() 下标（def 生命周期 = 进程级）
    u32 state_offset{ 0 };           // 发射器状态缓冲内偏移
    std::vector<u8> params;          // POD blob，尺寸 = def.params_size
};

// 发射源网格（ShapeLocation mesh_surface）：模块参数里直写 OBJ 路径，
// 编译期经编译环境在此表按路径去重落位（与 fields 场表同模式、免文档段）。
struct mesh_entry {
    std::string path;
    std::shared_ptr<mesh_geometry> mesh;
};

// 逐粒子刚体绑定声明（路线图 P2：JSON instance.physics）。rigid_body=true 时
// 出生粒子建 Jolt 动态体、物理持有变换权威；与 update 阶段 PhysicsCollision
// 模块互斥（编译期拒绝——刚体自带碰撞，双开语义冲突）。
struct instance_physics_settings {
    bool rigid_body{ false };
    u8  collider{ 0 };          // 0 sphere（radius 为半径）| 1 convex（P3：instance.mesh 顶点凸包）
    bool self_collision{ true }; // true=kDynamic(互相排开堆叠) / false=kParticle(只碰世界,互相穿透)
    f32 radius{ 0.05f };
    f32 mass{ 1.f };
    f32 restitution{ 0.2f };
    f32 friction{ 0.5f };
    u32 max_bodies{ 256 };
};

struct emitter_program {
    std::string name;
    u32 asset_version{ 1 };
    u32 capacity{ 1024 };
    u32 seed{ 0 };
    math::v3 source_velocity{};      // 继承源速度（N1 常量；N3 由组件回填）
    std::vector<user_attr> user_attrs;
    std::vector<exposed_param> exposed;
    // 场表（N4）：JSON "fields":[{"name":"sdf","file":"x.pgrid"}]，模块按名引用
    struct field_entry {
        std::string name;
        std::shared_ptr<volume_grid> grid;
    };
    std::vector<field_entry> fields;
    std::vector<std::shared_ptr<volume_grid>> field_grids;   // 与 fields 平行（sim_context 视图）
    std::vector<mesh_entry> meshes;                          // 按路径去重（编译期填充）
    std::vector<std::shared_ptr<mesh_geometry>> mesh_views;  // 与 meshes 平行（sim_context 视图）
    instance_physics_settings instance_physics;              // instance.physics 声明
    std::vector<module_instance> stages[3];   // [stage_kind]
    u32 state_bytes{ 0 };            // 全部实例状态块总大小

    // 混合模式（N6 桥接：sprite 兼容桥按此选 ParticlePass 管线）
    // 0 additive / 1 alpha / 2 multiply / 3 premultiplied（与 legacy blend_mode 对齐）
    u8 blend{ 0 };

    // 渲染器类型（§7-N2）：0 billboard（默认，走 sprite 兼容桥）/
    // 1 mesh（走 ParticleFXMeshPass 立方体实例）。JSON "renderer" 字符串。
    u8 renderer_type{ 0 };
    static constexpr u8 renderer_billboard{ 0 };
    static constexpr u8 renderer_mesh{ 1 };

    // 渲染 instance 几何源：JSON "instance":{"mesh":"<cooked .mesh 路径>"}。
    // 空 = 内置立方体回退；运行期由渲染桥按路径懒加载换几何槽（零编译期 IO）。
    std::string instance_mesh_path;
};

// 编译 JSON 根对象 → 程序。失败返回 false 并写 err（含路径与行号）。
bool compile_emitter(const json::value& root, emitter_program& out, std::string& err);

// 从文本解析 + 编译。
bool compile_emitter_from_text(const char* text, size_t length, emitter_program& out, std::string& err);

// 从资产文件加载 + 编译。
bool load_emitter_asset(const char* path, emitter_program& out, std::string& err);

// schema 反射 JSON（§6 PfxGetEmitterSchema 内容；编辑器表单与 CLI 校验的单一事实源）。
std::string build_schema_json();

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
