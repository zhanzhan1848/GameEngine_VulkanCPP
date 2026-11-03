/**
 * @file D3D12StateMapping.h
 * @brief RHI ResourceState / PipelineStage / AccessFlag -> D3D12 等价翻译
 * @details D3D12 RHI 现代化 M0（F0-6）。平台中立，无 Windows 头。
 *
 *          - ResourceState 布局态（General..IndirectArgument，值 8-19）→
 *            D3D12_RESOURCE_STATES 位（D3D12Abi.h）。生命周期态
 *            （Unknown..Destroyed，值 0-7）无 D3D12 等价物，Translate 返回
 *            false 显式拒绝（调用方不得静默当 COMMON 处理）。
 *          - PipelineStage / AccessFlag → 经典 D3D12 资源状态位域等价
 *            （ToResourceStatesBits）：这是经典 ResourceBarrier 世界里
 *            "stage/access 组合意味着什么状态" 的翻译，UAV barrier 的
 *            写检测与 rhi-barrier-semantics.md §1 的跨端约定都以此表达。
 *
 *          TODO(M5+, Enhanced Barriers)：D3D12_BARRIER_SYNC/ACCESS 的位值
 *          在公开文档页未列数值，D3D12Abi.h 刻意未手定义；引入 Enhanced
 *          Barriers 时随 PRIMAL_D3D12_ABI_CHECKS 从真实 d3d12.h 锁值后再
 *          提供第二张翻译表（stage→SYNC、access→ACCESS）。
 */

#pragma once

#include "../../Core/RHITypes.h"
#include "../../Core/RHICommand.h"
#include "D3D12Abi.h"

namespace primal::graphics::rhi::d3d12 {

/// RHI 布局态 -> D3D12_RESOURCE_STATES 位。
/// 生命周期态（Unknown..Destroyed）返回 false 且 *bits 未定义。
bool ToD3D12ResourceStates(ResourceState state, u32& bits);

/// 便捷版：调用方已确认是布局态（失败返回 kD3D12ResourceState_Common，
/// 仅用于诊断打印路径，不用于构造 barrier）。
u32 ToD3D12ResourceStatesOrCommon(ResourceState state);

/// D3D12_RESOURCE_STATES 位 -> 最接近的 RHI 布局态（反向诊断用；
/// 组合位只认本文件产出的规范组合，未知组合返回 ResourceState::Unknown）。
ResourceState FromD3D12ResourceStates(u32 bits);

/// PipelineStage 组合 -> 经典 D3D12 资源状态位域等价（该执行阶段
/// 消费资源时资源应处的状态位）。翻译域是 RESOURCE_STATES 而非
/// Enhanced-Barriers SYNC；见文件头 TODO。
u32 PipelineStageToResourceStatesBits(PipelineStage stage);

/// AccessFlag 组合 -> 经典 D3D12 资源状态位域等价。
/// 注意：ShaderWrite 单独出现按 UNORDERED_ACCESS 处理（UAV 写），
/// ShaderRead 单独出现按 AllShaderResource 处理（SRV 读）。
u32 AccessFlagsToResourceStatesBits(AccessFlag access);

/// RHI 布局态是否属于"读类"——精确等于 D3D12 COMMON 自动提升（promotion）
/// 的目标集合：SRV 读 / 复制源 / 间接参数。注意 DepthStencilReadOnly 与
/// ResolveSource 语义上是只读使用，但 D3D12 规范不允许从 COMMON 直接提升
/// （显式 transition 必发），故不在本集合内。
bool IsReadClassState(ResourceState state);

/// F2-3（D3D12 M2）：ComparisonFunc -> D3D12_COMPARISON_FUNC 常量
/// （值取 D3D12Abi.h，平台中立可单测；Windows 侧 PRIMAL_D3D12_ABI_CHECKS
/// 已把常量锁到真实 d3d12.h）。
u32 ToD3D12ComparisonFunc(ComparisonFunc func);

/// F2-3（M2）：该 desc 是否应创建为"比较采样器"。
/// 谓词与 VulkanSampler 完全对齐（VulkanSampler.cpp:
/// compareEnable = (comparisonFunc != Never)）：
///   - Never     → 普通采样（Vulkan compareEnable=FALSE 的镜像）
///   - 其余一切（含 RHI 默认 Always）→ 比较采样器
/// 注意这是 M2 的行为修正：M1 版谓词是 "!= Always && != Unknown"，导致
/// Never 在 D3D12 变比较采样器而 Vulkan 是普通采样器（ShadowFilter 的
/// linear sampler 即 Never 值——两端口径必须一致）。RHI 默认值
/// SamplerDesc::comparisonFunc == ComparisonFunc::Always 的"坑"在两端
/// 表现一致（都是比较采样器，采样恒 1），跨端等价优先于单端宽容；
/// 调用方（如 M2 宿主 M2ShadowSampler）必须显式传 LessEqual。
bool SamplerDescWantsComparison(const SamplerDesc& desc);

/// F2-3（M2）：SamplerDesc 过滤档 -> D3D12_FILTER 常量。min/mag/mip
/// 点/线性组合三档 + 各向异性档；比较谓词满足时切 COMPARISON_* 变体
/// （各向异性保留 COMPARISON_ANISOTROPIC，不静默降档）。
u32 SelectD3D12SamplerFilter(const SamplerDesc& desc);

} // namespace primal::graphics::rhi::d3d12
