/**
 * @file D3D12StateTracker.h
 * @brief D3D12 资源状态跟踪器（平台中立，无 Windows 头，不依赖 Device）
 * @details D3D12 RHI 现代化 M0（F0-7）。
 *
 *          输入 = RHI ResourceState + 子资源坐标；输出 = 有序的
 *          D3D12ResourceBarrierOp 列表（Transition / UAV / Aliasing），
 *          由 Windows 层（M1+ CommandBuffer）翻译成真实
 *          D3D12_RESOURCE_BARRIER 数组下发。
 *
 *          实现的 D3D12 规范规则（Implicit State Transitions）：
 *            ① 纹理按子资源（>= per-mip 粒度，D3D12 subresource 索引）
 *               独立跟踪；buffer 单一状态（D3D12 buffer 无子资源概念）。
 *            ② COMMON 自动提升（promotion）：buffer 可从 COMMON 提升到
 *               任意状态；纹理只能提升到读类（SRV/CopySource/IndirectArg，
 *               见 D3D12StateMapping::IsReadClassState）。提升零 barrier；
 *               写类目标（RT/DS/CopyDest/UAV/ResolveDest...）必须显式
 *               transition。simultaneous-access 纹理的"提升到任意状态"
 *               扩展需要 Windows 层的创建 flag，M0 留 TODO。
 *            ③ 提交边界衰减（decay）：ExecuteCommandLists 完成后
 *               buffer 全部衰减回 COMMON；纹理仅当状态来自提升（而非显式
 *               transition）时衰减。OnSubmitBoundary() 返回需衰减的集合
 *               并更新内部状态；初版 decayOnSubmit 开关（默认开）。
 *            ④ 同状态 UnorderedAccess -> UnorderedAccess（compute->compute
 *               WAW）必发 UAV barrier（不改变状态）。
 *            ⑤ 相同状态的 transition 请求（除 ④ 的 UAV 特例）不产出 op。
 *
 *          跨队列所有权（acquire/release）：初版单 DIRECT 队列，接口留空
 *          实现 + TODO（见 AcquireOwnership/ReleaseOwnership）。
 *
 *          生命周期以 fence 值为据的回收判定属 Windows 层（D3D12Fence），
 *          跟踪器不做帧数算术。
 */

#pragma once

#include <cstdint>
#include "../../Core/RHITypes.h"
#include "D3D12Abi.h"
#include "D3D12StateMapping.h"

namespace primal::graphics::rhi::d3d12 {

/// 子资源坐标。resource 是调用方分配的稳定 id（如物理句柄数值）；
/// buffer 用 isTexture=false（subresource 恒 0 / kAllSubresources 等价）；
/// 纹理 subresource 为 D3D12 线性子资源索引
/// （mip + levelCount * (arraySlice + arraySize * planeSlice)），
/// 或 kD3D12ResourceBarrierAllSubresources 表示整资源。
///
/// R16-A 键空间契约：条目键 = (resource 数值, isTexture) 二元组。RHI 的
/// buffer/texture 分配器是两个独立 free_list，各自从 0 起编号——同数值
/// 句柄必然跨类型重叠（M0 初版仅按 resource 数值查键：后注册方被"双注册
/// 防御"静默拒绝、其全部 Transition 返回空 op——Windows 十五测 #538
/// INVALID_SUBRESOURCE_STATE 的根因链，见 runbook §25-A）。
struct D3D12SubresourceKey {
    u64 resource = 0;
    u32 subresource = 0;
    bool isTexture = false;

    bool operator==(const D3D12SubresourceKey& o) const {
        return resource == o.resource && subresource == o.subresource
            && isTexture == o.isTexture;
    }
};

/// 跟踪器产出的 barrier 指令（平台中立的 op 记录；Windows 层翻译）。
struct D3D12ResourceBarrierOp {
    enum class Kind : u8 {
        Transition, ///< 状态转换（stateBefore->stateAfter）
        UAV,        ///< UAV 执行/内存依赖（WAW；无状态变化）
        Aliasing,   ///< 别名屏障（placed resource 间）
    };

    Kind kind = Kind::Transition;
    u64 resource = 0;                 ///< D3D12SubresourceKey.resource
    u32 stateBefore = 0;              ///< Transition: kD3D12ResourceState_* 位
    u32 stateAfter = 0;               ///< Transition: kD3D12ResourceState_* 位
    u32 subresource = 0;              ///< 具体索引或 kD3D12ResourceBarrierAllSubresources
    u64 aliasBefore = 0;              ///< Aliasing: 之前的资源 id（0=丢弃屏障）
    u64 aliasAfter = 0;               ///< Aliasing: 之后的资源 id
};

/// 提交边界衰减结果项。
struct D3D12DecayRecord {
    u64 resource = 0;
    u32 subresource = 0;              ///< 具体索引（衰减按子资源粒度回报）
};

class D3D12StateTracker {
public:
    struct Config {
        /// 提交边界是否执行 COMMON 衰减（D3D12 规范行为；开关供诊断对照）。
        bool decayOnSubmit = true;
        /// 是否启用 COMMON 自动提升（规范行为；关闭=全部显式 transition，供诊断）。
        bool promotionEnabled = true;
    };

