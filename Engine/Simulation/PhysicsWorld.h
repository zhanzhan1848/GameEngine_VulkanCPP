#pragma once

// PhysicsWorld.h — simulation 门面实现（全局单例状态住在这里）。
// 头文件只为单测/工具暴露少量实现细节，常规消费方走 SimulationAPI.h 的自由函数。

#include "SimulationAPI.h"

namespace primal::simulation {

// 供 L0 单测直接覆盖的纯逻辑实现在 SimulationAPI.h 内联/编译单元中：
// fixed_step_accumulator::advance 的实现放在 PhysicsWorld.cpp（引擎内），单测通过链接 Engine 复用。

} // namespace primal::simulation
