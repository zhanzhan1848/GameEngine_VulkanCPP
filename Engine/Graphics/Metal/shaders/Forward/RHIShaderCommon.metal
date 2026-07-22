#pragma once

#include <metal_stdlib>
using namespace metal;

/**
 * @file RHIShaderCommon.metal
 * @brief 通用着色器聚合头文件
 * @details 引入所有模块化的着色器组件
 */

// 1. 常量定义
#include "RHIShaderConstants.metal"

// 2. 数据结构定义
#include "RHIShaderTypes.metal"

// 3. 通用函数库 (依赖常量)
#include "RHIShaderFunctions.metal"
