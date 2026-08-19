// RHIShaderCommon.glsl — Phase 0.5 port of RHIShaderCommon.metal.
// Aggregate header — pulls in the entire RHI shader library in the same
// order as the Metal original so consumers only need a single #include.
//
// Order matters: constants first (no deps), then types (no deps), then
// functions (depend on constants). PBR functions are pulled in via
// RHIShaderFunctions.glsl's RHI_ENABLE_PBR gate.
//
// Reference: Engine/Graphics/RHI/Shaders/RHIShaderCommon.metal:1-20.

#ifndef RHIS_SHADER_COMMON_GLSL
#define RHIS_SHADER_COMMON_GLSL

// ============================================================================
// P4c-F6 布局约定:SetComputeBytes >128B 隐式 UBO 回退
//
// VulkanCommandBuffer::SetComputeBytes(index, bytes, size) 在
// index*16 + size > maxPushConstantsSize(通常 128B)时自动回退到设备级
// 隐式 UBO,shader 侧读取方式:
//
//     layout(set = 3, binding = 0) uniform <BlockName> { ... };
//
// 规则:
//   - set 3 为引擎保留(VulkanPipelineLayout 自动追加隐式 set layout,
//     用户 shader 声明的 set 从 0 开始、不得超过 2)
//   - 隐式 UBO 的内容是 SetComputeBytes 提交的完整 bytes blob
//     (不分 index*16 槽;shader 按自己的 struct 布局解释)
//   - ≤128B 路径继续走 push constant(offset = index*16),行为不变
//   - Metal 侧行为不变(始终 setBytes)
// ============================================================================

// 1. 常量定义
#include "RHIShaderConstants.glsl"

// 2. 数据结构定义
#include "RHIShaderTypes.glsl"

// 3. 通用函数库 (依赖常量)
#include "RHIShaderFunctions.glsl"

#endif // RHIS_SHADER_COMMON_GLSL
