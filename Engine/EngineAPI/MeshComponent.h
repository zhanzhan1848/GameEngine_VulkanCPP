// 文件说明: MeshComponent 的 API 声明，提供 ECS 中网格组件的轻量句柄与访问接口。
// 设计目标: 高性能、可扩展、可复用。组件本身只保存一个类型安全的 id，实际数据由 Components 层管理。

#pragma once

#include "Components/ComponentsCommon.h"

namespace primal::mesh {

    // 定义 Mesh 组件的类型安全 id
    DEFINE_TYPED_ID(mesh_id);

    // 类说明: Mesh 组件轻量句柄。仅保存 id，实际数据由 Components 层的存储管理。
    class component final
    {
    public:
        // 函数说明: 构造一个有效的组件句柄
        constexpr explicit component(mesh_id id) : _id{ id } {}
        // 函数说明: 构造一个无效的组件句柄
        constexpr component() : _id{ id::invalid_id } {}
        // 函数说明: 返回组件的类型安全 id
        [[nodiscard]] constexpr mesh_id get_id() const { return _id; }
        // 函数说明: 判断组件句柄是否有效
        [[nodiscard]] constexpr bool is_valid() const { return id::is_valid(_id); }

    private:
        mesh_id _id;
    };
}