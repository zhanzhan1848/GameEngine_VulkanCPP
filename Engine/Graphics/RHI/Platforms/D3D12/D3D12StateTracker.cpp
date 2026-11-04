/**
 * @file D3D12StateTracker.cpp
 * @brief D3D12 资源状态跟踪器实现（平台中立）
 * @details 规则表断言（>=30 条）见
 *          EngineTest/UnitTests/RHI/D3D12/TestD3D12StateTracker.cpp。
 */

#include "D3D12StateTracker.h"

#include <utility>

namespace primal::graphics::rhi::d3d12 {

bool D3D12StateTracker::RegisterResource(u64 resource, bool isTexture,
                                         u32 mipLevels, u32 arraySize,
                                         u32 planeSlices,
                                         ResourceState initialState) {
    // R16-A：键 = (resource, isTexture)。buffer/texture 分池同号句柄合法
    // 共存（各自独立跟踪）；仅同 id 同类型的重复注册才是双注册冲突。
    for (const auto& [id, entry] : entries_) {
        if (id == resource && entry.isTexture == isTexture) {
            return false;  // 双注册防御（显式失败）
        }
    }
    u32 initialBits = kD3D12ResourceState_Common;
    if (!ToD3D12ResourceStates(initialState, initialBits)) {
        return false;  // 生命周期态不能作为 D3D12 初始布局
    }
    ResourceEntry entry;
    entry.isTexture = isTexture;
    if (isTexture) {
        // D3D12 subresource 线性计数：mipLevels * arraySize * planeSlices
        const u64 count64 = u64(mipLevels ? mipLevels : 1)
            * (arraySize ? arraySize : 1) * (planeSlices ? planeSlices : 1);
        if (count64 > 0xFFFFu) return false;  // 防御荒谬几何
        entry.subresourceCount = u32(count64);
    } else {
        entry.subresourceCount = 1;
    }
    entry.subresources.resize(entry.subresourceCount);
    for (auto& sub : entry.subresources) {
        sub.stateBits = initialBits;
        sub.promoted = false;
    }
    entries_.emplace_back(resource, std::move(entry));
    return true;
}

bool D3D12StateTracker::UnregisterResource(u64 resource, bool isTexture) {
    // R16-A：注销也按 (resource, isTexture) 键——跨类型误注销（buffer 销毁
    // 带走同号纹理条目 / 反之）在 M0 键空间下会静默 orphan 对方，此后对方
    // 的全部 Transition 返回空 op（#538 家族的第二入口）。
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].first == resource
            && entries_[i].second.isTexture == isTexture) {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool D3D12StateTracker::Track(const D3D12SubresourceKey& key, ResourceState state) {
    u32 bits = 0;
    if (!ToD3D12ResourceStates(state, bits)) return false;
    SubEntry* sub = ResolveSub(key);
    if (!sub) return false;
    sub->stateBits = bits;
    sub->promoted = false;  // 外部事实注入按显式状态处理（不参与提升衰减）
    return true;
}

utl::vector<D3D12ResourceBarrierOp> D3D12StateTracker::Transition(
        const D3D12SubresourceKey& key, ResourceState target) {
    utl::vector<D3D12ResourceBarrierOp> ops;
    u32 targetBits = 0;
    if (!ToD3D12ResourceStates(target, targetBits)) return ops;  // 生命周期态拒绝

    ResourceEntry* entry = nullptr;
    // R16-A：按 (resource, isTexture) 查条目——同数值 id 的 buffer/texture
    // 条目共存时跳过类型不符的条目继续扫描（M0 初版在首个同 id 条目上做
    // 类型判定即返回空 op，后注册方因此永远拿不到 barrier）。
    for (auto& [id, e] : entries_) {
        if (id == key.resource && e.isTexture == key.isTexture) {
            entry = &e;
            break;
        }
    }
    if (!entry) return ops;  // 未注册/类型不匹配拒绝

    // 展开目标子资源集合（升序——确定性 op 序）
    const bool wholeResource = key.subresource == kD3D12ResourceBarrierAllSubresources;
    if (!wholeResource && key.subresource >= entry->subresourceCount) return ops;

    const u32 first = wholeResource ? 0u : key.subresource;
    const u32 last = wholeResource ? entry->subresourceCount - 1u : key.subresource;

    bool needsUav = false;                 // UAV barrier 资源级只发一条
    u32 uniformBefore = 0;                 // 整资源折叠判定
    bool uniform = true;
    u32 transitionCount = 0;
    utl::vector<D3D12ResourceBarrierOp> perSubOps;  // 混合状态时逐子资源

    for (u32 s = first; s <= last; ++s) {
        SubEntry& sub = entry->subresources[s];
        // 规则 ④：同状态 UAV -> UAV（compute WAW）必发 UAV barrier，状态不变。
        if (sub.stateBits == targetBits
            && targetBits == kD3D12ResourceState_UnorderedAccess) {
            needsUav = true;
            continue;
        }
        // 规则 ⑤：相同状态（非 UAV 特例）不产出。
        if (sub.stateBits == targetBits) continue;

        // 规则 ②：COMMON 自动提升（buffer 任意状态；纹理读类）。
        if (config_.promotionEnabled && sub.stateBits == kD3D12ResourceState_Common) {
            const bool promotable = !entry->isTexture || IsReadClassState(target);
            if (promotable) {
                sub.stateBits = targetBits;
                sub.promoted = true;
                continue;  // 零 barrier
            }
        }

        // 显式 transition（写类目标 / 非 COMMON 起点 / 提升关闭）。
        D3D12ResourceBarrierOp op;
        op.kind = D3D12ResourceBarrierOp::Kind::Transition;
        op.resource = key.resource;
        op.stateBefore = sub.stateBits;
        op.stateAfter = targetBits;
        op.subresource = s;
        perSubOps.push_back(op);
        if (transitionCount == 0) {
            uniformBefore = sub.stateBits;
        } else if (sub.stateBits != uniformBefore) {
            uniform = false;
        }
        ++transitionCount;
        sub.stateBits = targetBits;
        sub.promoted = false;  // 显式转换后的状态不衰减（规则 ③）
    }

    if (needsUav) {
        // D3D12 UAV barrier 无子资源字段——资源级一条。
        D3D12ResourceBarrierOp op;
        op.kind = D3D12ResourceBarrierOp::Kind::UAV;
        op.resource = key.resource;
        op.subresource = kD3D12ResourceBarrierAllSubresources;
        ops.push_back(op);
    }
    if (transitionCount > 0) {
        if (wholeResource && uniform
            && transitionCount == entry->subresourceCount) {
            // 全部子资源同一 before -> 折叠为一条 ALL-subresource op
            //（D3D12 语义：Subresource=ALL 要求全部子资源同一起点）。
            D3D12ResourceBarrierOp op;
            op.kind = D3D12ResourceBarrierOp::Kind::Transition;
            op.resource = key.resource;
            op.stateBefore = uniformBefore;
            op.stateAfter = targetBits;
            op.subresource = kD3D12ResourceBarrierAllSubresources;
            ops.push_back(op);
        } else {
            // 混合起点 / 部分子资源：逐子资源 op（显式索引，升序）。
            ops.insert(ops.end(), perSubOps.begin(), perSubOps.end());
        }
    }
    return ops;
}

bool D3D12StateTracker::QueryState(const D3D12SubresourceKey& key,
                                    ResourceState& out) const {
    const SubEntry* sub = ResolveSub(key);
    if (!sub) return false;
    out = FromD3D12ResourceStates(sub->stateBits);
    return out != ResourceState::Unknown;
}

utl::vector<D3D12DecayRecord> D3D12StateTracker::OnSubmitBoundary() {
    utl::vector<D3D12DecayRecord> decayed;
    if (!config_.decayOnSubmit) return decayed;
    // 规则 ③（R17-A 修正）：仅 buffer 衰减；纹理一律不衰减。按注册序 +
    // 子资源升序。D3D12 提交边界（ExecuteCommandLists 完成）的真实衰减面 =
    // buffer（全部）+ UPLOAD/READBACK heap 资源 + simultaneous-access 纹理；
    // 本引擎纹理恒为 DEFAULT-heap committed 且不带 ALLOW_SIMULTANEOUS_ACCESS
    //（D3D12Texture::Initialize），其状态——含从 COMMON 隐式提升出的读类态
    //——在 GPU 侧跨提交持久。M0 初版"纹理提升态衰减"把 tracker 拉回 COMMON
    // 而 GPU 保留提升态：此后任何转换 barrier 的 StateBefore=COMMON 与 GPU
    // 实际不符（debug-layer 拒绝→资源停留旧态，下一次 CopyTextureRegion
    // 即 #538 INVALID_SUBRESOURCE_STATE——Windows 十六测 M1 的 tracker/GPU
    // 分歧源之一，macOS 探针 P5 实证；runbook §26-A）。simultaneous-access
    // 纹理接入时需按资源 flag 恢复条件衰减（TODO M4+ 多队列/共享纹理）。
    for (auto& [id, entry] : entries_) {
        if (entry.isTexture) continue;  // R17-A：纹理不衰减（GPU 真实语义）
        for (u32 s = 0; s < entry.subresourceCount; ++s) {
            SubEntry& sub = entry.subresources[s];
            if (sub.stateBits == kD3D12ResourceState_Common) continue;
            D3D12DecayRecord rec;
            rec.resource = id;
            rec.subresource = s;
            decayed.push_back(rec);
            sub.stateBits = kD3D12ResourceState_Common;
            sub.promoted = false;
        }
    }
    return decayed;
}

D3D12StateTracker::SubEntry* D3D12StateTracker::ResolveSub(
        const D3D12SubresourceKey& key) {
    // R16-A：(resource, isTexture) 双元键——类型不符的同号条目跳过继续扫。
    for (auto& [id, entry] : entries_) {
        if (id != key.resource || key.isTexture != entry.isTexture) continue;
        const u32 index = (key.subresource == kD3D12ResourceBarrierAllSubresources)
            ? 0u : key.subresource;
        if (index >= entry.subresourceCount) return nullptr;
        return &entry.subresources[index];
    }
    return nullptr;
}

const D3D12StateTracker::SubEntry* D3D12StateTracker::ResolveSub(
        const D3D12SubresourceKey& key) const {
    // R16-A：同上（const 侧），双元键扫描。
    for (const auto& [id, entry] : entries_) {
        if (id != key.resource || key.isTexture != entry.isTexture) continue;
        const u32 index = (key.subresource == kD3D12ResourceBarrierAllSubresources)
            ? 0u : key.subresource;
        if (index >= entry.subresourceCount) return nullptr;
        return &entry.subresources[index];
    }
    return nullptr;
}

} // namespace primal::graphics::rhi::d3d12