    D3D12StateTracker() = default;
    explicit D3D12StateTracker(const Config& config) : config_(config) {}

    /// 注册资源几何。纹理传真实 mip/array/plane 数（决定整资源展开与
    /// 衰减集合）；buffer 三参传 1。initialState 为创建时的 D3D12 初始状态。
    /// 重复注册同一 (resource, isTexture)：断言冲突（防御双注册），返回
    /// false。同数值 resource 但不同 isTexture（buffer/texture 分池同号）：
    /// 合法共存，各自独立跟踪（R16-A 键空间契约，见 D3D12SubresourceKey 注）。
    bool RegisterResource(u64 resource, bool isTexture, u32 mipLevels,
                          u32 arraySize, u32 planeSlices,
                          ResourceState initialState = ResourceState::General);

    /// 注销资源（销毁路径）。isTexture 必须与注册时一致——同数值句柄的
    /// 另一类型条目不受影响（跨类型误注销会静默 orphan 对方，R16-A 前
    /// 的实际行为）。未注册该 (resource, isTexture) 返回 false。
    bool UnregisterResource(u64 resource, bool isTexture);

    /// 声明"该子资源当下处于 state"（外部事实注入：初始状态/别的路径改过）。
    /// 生命周期态被拒绝（返回 false）；未注册资源拒绝。
    bool Track(const D3D12SubresourceKey& key, ResourceState state);

    /// 请求把 key（可为整资源 ALL）转入 target，返回需下发的有序 op 列表
    /// 并更新内部跟踪状态。规则见文件头 ②④⑤；非法请求（未注册——含
    /// (resource, isTexture) 类型不匹配/生命周期态/子资源越界）返回空列表
    /// （显式失败，不静默）。
    utl::vector<D3D12ResourceBarrierOp> Transition(const D3D12SubresourceKey& key,
                                                   ResourceState target);

    /// 查询当前跟踪状态（诊断）。未跟踪返回 false。
    /// 注意：D3D12 中 Present 与 General 同为位值 0（COMMON 的同义词），
    /// QueryState 对两者都回报 General——这是 D3D12 语义，不是丢信息。
    bool QueryState(const D3D12SubresourceKey& key, ResourceState& out) const;

    /// 提交边界：按规则 ③ 计算衰减集合并更新内部状态；返回衰减记录
    /// （按资源注册顺序、子资源升序——确定性输出）。decayOnSubmit=false
    /// 时返回空且不改变状态。
    utl::vector<D3D12DecayRecord> OnSubmitBoundary();

    // ------------------------------------------------------------------------
    // 跨队列所有权（M0 单 DIRECT 队列：接口占位，显式不可用）
    // TODO(M4+/多队列): DIRECT/COMPUTE/COPY 间 acquire/release 需要把
    // queueFamily 记入子资源条目，且 COMMON 在跨队列边界成为强制中转态；
    // 当前实现直接返回 false（显式不支持），不产生任何 op。
    // ------------------------------------------------------------------------
    bool AcquireOwnership(const D3D12SubresourceKey& /*key*/, u32 /*afterQueue*/) {
        return false; // TODO: 跨队列所有权（单 DIRECT 队列初版）
    }
    bool ReleaseOwnership(const D3D12SubresourceKey& /*key*/, u32 /*fromQueue*/) {
        return false; // TODO: 跨队列所有权（单 DIRECT 队列初版）
    }

    /// 诊断：已注册资源数。
    size_t RegisteredResourceCount() const { return entries_.size(); }

private:
    struct SubEntry {
        u32 stateBits = kD3D12ResourceState_Common;
        bool promoted = false;    ///< 当前状态是否来自 COMMON 提升（decay 判据）
    };
    struct ResourceEntry {
        bool isTexture = false;
        u32 subresourceCount = 1; ///< mip*array*plane；buffer 恒 1
        utl::vector<SubEntry> subresources;  ///< 索引 0..count-1
    };

    /// D3D12 线性子资源索引工具（静态，供调用方对齐）。
    static u32 SubresourceIndex(u32 mip, u32 arraySlice, u32 planeSlice,
                                u32 mipLevels, u32 arraySize) {
        return mip + mipLevels * (arraySlice + arraySize * planeSlice);
    }

    SubEntry* ResolveSub(const D3D12SubresourceKey& key);
    const SubEntry* ResolveSub(const D3D12SubresourceKey& key) const;

    Config config_;
    utl::vector<std::pair<u64, ResourceEntry>> entries_;  ///< 插入序（确定性遍历）
};

} // namespace primal::graphics::rhi::d3d12
