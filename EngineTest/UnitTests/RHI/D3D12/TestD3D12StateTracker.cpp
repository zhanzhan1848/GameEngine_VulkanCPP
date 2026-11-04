/**
 * @file TestD3D12StateTracker.cpp
 * @brief D3D12 状态跟踪器 + 状态映射规则表单测（>=30 断言）
 * @details D3D12 M0（F0-6/F0-7 验收）。规则维度：
 *          ① 状态映射（布局态全值 + 生命周期态拒绝 + Present=COMMON）
 *          ② COMMON 读类自动提升（buffer 全类 / 纹理读类 / 写类必显式）
 *          ③ 相同状态无 op（UAV 特例除外）
 *          ④ UAV->UAV WAW 必发 UAV barrier
 *          ⑤ buffer 与纹理 per-mip 独立跟踪
 *          ⑥ 提交边界 decay（R17-A：仅 buffer 全衰；纹理状态——含提升态
 *            ——一律保持，对齐 DEFAULT-heap 非 simultaneous 纹理的 D3D12
 *            真实语义 / 开关）
 *          ⑦ op 序有序性（折叠 ALL / 混合 per-sub 升序）
 *          ⑧ 未注册/越界/生命周期态显式拒绝
 *          ⑨ M2 F2-3：比较采样器映射（ComparisonFunc 全值 / 比较谓词与
 *            VulkanSampler compareEnable 对齐 / 滤波档 COMPARISON 平移）
 *          ⑩ M2 F2-2：阴影深度 pass 转换（DEPTH_WRITE <-> SRV/DEPTH_READ）
 *          ⑫ R16-A：buffer/texture 同数值句柄共存与路由（两池各自从 0
 *            编号，同号必然重叠；M0 键空间下后注册方被静默拒绝、其全部
 *            Transition 返回空 op = Windows 十五测 #538 根因链）+ mipgen
 *            逐 mip 序列回归（GenerateMipmaps 发射链的 tracker 语义快照）
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/D3D12/D3D12StateTracker.h"

using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::d3d12;
using namespace Engine::Test;

// ===== ① 状态映射 =====
TestResult TestStateMappingTable() {
    u32 bits = 0;
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::General, bits) && bits == kD3D12ResourceState_Common, "General -> COMMON");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::ShaderResource, bits) && bits == kD3D12ResourceState_AllShaderResource, "ShaderResource -> 0xC0");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::RenderTarget, bits) && bits == kD3D12ResourceState_RenderTarget, "RenderTarget -> 0x4");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::DepthStencil, bits) && bits == kD3D12ResourceState_DepthWrite, "DepthStencil -> DEPTH_WRITE");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::DepthStencilReadOnly, bits) && bits == kD3D12ResourceState_DepthRead, "DepthReadOnly -> DEPTH_READ");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::UnorderedAccess, bits) && bits == kD3D12ResourceState_UnorderedAccess, "UAV -> 0x8");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::CopySource, bits) && bits == kD3D12ResourceState_CopySource, "CopySource -> 0x800");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::CopyDest, bits) && bits == kD3D12ResourceState_CopyDest, "CopyDest -> 0x400");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::Present, bits) && bits == kD3D12ResourceState_Present && bits == 0, "Present -> 0 (== COMMON)");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::ResolveSource, bits) && bits == kD3D12ResourceState_ResolveSource, "ResolveSource -> 0x2000");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::ResolveDest, bits) && bits == kD3D12ResourceState_ResolveDest, "ResolveDest -> 0x1000");
    TEST_ASSERT(ToD3D12ResourceStates(ResourceState::IndirectArgument, bits) && bits == kD3D12ResourceState_IndirectArgument, "Indirect -> 0x200");
    // 生命周期态显式拒绝
    TEST_ASSERT(!ToD3D12ResourceStates(ResourceState::Unknown, bits), "Unknown rejected");
    TEST_ASSERT(!ToD3D12ResourceStates(ResourceState::Created, bits), "Created rejected");
    TEST_ASSERT(!ToD3D12ResourceStates(ResourceState::PendingUpload, bits), "PendingUpload rejected");
    TEST_ASSERT(!ToD3D12ResourceStates(ResourceState::Destroyed, bits), "Destroyed rejected");
    // PipelineStage / AccessFlag 翻译
    TEST_ASSERT(PipelineStageToResourceStatesBits(PipelineStage::ComputeShader) == kD3D12ResourceState_NonPixelShaderResource, "Compute stage -> NPSR");
    TEST_ASSERT(PipelineStageToResourceStatesBits(PipelineStage::FragmentShader) == kD3D12ResourceState_PixelShaderResource, "Fragment stage -> PSR");
    TEST_ASSERT(AccessFlagsToResourceStatesBits(AccessFlag::ShaderWrite) == kD3D12ResourceState_UnorderedAccess, "ShaderWrite -> UAV");
    TEST_ASSERT(AccessFlagsToResourceStatesBits(AccessFlag::ShaderRead) == kD3D12ResourceState_AllShaderResource, "ShaderRead -> SRV");
    TEST_ASSERT(AccessFlagsToResourceStatesBits(AccessFlag::TransferWrite) == kD3D12ResourceState_CopyDest, "TransferWrite -> COPY_DEST");
    // 提升集合精确性：DepthRead/ResolveSource 只读但不可提升
    TEST_ASSERT(IsReadClassState(ResourceState::ShaderResource), "SRV promotable");
    TEST_ASSERT(IsReadClassState(ResourceState::CopySource), "CopySrc promotable");
    TEST_ASSERT(IsReadClassState(ResourceState::IndirectArgument), "Indirect promotable");
    TEST_ASSERT(!IsReadClassState(ResourceState::DepthStencilReadOnly), "DepthRead NOT promotable");
    TEST_ASSERT(!IsReadClassState(ResourceState::ResolveSource), "ResolveSrc NOT promotable");
    return TestResult::Passed;
}

// ===== ②③ 显式 transition / 相同状态 no-op / COMMON 提升 =====
TestResult TestTransitionBasics() {
    D3D12StateTracker tracker;
    // 纹理 3 mip；显式初始 CopyDest（写类：注册即显式状态，非提升）
    TEST_ASSERT(tracker.RegisterResource(1, true, 3, 1, 1, ResourceState::CopyDest), "register texture 3 mips");

    D3D12SubresourceKey k1{1, 1, true};  // mip 1
    auto ops = tracker.Transition(k1, ResourceState::ShaderResource);
    TEST_ASSERT(ops.size() == 1, "explicit transition emits 1 op");
    TEST_ASSERT(ops[0].kind == D3D12ResourceBarrierOp::Kind::Transition, "kind=Transition");
    TEST_ASSERT(ops[0].stateBefore == kD3D12ResourceState_CopyDest, "before=COPY_DEST");
    TEST_ASSERT(ops[0].stateAfter == kD3D12ResourceState_AllShaderResource, "after=SRV");
    TEST_ASSERT(ops[0].subresource == 1, "subresource index preserved");

    // 相同状态再转：无 op
    ops = tracker.Transition(k1, ResourceState::ShaderResource);
    TEST_ASSERT(ops.empty(), "same-state transition emits nothing");

    // 显式转换回 COMMON 后，读类目标走提升（零 barrier）
    ops = tracker.Transition(k1, ResourceState::General);
    TEST_ASSERT(ops.size() == 1 && ops[0].stateAfter == kD3D12ResourceState_Common, "to COMMON explicit");
    ops = tracker.Transition(k1, ResourceState::CopySource);
    TEST_ASSERT(ops.empty(), "COMMON -> read-class texture promotes silently");
    ResourceState cur = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState(k1, cur) && cur == ResourceState::CopySource, "tracked after promotion");

    // 提升后状态已前进到 CopySource：再转写类是 CopySource -> RT 显式转换
    ops = tracker.Transition(k1, ResourceState::RenderTarget);
    TEST_ASSERT(ops.size() == 1, "promoted-state -> write-class emits explicit transition");
    TEST_ASSERT(ops[0].stateBefore == kD3D12ResourceState_CopySource, "before=COPY_SOURCE (promoted state)");
    TEST_ASSERT(ops[0].stateAfter == kD3D12ResourceState_RenderTarget, "after=RT");

    // 仍在注册初始态（CopyDest）的 mip2：先显式转回 COMMON，再验证
    // COMMON 起点 -> 写类的显式转换
    D3D12SubresourceKey k2{1, 2, true};
    ops = tracker.Transition(k2, ResourceState::General);
    TEST_ASSERT(ops.size() == 1 && ops[0].stateBefore == kD3D12ResourceState_CopyDest,
                "mip2 CopyDest -> COMMON explicit");
    ops = tracker.Transition(k2, ResourceState::RenderTarget);
    TEST_ASSERT(ops.size() == 1, "COMMON -> write-class emits explicit transition");
    TEST_ASSERT(ops[0].stateBefore == kD3D12ResourceState_Common, "before=COMMON");
    TEST_ASSERT(ops[0].stateAfter == kD3D12ResourceState_RenderTarget, "after=RT (COMMON start)");

    // COMMON -> DepthRead：只读但不可提升（D3D12 规则）→ 显式 transition
    ops = tracker.Transition(k2, ResourceState::General);
    TEST_ASSERT(ops.size() == 1, "mip2 RT -> COMMON explicit");
    ops = tracker.Transition(k2, ResourceState::DepthStencilReadOnly);
    TEST_ASSERT(ops.size() == 1, "COMMON -> DepthRead explicit (not promotable)");
    TEST_ASSERT(ops[0].stateBefore == kD3D12ResourceState_Common, "DepthRead before=COMMON");
    return TestResult::Passed;
}

TestResult TestBufferPromotion() {
    D3D12StateTracker tracker;
    TEST_ASSERT(tracker.RegisterResource(7, false, 1, 1, 1, ResourceState::General), "register buffer COMMON");
    D3D12SubresourceKey k{7, 0, false};

    // buffer：COMMON -> 任意状态（含写类）全提升
    auto ops = tracker.Transition(k, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.empty(), "buffer COMMON -> UAV promotes (no barrier)");
    ops = tracker.Transition(k, ResourceState::RenderTarget);
    TEST_ASSERT(ops.size() == 1, "buffer COMMON -> RT after leaving UAV: explicit");

    // promotion 关闭：全显式
    D3D12StateTracker::Config noPromo;
    noPromo.promotionEnabled = false;
    D3D12StateTracker tracker2{noPromo};
    TEST_ASSERT(tracker2.RegisterResource(8, false, 1, 1, 1, ResourceState::General), "register buffer 2");
    D3D12SubresourceKey k8{8, 0, false};
    ops = tracker2.Transition(k8, ResourceState::ShaderResource);
    TEST_ASSERT(ops.size() == 1, "promotion disabled -> explicit transition");
    return TestResult::Passed;
}

// ===== ④ UAV WAW =====
TestResult TestUavWawAlwaysBarriers() {
    D3D12StateTracker tracker;
    TEST_ASSERT(tracker.RegisterResource(2, true, 1, 1, 1, ResourceState::UnorderedAccess), "register UAV texture");
    D3D12SubresourceKey k{2, 0, true};

    auto ops = tracker.Transition(k, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.size() == 1, "UAV -> UAV same state emits barrier");
    TEST_ASSERT(ops[0].kind == D3D12ResourceBarrierOp::Kind::UAV, "kind=UAV");
    TEST_ASSERT(ops[0].stateBefore == 0 && ops[0].stateAfter == 0, "UAV op carries no state change");

    // 连续 compute->compute WAW：每次都发
    ops = tracker.Transition(k, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.size() == 1 && ops[0].kind == D3D12ResourceBarrierOp::Kind::UAV, "second WAW also emits");

    // buffer 的 UAV WAW 同样必发
    D3D12StateTracker trackerB;
    TEST_ASSERT(trackerB.RegisterResource(9, false, 1, 1, 1, ResourceState::UnorderedAccess), "register UAV buffer");
    D3D12SubresourceKey kb{9, 0, false};
    ops = trackerB.Transition(kb, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.size() == 1 && ops[0].kind == D3D12ResourceBarrierOp::Kind::UAV, "buffer WAW emits");
    return TestResult::Passed;
}

// ===== ⑤ per-mip 独立 + ⑦ op 序 =====
TestResult TestPerMipIndependenceAndOrdering() {
    D3D12StateTracker tracker;
    TEST_ASSERT(tracker.RegisterResource(3, true, 3, 1, 1, ResourceState::General), "register 3-mip COMMON");

    // 各 mip 走不同路径：mip0 提升、mip1 显式、mip2 不动
    TEST_ASSERT(tracker.Transition({3, 0, true}, ResourceState::ShaderResource).empty(), "mip0 promotes");
    auto ops = tracker.Transition({3, 1, true}, ResourceState::RenderTarget);
    TEST_ASSERT(ops.size() == 1 && ops[0].subresource == 1, "mip1 explicit RT");
    TEST_ASSERT(tracker.Transition({3, 2, true}, ResourceState::General).empty(), "mip2 stays COMMON (no op)");

    ResourceState s0 = ResourceState::Unknown, s1 = ResourceState::Unknown, s2 = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState({3, 0, true}, s0) && s0 == ResourceState::ShaderResource, "mip0 SRV");
    TEST_ASSERT(tracker.QueryState({3, 1, true}, s1) && s1 == ResourceState::RenderTarget, "mip1 RT");
    TEST_ASSERT(tracker.QueryState({3, 2, true}, s2) && s2 == ResourceState::General, "mip2 COMMON");

    // 混合起点整资源转 CopySource：mip0 SRV→CopySrc 显式、mip1 RT→CopySrc
    // 显式、mip2 COMMON 提升（无 op）→ 两条 per-sub op，升序，各自携带真实 before
    ops = tracker.Transition({3, kD3D12ResourceBarrierAllSubresources, true}, ResourceState::CopySource);
    TEST_ASSERT(ops.size() == 2, "mixed-state whole-resource: explicit subs emit per-sub ops");
    TEST_ASSERT(ops[0].subresource == 0 && ops[0].stateBefore == kD3D12ResourceState_AllShaderResource,
                "op[0] mip0 before=SRV (ascending order)");
    TEST_ASSERT(ops[1].subresource == 1 && ops[1].stateBefore == kD3D12ResourceState_RenderTarget,
                "op[1] mip1 before=RT");
    TEST_ASSERT(ops[0].kind == D3D12ResourceBarrierOp::Kind::Transition
        && ops[1].kind == D3D12ResourceBarrierOp::Kind::Transition, "both are Transition ops");

    // 均匀起点整资源转换：折叠为单条 ALL op
    D3D12StateTracker tracker2;
    TEST_ASSERT(tracker2.RegisterResource(4, true, 4, 1, 1, ResourceState::RenderTarget), "register 4-mip RT");
    ops = tracker2.Transition({4, kD3D12ResourceBarrierAllSubresources, true}, ResourceState::ShaderResource);
    TEST_ASSERT(ops.size() == 1, "uniform whole-resource collapses to 1 op");
    TEST_ASSERT(ops[0].subresource == kD3D12ResourceBarrierAllSubresources, "collapsed op carries ALL marker");
    TEST_ASSERT(ops[0].stateBefore == kD3D12ResourceState_RenderTarget, "collapsed before=RT");
    return TestResult::Passed;
}

// ===== ⑥ decay =====
TestResult TestSubmitBoundaryDecay() {
    D3D12StateTracker tracker;
    TEST_ASSERT(tracker.RegisterResource(5, false, 1, 1, 1, ResourceState::General), "buffer");
    TEST_ASSERT(tracker.RegisterResource(6, true, 2, 1, 1, ResourceState::General), "texture 2 mip");

    // buffer 先提升到 SRV（COMMON->读类，零 op），再 SRV->UAV 显式转换
    TEST_ASSERT(tracker.Transition({5, 0, false}, ResourceState::ShaderResource).empty(), "buffer COMMON->SRV promotes");
    TEST_ASSERT(tracker.Transition({5, 0, false}, ResourceState::UnorderedAccess).size() == 1, "buffer SRV->UAV explicit");
    // 纹理 mip0 显式转 RT；mip1 COMMON 提升到 SRV
    TEST_ASSERT(tracker.Transition({6, 0, true}, ResourceState::RenderTarget).size() == 1, "mip0 explicit RT");
    TEST_ASSERT(tracker.Transition({6, 1, true}, ResourceState::ShaderResource).empty(), "mip1 promotes to SRV");

    auto decayed = tracker.OnSubmitBoundary();
    // R17-A 契约：仅 buffer 衰减——纹理（DEFAULT-heap 非 simultaneous）的
    // 状态含提升态在 GPU 侧跨提交持久，tracker 不得衰减（M0 初版衰减提升
    // 态纹理与 D3D12 真实语义相反 = #538 家族 tracker/GPU 分歧源）。
    TEST_ASSERT(decayed.size() == 1, "buffer decays; texture states (explicit AND promoted) persist");
    TEST_ASSERT(decayed[0].resource == 5 && decayed[0].subresource == 0, "buffer decay record");

    ResourceState s = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState({5, 0, false}, s) && s == ResourceState::General, "buffer back to COMMON");
    TEST_ASSERT(tracker.QueryState({6, 0, true}, s) && s == ResourceState::RenderTarget, "explicit RT persists");
    TEST_ASSERT(tracker.QueryState({6, 1, true}, s) && s == ResourceState::ShaderResource,
                "promoted mip persists (R17-A: GPU keeps promoted texture states)");

    // 再次提交边界：无新衰减（RT 不衰减；其余已 COMMON）
    decayed = tracker.OnSubmitBoundary();
    TEST_ASSERT(decayed.empty(), "second boundary: no further decay");

    // decay 开关关闭：不做任何衰减
    D3D12StateTracker::Config noDecay;
    noDecay.decayOnSubmit = false;
    D3D12StateTracker tracker2{noDecay};
    TEST_ASSERT(tracker2.RegisterResource(10, false, 1, 1, 1, ResourceState::UnorderedAccess), "buffer UAV");
    TEST_ASSERT(tracker2.OnSubmitBoundary().empty(), "decay disabled: nothing decays");
    ResourceState s2 = ResourceState::Unknown;
    TEST_ASSERT(tracker2.QueryState({10, 0, false}, s2) && s2 == ResourceState::UnorderedAccess, "state kept with decay off");
    return TestResult::Passed;
}

// ===== ⑦ Track 显式注入 + ⑧ 拒绝语义 =====
TestResult TestTrackAndRejections() {
    D3D12StateTracker tracker;
    TEST_ASSERT(tracker.RegisterResource(11, true, 2, 1, 1, ResourceState::RenderTarget), "register");
    TEST_ASSERT(!tracker.RegisterResource(11, true, 2, 1, 1, ResourceState::General), "double register rejected");

    // Track 显式改状态（当作显式转换：不参与提升衰减）
    TEST_ASSERT(tracker.Track({11, 1, true}, ResourceState::CopyDest), "track explicit state");
    ResourceState s = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState({11, 1, true}, s) && s == ResourceState::CopyDest, "tracked state visible");
    TEST_ASSERT(!tracker.Track({11, 0, true}, ResourceState::PendingUpload), "lifecycle state rejected by Track");
    TEST_ASSERT(!tracker.Track({99, 0, true}, ResourceState::General), "unregistered resource rejected");

    // 越界子资源 / 生命周期目标 / 未注册：显式空结果
    TEST_ASSERT(tracker.Transition({11, 5, true}, ResourceState::General).empty(), "out-of-range subresource rejected");
    TEST_ASSERT(tracker.Transition({11, 0, true}, ResourceState::Ready).empty(), "lifecycle target rejected");
    TEST_ASSERT(tracker.Transition({42, 0, true}, ResourceState::General).empty(), "unregistered transition rejected");

    // 注销（R16-A：签名带 isTexture——同号另一类型条目不受影响）
    TEST_ASSERT(tracker.UnregisterResource(11, true), "unregister");
    TEST_ASSERT(!tracker.UnregisterResource(11, true), "double unregister rejected");
    TEST_ASSERT(tracker.RegisteredResourceCount() == 0, "empty after unregister");
    return TestResult::Passed;
}

// ===== ⑨ F2-3（M2）比较采样器映射：ComparisonFunc / 比较谓词 / 滤波档 =====
TestResult TestComparisonSamplerMapping() {
    // ComparisonFunc 全值映射（D3D12_COMPARISON_FUNC 枚举值 1..8）
    TEST_ASSERT(ToD3D12ComparisonFunc(ComparisonFunc::Never) == kD3D12ComparisonFunc_Never, "cmp Never");
    TEST_ASSERT(ToD3D12ComparisonFunc(ComparisonFunc::Less) == kD3D12ComparisonFunc_Less, "cmp Less");
    TEST_ASSERT(ToD3D12ComparisonFunc(ComparisonFunc::Equal) == kD3D12ComparisonFunc_Equal, "cmp Equal");
    TEST_ASSERT(ToD3D12ComparisonFunc(ComparisonFunc::LessEqual) == kD3D12ComparisonFunc_LessEqual, "cmp LessEqual");
    TEST_ASSERT(ToD3D12ComparisonFunc(ComparisonFunc::Greater) == kD3D12ComparisonFunc_Greater, "cmp Greater");
    TEST_ASSERT(ToD3D12ComparisonFunc(ComparisonFunc::NotEqual) == kD3D12ComparisonFunc_NotEqual, "cmp NotEqual");
    TEST_ASSERT(ToD3D12ComparisonFunc(ComparisonFunc::GreaterEqual) == kD3D12ComparisonFunc_GreaterEqual, "cmp GreaterEqual");
    TEST_ASSERT(ToD3D12ComparisonFunc(ComparisonFunc::Always) == kD3D12ComparisonFunc_Always, "cmp Always");
    TEST_ASSERT(ToD3D12ComparisonFunc(ComparisonFunc::Unknown) == kD3D12ComparisonFunc_Never, "cmp Unknown->Never conservative");

    // 比较谓词与 VulkanSampler compareEnable 逐字对齐（!= Never）。
    // 关键事实：SamplerDesc 默认构造（comparisonFunc==Always）→ 比较采样器
    // ——RHI 默认值坑在两端一致呈现，调用方必须显式传值（M2 §F2-3 契约）。
    SamplerDesc def{};
    TEST_ASSERT(def.comparisonFunc == ComparisonFunc::Always, "default comparisonFunc is Always (trap fact)");
    TEST_ASSERT(SamplerDescWantsComparison(def), "default desc IS comparison sampler (parity with Vulkan)");
    SamplerDesc never{}; never.comparisonFunc = ComparisonFunc::Never;
    TEST_ASSERT(!SamplerDescWantsComparison(never), "Never -> plain sampler (Vulkan parity)");
    SamplerDesc leq{}; leq.comparisonFunc = ComparisonFunc::LessEqual;
    TEST_ASSERT(SamplerDescWantsComparison(leq), "LessEqual -> comparison sampler");

    // 滤波档：普通档位 + COMPARISON 平移 + 各向异性保留比较档
    TEST_ASSERT(SelectD3D12SamplerFilter(def) == kD3D12Filter_ComparisonMinMagMipLinear, "default Linear3 -> Comparison Linear3");
    TEST_ASSERT(SelectD3D12SamplerFilter(never) == kD3D12Filter_MinMagMipLinear, "Never Linear3 -> plain Linear3");
    SamplerDesc pnt = never; pnt.minFilter = pnt.magFilter = pnt.mipFilter = FilterMode::Point;
    TEST_ASSERT(SelectD3D12SamplerFilter(pnt) == kD3D12Filter_MinMagMipPoint, "Never Point3 -> plain Point3");
    SamplerDesc cmpp = leq; cmpp.minFilter = cmpp.magFilter = cmpp.mipFilter = FilterMode::Point;
    TEST_ASSERT(SelectD3D12SamplerFilter(cmpp) == kD3D12Filter_ComparisonMinMagMipPoint, "LessEqual Point3 -> Comparison Point3");
    SamplerDesc mixed = leq; mixed.minFilter = FilterMode::Point;
    TEST_ASSERT(SelectD3D12SamplerFilter(mixed) == kD3D12Filter_ComparisonMinPointMagLinearMipLinear, "LessEqual P/L/L -> Comparison MinPointMagLinearMipLinear");
    SamplerDesc aniso = never; aniso.maxAnisotropy = 8;
    TEST_ASSERT(SelectD3D12SamplerFilter(aniso) == kD3D12Filter_Anisotropic, "Never aniso -> plain aniso");
    SamplerDesc caniso = leq; caniso.maxAnisotropy = 8;
    TEST_ASSERT(SelectD3D12SamplerFilter(caniso) == kD3D12Filter_ComparisonAnisotropic, "LessEqual aniso -> COMPARISON aniso (no silent tier drop)");
    return TestResult::Passed;
}

// ===== ⑩ F2-2（M2）阴影深度 pass 转换：DEPTH_WRITE <-> SRV/DEPTH_READ =====
TestResult TestShadowDepthPassTransitions() {
    D3D12StateTracker tracker;
    // shadow map：D32 单 mip，注册即 DepthStencil（BeginRenderPass 深度附件态）
    TEST_ASSERT(tracker.RegisterResource(21, true, 1, 1, 1, ResourceState::DepthStencil),
                "register shadow map at DepthStencil");
    D3D12SubresourceKey sm{21, 0, true};

    // 深度 pass 结束 → 采样（InsertBarrier DepthWrite→ShaderResource）
    auto ops = tracker.Transition(sm, ResourceState::ShaderResource);
    TEST_ASSERT(ops.size() == 1, "DEPTH_WRITE -> SRV emits 1 op");
    TEST_ASSERT(ops[0].stateBefore == kD3D12ResourceState_DepthWrite, "before=DEPTH_WRITE");
    TEST_ASSERT(ops[0].stateAfter == kD3D12ResourceState_AllShaderResource, "after=SRV");

    // 采样后下一帧再作深度附件（ShaderResource→DepthStencil：清屏写深度）
    ops = tracker.Transition(sm, ResourceState::DepthStencil);
    TEST_ASSERT(ops.size() == 1, "SRV -> DEPTH_WRITE emits 1 op");
    TEST_ASSERT(ops[0].stateBefore == kD3D12ResourceState_AllShaderResource, "before=SRV");
    TEST_ASSERT(ops[0].stateAfter == kD3D12ResourceState_DepthWrite, "after=DEPTH_WRITE");

    // 只读深度附件路径（lit pass 复用主深度但不写）：DepthWrite→DepthRead
    ops = tracker.Transition(sm, ResourceState::DepthStencilReadOnly);
    TEST_ASSERT(ops.size() == 1 && ops[0].stateAfter == kD3D12ResourceState_DepthRead,
                "DEPTH_WRITE -> DEPTH_READ emits 1 op (ReadOnly mapping)");
    // DepthRead 不在 COMMON 提升集合：回 SRV 须显式 transition（非零 op）
    ops = tracker.Transition(sm, ResourceState::ShaderResource);
    TEST_ASSERT(ops.size() == 1 && ops[0].stateBefore == kD3D12ResourceState_DepthRead,
                "DEPTH_READ -> SRV explicit (not promotion)");
    return TestResult::Passed;
}

// ===== ⑪ F5-4（M5）多阶段 compute UAV 链：GPU 剔除链场景 =====
// 场景=M5 共同 GPU-driven 切片的真实链形：reset(compute 写 counter) →
// cull(compute 写 counter+arg buffer，同 buffer WAW/WAR) →[经
// InsertBarrier(UAV→INDIRECT_ARGUMENT) 事实注入]→ DrawIndexedIndirect 读。
// 断言覆盖（M0 规范措辞）：
//   - 连续 compute 写同一 buffer：每对 UAV→UAV 必发一条 UAV barrier（规则④）；
//   - compute 写→draw 读：无 transition 提升路径（COMMON→INDIRECT_ARGUMENT
//     读类提升零 op）与显式转换路径（UAV→INDIRECT_ARGUMENT 一条 Transition）
//     的边界语义都有记录；
//   - 提交边界 decay 后链可逐帧重放。
TestResult TestM5ComputeChainUavDiscipline() {
    D3D12StateTracker tracker;
    // arg/counter 复合 buffer（真实剔除链同 buffer 多阶段读写），COMMON 注册
    //（createBufferImpl 现行初始态）。
    TEST_ASSERT(tracker.RegisterResource(101, false, 1, 1, 1, ResourceState::General),
                "register cull buffer at COMMON");
    D3D12SubresourceKey cb{101, 0, false};

    // --- 帧 N：stage0 reset 写 counter（dispatch 的隐式 UAV 提升——
    // 由 InsertBarrier 的 beforeState 事实注入表达）---
    TEST_ASSERT(tracker.Track(cb, ResourceState::UnorderedAccess),
                "stage0 compute write: fact-inject UAV");

    // --- stage1 cull 写同 buffer（WAW）：每对必发 UAV barrier ---
    auto ops = tracker.Transition(cb, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.size() == 1 && ops[0].kind == D3D12ResourceBarrierOp::Kind::UAV,
                "stage0->stage1 same-buffer WAW emits exactly 1 UAV barrier");
    // stage2（假想多趟剔除，如 HZB 两阶段）：再来一对，仍必发
    ops = tracker.Transition(cb, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.size() == 1 && ops[0].kind == D3D12ResourceBarrierOp::Kind::UAV,
                "stage1->stage2 same-buffer WAW emits UAV barrier again");
    // stage3：三连写第三对仍必发（≥3 对验证"每对必发"，非仅首对）
    ops = tracker.Transition(cb, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.size() == 1 && ops[0].kind == D3D12ResourceBarrierOp::Kind::UAV
                && ops[0].subresource == kD3D12ResourceBarrierAllSubresources,
                "stage2->stage3 WAW barrier (resource-level, ALL subresources)");

    // --- compute 写 → draw 读（DrawIndexedIndirect 消费 arg buffer）---
    // 显式路径：UAV→INDIRECT_ARGUMENT 一条 Transition op（官方样例模式）
    ops = tracker.Transition(cb, ResourceState::IndirectArgument);
    TEST_ASSERT(ops.size() == 1 && ops[0].kind == D3D12ResourceBarrierOp::Kind::Transition
                && ops[0].stateBefore == kD3D12ResourceState_UnorderedAccess
                && ops[0].stateAfter == kD3D12ResourceState_IndirectArgument,
                "compute-write -> indirect-read: UAV->INDIRECT_ARGUMENT 1 transition");
    // 幂等：ExecuteIndirect 入口重复请求同状态零 op
    ops = tracker.Transition(cb, ResourceState::IndirectArgument);
    TEST_ASSERT(ops.empty(), "already INDIRECT_ARGUMENT: idempotent no-op");

    // --- 提交边界 decay：buffer 全衰减回 COMMON（规则③）---
    auto decayed = tracker.OnSubmitBoundary();
    TEST_ASSERT(decayed.size() == 1 && decayed[0].resource == 101,
                "submit boundary decays cull buffer to COMMON");
    // decay 后下一帧：COMMON→读类目标走提升（零 op，无 transition）——
    // "compute 写→draw 读经 promotion 无 transition"的边界语义记录：
    // 仅当 buffer 处于 COMMON（新帧）且目标为读类（IndirectArgument 可提升）
    // 时才零 barrier；帧内 UAV 写后必须走上面的显式转换。
    ops = tracker.Transition(cb, ResourceState::IndirectArgument);
    TEST_ASSERT(ops.empty(), "COMMON -> INDIRECT_ARGUMENT read promotion: 0 ops");
    TEST_ASSERT(IsReadClassState(ResourceState::IndirectArgument),
                "IndirectArgument is promotion-class read state (semantic record)");
    // 提升态（IndirectArgument）再次衰减后，COMMON→UAV 走 buffer 全类提升
    //（规则② buffer 任意状态可提升）——随后同 buffer WAW 恢复规则④。
    (void)tracker.OnSubmitBoundary();
    ops = tracker.Transition(cb, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.empty(), "COMMON -> UAV buffer promotion: 0 ops (rule 2)");
    ops = tracker.Transition(cb, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.size() == 1 && ops[0].kind == D3D12ResourceBarrierOp::Kind::UAV,
                "next-frame WAW pair still emits UAV barrier");
    return TestResult::Passed;
}

// ===== ⑪ M6 F6-3：TEXTURE3D 子资源模型 =====
// D3D12 3D 纹理无数组维（DepthOrArraySize=depth；子资源 = mip×1×plane，
// 即子资源索引 = mip）。验证：注册几何（mip×1）、整资源单态转换（GI 切片
// 8x8x8 单 mip = 单子资源）、多 mip 3D 的 per-mip 独立索引（mip N → 索引 N
// ——与 2D 的 mip + mipLevels*arrayLayer 公式在 arraySize=1 时一致）、
// D3D12Device 侧 arraySize 钳制语义由源审查覆盖（tracker 层按已钳制几何
// 注册的契约在此锁定）。
TestResult TestM6Texture3DSubresourceModel() {
    D3D12StateTracker tracker;
    // 8x8x8 RGBA16F 单 mip 3D（M6 探针网格）：子资源数=1，整资源单态。
    TEST_ASSERT(tracker.RegisterResource(21, true, 1, 1, 1, ResourceState::General),
                "register 3D probe grid (mip=1 array=1 plane=1)");
    D3D12SubresourceKey whole{21, kD3D12ResourceBarrierAllSubresources, true};
    auto ops = tracker.Transition(whole, ResourceState::UnorderedAccess);
    // COMMON→UAV 是写类显式转换（纹理规则：写类不可从 COMMON 提升）；
    // 单子资源整资源折叠为一条 ALL-subresource op（均匀起点折叠规则）。
    TEST_ASSERT(ops.size() == 1, "3D whole-resource COMMON->UAV explicit (1 op)");
    TEST_ASSERT(ops[0].subresource == kD3D12ResourceBarrierAllSubresources,
                "collapsed single-subresource whole-resource op = ALL marker");
    // UAV→UAV WAW 规则④同 2D（compute bake→accum 链）
    ops = tracker.Transition(whole, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.size() == 1 && ops[0].kind == D3D12ResourceBarrierOp::Kind::UAV,
                "3D UAV->UAV WAW emits UAV barrier (rule 4)");
    // UAV→SRV（延迟光照采样）：显式转换
    ops = tracker.Transition(whole, ResourceState::ShaderResource);
    TEST_ASSERT(ops.size() == 1
                && ops[0].stateBefore == kD3D12ResourceState_UnorderedAccess
                && ops[0].stateAfter == kD3D12ResourceState_AllShaderResource,
                "3D UAV->SRV explicit transition");
    ResourceState cur = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState(whole, cur) && cur == ResourceState::ShaderResource,
                "3D state tracked after transition");

    // 多 mip 3D（4 mip）：子资源索引=mip（数组维恒 1——D3D12 3D 几何）。
    TEST_ASSERT(tracker.RegisterResource(22, true, 4, 1, 1, ResourceState::General),
                "register 4-mip 3D");
    D3D12SubresourceKey m2{22, 2, true};
    ops = tracker.Transition(m2, ResourceState::UnorderedAccess);
    TEST_ASSERT(ops.size() == 1 && ops[0].subresource == 2,
                "3D per-mip index = mip (array dim collapsed)");
    // 越界 mip（=子资源数）拒绝
    D3D12SubresourceKey mBad{22, 4, true};
    ops = tracker.Transition(mBad, ResourceState::ShaderResource);
    TEST_ASSERT(ops.empty(), "out-of-range mip rejected (no ops)");
    return TestResult::Passed;
}

// ===== ⑫ R16-A：buffer/texture 同数值句柄共存 + 路由 + 跨类型注销隔离 =====
// 背景（Windows 十五测 #538 根因链，runbook §25-A）：D3D12Device 的
// bufferAllocator_ / textureAllocator_ 是两个独立 free_list，各自从 0 起编号
// ——同数值句柄必然跨类型重叠。M0 键空间仅按 resource 数值查条目：
// 后注册方被"双注册防御"静默拒绝（D3D12Device 侧不查返回值），其全部
// Transition 返回空 op（无 barrier），GPU 态滞留 COMMON；读类经 D3D12 隐式
// COMMON 提升照常工作（SRV 采样/拷贝均无感），首个写类状态（mipgen 的
// RENDER_TARGET）才被 debug-layer #538 拦下。R16-A 起 key = (resource,
// isTexture)：同号共存合法、路由独立、跨类型注销互不波及。
TestResult TestR16BufferTextureIdCoexistence() {
    // --- 顺序 A：buffer 先注册（十五测的实际触发序：mesh buffer 先于纹理）---
    D3D12StateTracker tracker;
    TEST_ASSERT(tracker.RegisterResource(64, false, 1, 1, 1, ResourceState::General),
                "buffer id=64 registers first");
    // M0 行为下此处返回 false（被静默拒绝）→ 后续纹理 Transition 全空 —— #538。
    TEST_ASSERT(tracker.RegisterResource(64, true, 4, 1, 1, ResourceState::General),
                "texture id=64 coexists with same-numeric buffer (R16-A key space)");
    TEST_ASSERT(tracker.RegisteredResourceCount() == 2, "two independent entries");

    // 纹理键路由到纹理条目：mip1 COMMON→RT 显式（十五测缺失的那条 barrier）
    auto ops = tracker.Transition({64, 1, true}, ResourceState::RenderTarget);
    TEST_ASSERT(ops.size() == 1 && ops[0].subresource == 1,
                "texture key routes to texture entry (sub=1)");
    TEST_ASSERT(ops[0].stateBefore == kD3D12ResourceState_Common
                && ops[0].stateAfter == kD3D12ResourceState_RenderTarget,
                "texture mip1 COMMON->RT (the #538 barrier)");

    // buffer 键路由到 buffer 条目：COMMON→读类提升（零 op），状态独立无串扰
    TEST_ASSERT(tracker.Transition({64, 0, false}, ResourceState::ShaderResource).empty(),
                "buffer key routes to buffer entry (promotion, no cross-talk)");
    ResourceState texS = ResourceState::Unknown, bufS = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState({64, 1, true}, texS) && texS == ResourceState::RenderTarget,
                "texture entry state independent");
    TEST_ASSERT(tracker.QueryState({64, 0, false}, bufS) && bufS == ResourceState::ShaderResource,
                "buffer entry state independent");

    // 错类型键的越界拒绝：buffer 条目无 sub=1（subresourceCount=1）
    TEST_ASSERT(tracker.Transition({64, 1, false}, ResourceState::RenderTarget).empty(),
                "wrong-type key sub=1 out of range for buffer entry");

    // 跨类型注销隔离：销毁 buffer 不得带走同号纹理条目（M0 键空间会）
    TEST_ASSERT(tracker.UnregisterResource(64, false), "unregister buffer only");
    TEST_ASSERT(!tracker.UnregisterResource(64, false), "buffer already gone");
    TEST_ASSERT(tracker.RegisteredResourceCount() == 1, "texture entry survives buffer destroy");
    TEST_ASSERT(tracker.QueryState({64, 1, true}, texS) && texS == ResourceState::RenderTarget,
                "texture state intact after cross-type destroy");
    TEST_ASSERT(tracker.Transition({64, 2, true}, ResourceState::RenderTarget).size() == 1,
                "texture transitions still emit after buffer destroy");
    TEST_ASSERT(tracker.UnregisterResource(64, true), "texture unregister (typed)");

    // --- 顺序 B：纹理先注册（对称锁定）---
    D3D12StateTracker trackerB;
    TEST_ASSERT(trackerB.RegisterResource(65, true, 2, 1, 1, ResourceState::General),
                "texture id=65 registers first");
    TEST_ASSERT(trackerB.RegisterResource(65, false, 1, 1, 1, ResourceState::General),
                "buffer id=65 coexists (reverse order)");
    TEST_ASSERT(trackerB.Transition({65, 1, true}, ResourceState::RenderTarget).size() == 1,
                "reverse-order texture routing intact");
    TEST_ASSERT(trackerB.Transition({65, 0, false}, ResourceState::UnorderedAccess).empty(),
                "reverse-order buffer promotion intact");
    TEST_ASSERT(trackerB.UnregisterResource(65, true), "reverse-order typed unregister");
    TEST_ASSERT(trackerB.RegisteredResourceCount() == 1, "buffer survives texture destroy");

    // 同 (id, 类型) 双注册仍是冲突（M0 原语义保留）
    TEST_ASSERT(!trackerB.RegisterResource(65, false, 1, 1, 1, ResourceState::General),
                "same id+type double-register still rejected");
    return TestResult::Passed;
}

// ===== ⑫ R16-A：mipgen 序列回归（GenerateMipmaps 发射链的 tracker 快照） =====
// 逐条复刻 D3D12CommandBuffer::GenerateMipmaps mip 循环对 tracker 的调用序
//（L1017/L1018/L1045 三拍：src mip m →SRV、dst mip m+1 →RT、画后 →SRV），
// 前置上传尾转换（D3D12Upload::EncodePoolCopies 的 sub0 CopyDest→backState
// ——十五测场景只传 base mip，sub1..3 保持 COMMON）。关键断言 = 首轮
// dst 转换必须产出 (sub1, COMMON→RENDER_TARGET) 一条 op——即 Windows
// debug-layer #538 报缺的那条 barrier；同号 buffer 先注册在场（根因序）。
TestResult TestR16MipgenSequence() {
    D3D12StateTracker tracker;
    // 根因序：同号 buffer 先注册 + 纹理后注册（mips=4，COMMON）
    TEST_ASSERT(tracker.RegisterResource(64, false, 1, 1, 1, ResourceState::General),
                "same-numeric buffer present (root-cause ordering)");
    TEST_ASSERT(tracker.RegisterResource(64, true, 4, 1, 1, ResourceState::General),
                "4-mip texture registers despite buffer id collision");

    // 上传尾（D3D12Upload 同构）：sub0 → COPY_DEST → backState(SRV)
    auto ops = tracker.Transition({64, 0, true}, ResourceState::CopyDest);
    TEST_ASSERT(ops.size() == 1 && ops[0].stateBefore == kD3D12ResourceState_Common
                && ops[0].stateAfter == kD3D12ResourceState_CopyDest,
                "upload: sub0 COMMON->COPY_DEST");
    ops = tracker.Transition({64, 0, true}, ResourceState::ShaderResource);
    TEST_ASSERT(ops.size() == 1 && ops[0].stateBefore == kD3D12ResourceState_CopyDest
                && ops[0].stateAfter == kD3D12ResourceState_AllShaderResource,
                "upload tail: sub0 COPY_DEST->SRV");

    // mipgen 循环 m=0..2（srvCount = mipCount-1）
    for (u32 m = 0; m + 1 < 4; ++m) {
        // 拍 1（L1017）：src mip m → SRV——m=0 幂等零 op（上传尾已 SRV）；
        // m>=1 为 COMMON 读类提升（零 op）。两分支都不得产出 barrier。
        ops = tracker.Transition({64, m, true}, ResourceState::ShaderResource);
        TEST_ASSERT(ops.empty(), "src mip promote/idempotent: 0 ops");

        // 拍 2（L1018）：dst mip m+1 → RT——COMMON 起点写类必显式，恰好 1 op，
        // 子资源号 = m+1，before=COMMON，after=RENDER_TARGET。
        ops = tracker.Transition({64, m + 1, true}, ResourceState::RenderTarget);
        TEST_ASSERT(ops.size() == 1, "dst mip COMMON->RT: exactly 1 op");
        TEST_ASSERT(ops[0].kind == D3D12ResourceBarrierOp::Kind::Transition,
                    "dst op is a Transition");
        TEST_ASSERT(ops[0].subresource == m + 1, "dst op subresource = m+1");
        TEST_ASSERT(ops[0].stateBefore == kD3D12ResourceState_Common,
                    "dst op before=COMMON (GPU-truthful)");
        TEST_ASSERT(ops[0].stateAfter == kD3D12ResourceState_RenderTarget,
                    "dst op after=RENDER_TARGET");

        // 拍 3（L1045）：画后 dst mip m+1 → SRV——RT→SRV 显式 1 op。
        ops = tracker.Transition({64, m + 1, true}, ResourceState::ShaderResource);
        TEST_ASSERT(ops.size() == 1 && ops[0].stateBefore == kD3D12ResourceState_RenderTarget
                    && ops[0].stateAfter == kD3D12ResourceState_AllShaderResource,
                    "post-draw RT->SRV");
    }

    // 终态：全 mip ShaderResource（与 Vulkan 版 SHADER_READ_ONLY 收敛一致）
    for (u32 s = 0; s < 4; ++s) {
        ResourceState st = ResourceState::Unknown;
        TEST_ASSERT(tracker.QueryState({64, s, true}, st) && st == ResourceState::ShaderResource,
                    "final: all mips ShaderResource");
    }
    // buffer 条目未被 mipgen 序列波及（仍 COMMON → 提升路径可用）
    ResourceState bufS = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState({64, 0, false}, bufS) && bufS == ResourceState::General,
                "same-numeric buffer untouched by texture mipgen sequence");
    return TestResult::Passed;
}

// ===== ⑬ R17-A：上传→读出→再上传交替序列（CopyTextureRegion #538 回归） =====
// 逐条复刻 Windows 十六测 M1 的纹理拷贝调用序（tracker 层快照）：
//   - 上传 = D3D12CommandBuffer::CopyBufferToTexture 形态（ALL 键 →COPY_DEST，
//     拷贝后留置 COPY_DEST——宿主尾 barrier 驱动后续状态）；
//   - 读出 = R17-A 后的 CopyTextureToBuffer 形态（region 子资源键 →COPY_SOURCE，
//     拷贝后回置 QueryState 存下的读前态）；
//   - 再上传 = 第二次 ALL 键 →COPY_DEST（幂等断言）。
// 断言重点 = COPY_DEST→COPY_SOURCE→COPY_DEST 全链显式 op 正确（子资源号 /
// before / after 逐条），交替后状态与 GPU 真实语义一致（含提交边界：纹理
// 提升态不衰减）。旧实现（ALL 键读出 + 无条件收敛 SRV）在"上传后未插尾
// barrier 即读出"时会把整纹理打回 SRV、且未读子资源被幻影提升——十六测
// #538 的分歧形态（macOS 探针 P3/P4，runbook §26-A）。
TestResult TestR17UploadReadbackAlternation() {
    D3D12StateTracker tracker;
    // 同号 buffer 在场（根因序延续 R16 用例）+ 4-mip COMMON 纹理
    TEST_ASSERT(tracker.RegisterResource(64, false, 1, 1, 1, ResourceState::General),
                "same-numeric buffer present");
    TEST_ASSERT(tracker.RegisterResource(64, true, 4, 1, 1, ResourceState::General),
                "4-mip texture registers");

    // --- 上传 1（CopyBufferToTexture 形态：ALL→COPY_DEST）---
    auto ops = tracker.Transition({64, kD3D12ResourceBarrierAllSubresources, true},
                                  ResourceState::CopyDest);
    TEST_ASSERT(ops.size() == 1 && ops[0].subresource == kD3D12ResourceBarrierAllSubresources
                && ops[0].stateBefore == kD3D12ResourceState_Common
                && ops[0].stateAfter == kD3D12ResourceState_CopyDest,
                "upload1: folded ALL COMMON->COPY_DEST");

    // --- 读出 1（R17-A CopyTextureToBuffer 形态：region sub0，读前态=COPY_DEST）---
    ResourceState pre = ResourceState::ShaderResource;
    TEST_ASSERT(tracker.QueryState({64, 0, true}, pre) && pre == ResourceState::CopyDest,
                "readback1: pre-state queried = COPY_DEST");
    ops = tracker.Transition({64, 0, true}, ResourceState::CopySource);
    TEST_ASSERT(ops.size() == 1 && ops[0].subresource == 0
                && ops[0].stateBefore == kD3D12ResourceState_CopyDest
                && ops[0].stateAfter == kD3D12ResourceState_CopySource,
                "readback1 pre: sub0 COPY_DEST->COPY_SOURCE explicit");
    // （CopyTextureRegion src=sub0 此处 GPU=COPY_SOURCE ✓——tracker 级断言状态即可）
    ops = tracker.Transition({64, 0, true}, pre);   // 回置读前态
    TEST_ASSERT(ops.size() == 1 && ops[0].stateBefore == kD3D12ResourceState_CopySource
                && ops[0].stateAfter == kD3D12ResourceState_CopyDest,
                "readback1 post: sub0 COPY_SOURCE->COPY_DEST restore");

    // --- 提交边界：纹理零衰减（R17-A 契约；buffer 才衰减）---
    auto decayed = tracker.OnSubmitBoundary();
    bool texDecay = false;
    for (const auto& d : decayed) texDecay = texDecay || d.resource == 64;
    TEST_ASSERT(!texDecay, "submit boundary: texture subresources never decay");

    // 读出作用域证据：sub1..3 此刻仍 COPY_DEST（旧 ALL 键实现会在此前把
    // 它们幻影提升成 COPY_SOURCE——本断言即其反面）。
    ResourceState untouched = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState({64, 1, true}, untouched)
                && untouched == ResourceState::CopyDest,
                "sub1 untouched by scoped readback (still COPY_DEST)");

    // --- 上传 2（ALL→COPY_DEST 幂等：全子资源已在 COPY_DEST → 零 op）---
    ops = tracker.Transition({64, kD3D12ResourceBarrierAllSubresources, true},
                             ResourceState::CopyDest);
    TEST_ASSERT(ops.empty(), "upload2: idempotent zero ops (state already COPY_DEST)");

    // --- 尾 barrier（宿主形态：ALL→SRV）+ 再读出（读前态=SRV）---
    ops = tracker.Transition({64, kD3D12ResourceBarrierAllSubresources, true},
                             ResourceState::ShaderResource);
    TEST_ASSERT(ops.size() == 1 && ops[0].stateBefore == kD3D12ResourceState_CopyDest
                && ops[0].stateAfter == kD3D12ResourceState_AllShaderResource,
                "tail: ALL COPY_DEST->SRV");
    pre = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState({64, 0, true}, pre) && pre == ResourceState::ShaderResource,
                "readback2: pre-state = SRV");
    ops = tracker.Transition({64, 0, true}, ResourceState::CopySource);
    TEST_ASSERT(ops.size() == 1 && ops[0].stateBefore == kD3D12ResourceState_AllShaderResource
                && ops[0].stateAfter == kD3D12ResourceState_CopySource,
                "readback2 pre: SRV->COPY_SOURCE explicit");
    ops = tracker.Transition({64, 0, true}, pre);
    TEST_ASSERT(ops.size() == 1 && ops[0].stateBefore == kD3D12ResourceState_CopySource
                && ops[0].stateAfter == kD3D12ResourceState_AllShaderResource,
                "readback2 post: COPY_SOURCE->SRV restore");

    // 终态：sub0 SRV（读前态保真）。sub1..3 的"未被读出路径波及"在上方
    // 尾 barrier 之前断言（ALL→SRV 尾 barrier 会合法地转换全部子资源，
    // 终态 sub1..3 = SRV 属宿主驱动，非读出路径所致）。
    ResourceState s0 = ResourceState::Unknown, s1 = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState({64, 0, true}, s0) && s0 == ResourceState::ShaderResource,
                "final sub0 = SRV");
    TEST_ASSERT(tracker.QueryState({64, 1, true}, s1) && s1 == ResourceState::ShaderResource,
                "final sub1 = SRV via host tail barrier (not readback path)");
    return TestResult::Passed;
}

// ===== ⑬ R17-A：全新 COMMON 纹理的区域读出（幻影提升回归） =====
// 旧实现 pre 用 ALL 键：region 未覆盖的子资源也在 tracker 里被零 barrier
// "提升"成 COPY_SOURCE（GPU 只在使用点提升实际被读的子资源），post 的
// COPY_SOURCE→SRV 显式 barrier 于是对 GPU 实际 COMMON 的子资源携带错误
// StateBefore。R17-A 后 pre/post 都按 region 子资源作用域 + 回置读前态：
// 全新 COMMON 纹理读 sub0 = 提升（零 op，GPU 于拷贝使用点同步提升）+
// 回置 COMMON（显式 COPY_SOURCE→COMMON barrier——X→COMMON 恒合法），
// 其余子资源零接触。
TestResult TestR17FreshCommonScopedReadback() {
    D3D12StateTracker tracker;
    TEST_ASSERT(tracker.RegisterResource(80, true, 4, 1, 1, ResourceState::General),
                "fresh 4-mip COMMON texture");

    ResourceState pre = ResourceState::ShaderResource;
    TEST_ASSERT(tracker.QueryState({80, 0, true}, pre) && pre == ResourceState::General,
                "pre-state queried = COMMON");
    // pre：sub0 COMMON→COPY_SOURCE = 读类提升（零 op；GPU 在 CopyTextureRegion
    // 使用点对被读子资源做同一提升——两者一致，无幻影）。
    auto ops = tracker.Transition({80, 0, true}, ResourceState::CopySource);
    TEST_ASSERT(ops.empty(), "scoped pre: sub0 COMMON->COPY_SOURCE promotes (used sub only)");
    // post：回置读前态 COMMON——显式 COPY_SOURCE→COMMON（合法转换对）。
    ops = tracker.Transition({80, 0, true}, pre);
    TEST_ASSERT(ops.size() == 1 && ops[0].subresource == 0
                && ops[0].stateBefore == kD3D12ResourceState_CopySource
                && ops[0].stateAfter == kD3D12ResourceState_Common,
                "scoped post: sub0 COPY_SOURCE->COMMON restore");

    // 其余子资源零接触（旧 ALL 键会把 sub1..3 幻影提升成 COPY_SOURCE）。
    for (u32 s = 1; s < 4; ++s) {
        ResourceState st = ResourceState::Unknown;
        TEST_ASSERT(tracker.QueryState({80, s, true}, st) && st == ResourceState::General,
                    "unread subs untouched (no phantom COPY_SOURCE)");
    }
    ResourceState s0 = ResourceState::Unknown;
    TEST_ASSERT(tracker.QueryState({80, 0, true}, s0) && s0 == ResourceState::General,
                "sub0 restored to COMMON");
    return TestResult::Passed;
}

void RegisterD3D12StateTrackerTests() {
    auto suite = std::make_shared<TestSuite>("D3D12StateTrackerTests");
    suite->AddTestCase(TestCase("StateMappingTable", TestStateMappingTable));
    suite->AddTestCase(TestCase("TransitionBasics", TestTransitionBasics));
    suite->AddTestCase(TestCase("BufferPromotion", TestBufferPromotion));
    suite->AddTestCase(TestCase("UavWawAlwaysBarriers", TestUavWawAlwaysBarriers));
    suite->AddTestCase(TestCase("PerMipIndependenceAndOrdering", TestPerMipIndependenceAndOrdering));
    suite->AddTestCase(TestCase("SubmitBoundaryDecay", TestSubmitBoundaryDecay));
    suite->AddTestCase(TestCase("TrackAndRejections", TestTrackAndRejections));
    suite->AddTestCase(TestCase("ComparisonSamplerMapping", TestComparisonSamplerMapping));
    suite->AddTestCase(TestCase("ShadowDepthPassTransitions", TestShadowDepthPassTransitions));
    suite->AddTestCase(TestCase("M5ComputeChainUavDiscipline", TestM5ComputeChainUavDiscipline));
    suite->AddTestCase(TestCase("M6Texture3DSubresourceModel", TestM6Texture3DSubresourceModel));
    suite->AddTestCase(TestCase("R16BufferTextureIdCoexistence", TestR16BufferTextureIdCoexistence));
    suite->AddTestCase(TestCase("R16MipgenSequence", TestR16MipgenSequence));
    suite->AddTestCase(TestCase("R17UploadReadbackAlternation", TestR17UploadReadbackAlternation));
    suite->AddTestCase(TestCase("R17FreshCommonScopedReadback", TestR17FreshCommonScopedReadback));
    TestRunner::RegisterTestSuite(suite);
}

#ifndef UNIT_TEST_LIB
int main() {
    RegisterD3D12StateTrackerTests();
    // M5 移交修复：失败码传播（对齐 TestNaniteResourceManager main 风格——
    // RunAllSuites 返回 TestStats，failedTests>0 → exit 1；不再恒 0）。
    Engine::Test::TestStats stats = TestRunner::RunAllSuites();
    return stats.failedTests == 0 ? 0 : 1;
}
#endif
