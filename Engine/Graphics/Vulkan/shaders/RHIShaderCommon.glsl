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

// 1. 常量定义
#include "RHIShaderConstants.glsl"

// 2. 数据结构定义
#include "RHIShaderTypes.glsl"

// 3. 通用函数库 (依赖常量)
#include "RHIShaderFunctions.glsl"

#endif // RHIS_SHADER_COMMON_GLSL
