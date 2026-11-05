// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12DescriptorHeaps.h — 描述符堆管理（M1 F1-5）。
//
// 堆布局（对齐 M1 阶段文档 F1-5 与 Parity Matrix 的 view 折叠决策）：
//   1. CBV/SRV/UAV 主堆：唯一 shader-visible 堆（单命令列表同时只能绑一个
//      CBV/SRV/UAV 堆——全局唯一堆 = descriptor table 绑定模型的硬前提）。
//      分配策略 = 分帧槽线性分配 + 每槽 fence 完成值回收 + 槽纪元（R21-C）：
//        - BeginFrame(slot)：若该槽上次提交的 fence 目标值已完成
//          （D3D12FencePool::GetCompletedValue >= perSlotGuardTarget[slot]），
//          bump offset 归零（该槽上一轮描述符已不在飞行中，安全复用）；
//          未完成则 CPU 等待该目标值（防御非 RenderSystem 宿主的提交顺序）。
//          同时该槽纪元（epoch）自增——旧纪元的全部线性区段作废。
//        - Alloc(count)：从当前槽线性 bump；容量耗尽 = 显式失败（false），
//          不静默回绕（回绕会让在飞描述符被覆写）。
//        - set 区段时效 = (帧槽, 纪元) 双键戳（R21-C）：EnsureSetRegionCurrent
//          只比槽号的旧判定会在"set 隔一整个槽周期未被触碰后同槽回归"时
//          放行已被回收的陈旧区段 → 区段重叠互踩；纪元键封死该路径。
//          数学/纪元策略在 DescriptorFrameSlotPolicy（平台中立，macOS 单测
//          同源——R21-C）。
//        - 帧内分配的 fence 护栏 = "下一次将被 Signal 的目标值"
//          （D3D12FencePool::PeekNextTarget）——消费这些描述符的提交即
//          Signal 该值；复用判定见上。生命周期以 fence 值为据，无帧数算术。
//   2. Sampler 堆：shader-visible（descriptor table 绑采样器必需）；持久
//      槽位 + free-list 回收（SamplerHandle 生命周期走 RHIAllocator 的
//      延迟槽位释放，与资源同款护栏）。
//   3. RTV / DSV 堆：非 shader-visible（CPU 写入即可），持久槽位 free-list。
//
// 双堆（shadow）模型（R13/T1）：两个 shader-visible 堆（CBV/SRV/UAV 主堆
// 与 Sampler 堆）各配一个**同容量、同槽位布局的非 shader-visible 阴影堆**。
// 根因（Windows 十一测 CDB 实证，debug-layer error #654
// COPY_DESCRIPTORS_WRITE_ONLY_DESCRIPTOR）：shader-visible 堆的 CPU 句柄是
// write-only——任何 CopyDescriptorsSimple 以它作源 = 非法。M1 设计里纹理
// SRV/UAV 持久槽与 set 区段迁移/拷贝全在可见堆内自拷，12 次运行全在首帧
// 前 exit 2170（0x87A debug break）。修法=标准双写模式：
//   - 描述符先写 shadow 堆（Create*View/CreateSampler 目标 = shadow 句柄，
//     或 CopyDescriptorsSimple 拷入 shadow 槽）；
//   - 再 CopyDescriptorsSimple(shadow → visible) 同槽位（唯一合法方向）；
//   - 一切"读描述符做源"的拷贝（纹理 SRV/UAV 源、sampler 源、set 区段
//     迁移源）一律读 shadow 堆。
// 不变量：任一 live 槽位的 shadow 内容 == visible 内容（每次写都双落）。
// GPU 句柄仍只来自 visible 堆（descriptor table 绑定模型不变）；幂等/
// 迁移/帧槽回收语义不变（shadow 与 visible 槽位一一对应，bump/free-list
// 只有一套，作用于两者共用的"槽位号"坐标）。RTV/DSV 非可见堆无此限制，
// 不配 shadow。
//
// 句柄语义（双堆下按堆归属命名）：调用方持有的是"槽位号"（u32）；
//   - CbvSrvUavCpuHandle / SamplerCpuHandle       = **visible 堆** CPU 句柄
//     （write-only：仅可作 Create*View 目标或 CopyDescriptors 目标，禁作源）；
//   - CbvSrvUavShadowCpuHandle / SamplerShadowCpuHandle = **shadow 堆** CPU
//     句柄（read-write：Create*View 目标 + 一切拷贝的源/中转）。
// GPU 句柄由 visible 堆换算。kInvalidSlot = 无效。
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include "../../Core/RHIDescriptorSetLayout.h"
#include "../../Core/RHIDescriptorSet.h"
// R21-C：帧槽线性分配 + 槽纪元策略（平台中立模型，macOS 单测同源）。
#include "D3D12DescriptorSlotPolicy.h"

// 2026-11-03 R2（Windows 首编修复）：本头此前直接开 rhi::d3d12 命名空间且未
// 前置声明 rhi::D3D12Device / D3D12DescriptorHeapManager——
//   1) 类内 `friend class primal::graphics::rhi::D3D12Device;` 引用未声明类
//      （ill-formed：clang 实测报 no class named ...；MSVC 报不完整类型系）；
//   2) D3D12Sampler::Initialize/Destroy 的参数 D3D12DescriptorHeapManager&
//      在声明点（本头 157 行）之前不可见。
// 修法=对齐 D3D12Buffer.h 等兄弟头：先在 rhi 命名空间前置声明 D3D12Device，
//  再入 d3d12 子命名空间并前置声明 D3D12DescriptorHeapManager（引用形参
//  只需不完整类型即可）。
namespace primal::graphics::rhi {

class D3D12Device;

namespace d3d12 {

class D3D12FencePool;
class D3D12DescriptorHeapManager;

// ============================================================================
// 描述符堆容量规划（R18-B，2026-11-05 Windows 十七测后）
//
// 根因：M1 期初版 Config 默认 sampler 堆总容量 128 / 持久段 32 → 帧槽
// 线性区 (128-32)/3 = 32/slot。十七测 M2-M6 实测：场景既有 31 + 新增 2
// = 33 > 32 → AllocSamplerRange/持久槽位耗尽 → 材质描述符（set）创建
// 失败，M2-M6 全挂。容量从此集中在此处命名常量（此前散在 Config 默认
// 值里，数字无出处注释）。
//
// 真头依据（d3d12.h，Windows SDK；仓库内 vendored 副本对照一致
// third_party/xatlas/.../dxsdk/include/d3d12.h L861/L857/L859，并与
// microsoft/DirectX-Headers 官方主线逐字核对）：
//   #define D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE        ( 2048 )
//   #define D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 ( 1000000 )
//   #define D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_2 ( 1000000 )
// sampler 堆满配 2048（规范上限，无空堆创建语义风险——所有堆容量非零）；
// CBV/SRV/UAV 主堆现值 16384 距 TIER 上限 1,000,000 尚有 61x 余量，
// 十七测无该堆耗尽签名 → 按"只提确认会超的"原则本轮不提，登记观察
// （耗尽签名 = AllocShaderVisible 的 "descriptor capacity exhausted" 日志）。
//
// 采样器堆分段：持久段 512（渲染器侧 37 处 CreateSampler 调用点 + 场景
// 材质采样器，十七测实测 33 即耗尽 → 512 ≈ 15x 余量）；线性区
// (2048-512)/3 = 512/slot（十七测 32/slot 即耗尽 → 16x 余量）。
// ============================================================================
inline constexpr u32 kCbvSrvUavHeapCapacity = 16384;        ///< 主堆总容量（距 TIER 上限 61x，未提——见上）
inline constexpr u32 kCbvSrvUavPersistentCapacity = 4096;    ///< 纹理 SRV/UAV 持久槽位
inline constexpr u32 kSamplerHeapCapacity =
    D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;              ///< 2048，规范上限满配
inline constexpr u32 kSamplerPersistentCapacity = 512;       ///< SamplerHandle 对象持久槽位
inline constexpr u32 kRtvHeapCapacity = 256;                 ///< RTV 非可见堆
inline constexpr u32 kDsvHeapCapacity = 64;                  ///< DSV 非可见堆
inline constexpr u32 kDescriptorHeapFrameSlotCount = 3;      ///< = MAX_FRAMES_IN_FLIGHT

// ============================================================================
// D3D12DescriptorSetLayout — 绑定模型的 set 布局描述（M1 F1-4/F1-5）
//
// 持有 RHI 绑定副本 + "set 实例区段内描述符排列"（绑定模型契约 §2/§4：
// CBV 段 → SRV 段 → UAV 段，段内 binding 升序；sampler 绑定另入
// sampler 表区段）。binding → 区段内偏移映射供 UpdateDescriptorSets 写入。
// RegisterSpace（set 序号）在 PipelineLayout 构建期填充（layout 单独
// 创建时不自知其在管线布局中的位置）。
// ============================================================================
class D3D12DescriptorSetLayout : public RHIDescriptorSetLayout {
    friend class primal::graphics::rhi::D3D12Device;
public:
    D3D12DescriptorSetLayout(RHIDeviceBase& device, const DescriptorSetLayoutDesc& desc);
    D3D12DescriptorSetLayout(D3D12DescriptorSetLayout&&) = delete;
    D3D12DescriptorSetLayout& operator=(D3D12DescriptorSetLayout&&) = delete;
    D3D12DescriptorSetLayout(const D3D12DescriptorSetLayout&) = delete;
    D3D12DescriptorSetLayout& operator=(const D3D12DescriptorSetLayout&) = delete;
    virtual ~D3D12DescriptorSetLayout() { destroyImpl(); }

    bool Initialize() override;

    u32 TotalDescriptorCount() const { return totalDescriptors_; }
    u32 SamplerCount() const { return samplerCount_; }

    /// binding 分类（UpdateDescriptorSets 写入 + root range 构建的依据）。
    /// 主段（CBV/SRV/UAV 主堆）与 sampler 段可同时存在（CombinedImageSampler：
    /// 图像进 SRV 段 + 采样器进 sampler 段）。
    struct BindingClass {
        bool hasMain = false;                    ///< 进主表（CBV/SRV/UAV）
        u32 mainOffset = 0;                      ///< 主段内偏移
        D3D12_DESCRIPTOR_RANGE_TYPE mainType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bool hasSampler = false;                 ///< 进 sampler 表
        u32 samplerOffset = 0;                   ///< sampler 段内偏移
        u32 count = 1;                           ///< 数组长度
    };
    bool QueryBinding(u32 binding, BindingClass& out) const;

protected:
    void destroyImpl() override {}
    void* mapImpl(u64, u64) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, u64, u64) override { return false; }

private:
    std::vector<std::pair<u32, BindingClass>> slotTable_;  ///< (binding, class)；初始化时排序
    u32 totalDescriptors_ = 0;         ///< 主堆（CBV/SRV/UAV）描述符数
    u32 samplerCount_ = 0;             ///< sampler 堆描述符数
};

// ============================================================================
// D3D12DescriptorSet — set 实例（shader-visible 堆中的连续区段）
//
// 区段随帧槽线性分配；跨帧长寿命复用（Vulkan 语义：创建一次、每帧绑定）
// 由"区段迁移"闭合：绑定期发现区段属旧帧槽纪元（该槽区域即将/已被轮转
// 回收）→ 在当前槽重分配并 CopyDescriptorsSimple 迁移内容。帧内每帧重写
// （RenderGraph 路径）则迁移不触发（当前槽即属主槽）。
// ============================================================================
class D3D12DescriptorSet : public RHIDescriptorSet {
    friend class primal::graphics::rhi::D3D12Device;
    friend class D3D12CommandBufferImpl;
public:
    D3D12DescriptorSet(RHIDeviceBase& device, const DescriptorSetDesc& desc);
    D3D12DescriptorSet(D3D12DescriptorSet&&) = delete;
    D3D12DescriptorSet& operator=(D3D12DescriptorSet&&) = delete;
    D3D12DescriptorSet(const D3D12DescriptorSet&) = delete;
    D3D12DescriptorSet& operator=(const D3D12DescriptorSet&) = delete;
    virtual ~D3D12DescriptorSet() { destroyImpl(); }

    bool Initialize() override;

    /// 主堆区段基槽 + sampler 区段基槽（kInvalidSlot = 无该区段）。
    u32 BaseSlot() const { return baseSlot_; }
    u32 SamplerBaseSlot() const { return samplerBaseSlot_; }

    /// 区段属主槽 + 分配纪元（R21-C：两者都与当前帧槽/纪元一致才免迁移
    /// ——只比槽号的旧判定在"set 隔一整个槽周期未被触碰后同槽回归"时
    /// 会放行已被 BeginFrame 回收的陈旧区段 → 区段重叠互踩）。
    u32 OwnerSlot() const { return ownerSlot_; }
    u64 OwnerEpoch() const { return ownerEpoch_; }
    u32 MainCount() const { return mainCount_; }
    u32 SamplerCount() const { return samplerCount_; }

protected:
    void destroyImpl() override;
    void* mapImpl(u64, u64) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, u64, u64) override { return false; }

private:
    friend class primal::graphics::rhi::D3D12Device;
    u32 baseSlot_ = 0xFFFFFFFFu;
    u32 samplerBaseSlot_ = 0xFFFFFFFFu;
    u32 ownerSlot_ = 0xFFFFFFFFu;
    u64 ownerEpoch_ = 0;               ///< R21C：分配时槽纪元戳
    u32 mainCount_ = 0;
    u32 samplerCount_ = 0;
};

// ============================================================================
// D3D12Sampler — SamplerHandle 承载体（sampler 堆持久槽位）
// ============================================================================
class D3D12Sampler {
    friend class primal::graphics::rhi::D3D12Device;
public:
    D3D12Sampler() = default;
    D3D12Sampler(const D3D12Sampler&) = delete;
    D3D12Sampler& operator=(const D3D12Sampler&) = delete;

    bool Initialize(ID3D12Device* device, D3D12DescriptorHeapManager& heaps,
                    const SamplerDesc& desc);
    void Destroy(D3D12DescriptorHeapManager& heaps);

    u32 Slot() const { return slot_; }
    SamplerHandle GetHandle() const { return handle_; }
    void SetHandle(SamplerHandle h) { handle_ = h; }

private:
    SamplerHandle handle_{handles::INVALID_SAMPLER};
    u32 slot_ = 0xFFFFFFFFu;
};

class D3D12DescriptorHeapManager {
public:
    static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

    struct Config {
        u32 cbvSrvUavCapacity = kCbvSrvUavHeapCapacity;             ///< shader-visible 主堆总容量
        u32 cbvSrvUavPersistentCapacity = kCbvSrvUavPersistentCapacity;  ///< 其中持久槽位（纹理 SRV/UAV 源）
        u32 samplerCapacity = kSamplerHeapCapacity;                 ///< shader-visible sampler 堆总容量（2048 规范上限）
        u32 samplerPersistentCapacity = kSamplerPersistentCapacity; ///< 其中持久槽位数（SamplerHandle 对象）
        u32 rtvCapacity = kRtvHeapCapacity;                         ///< RTV 非可见堆容量
        u32 dsvCapacity = kDsvHeapCapacity;                         ///< DSV 非可见堆容量
        u32 frameSlotCount = kDescriptorHeapFrameSlotCount;         ///< 线性分配子区域数（= MAX_FRAMES_IN_FLIGHT）
    };

    D3D12DescriptorHeapManager() = default;
    ~D3D12DescriptorHeapManager() = default;
    D3D12DescriptorHeapManager(const D3D12DescriptorHeapManager&) = delete;
    D3D12DescriptorHeapManager& operator=(const D3D12DescriptorHeapManager&) = delete;

    bool Initialize(ID3D12Device* device, const Config& config);
    void Shutdown();

    // ------------------------------------------------------------------
    // 帧槽线性分配（shader-visible CBV/SRV/UAV 主堆）
    // ------------------------------------------------------------------
    /// 帧槽推进：护拦目标值已完成 → bump 归零；否则 CPU 等待（fence 完成值
    /// 判定，见文件头）。每帧 BeginFrame（RenderSystem::BeginFrame 之后）调用。
    void BeginFrame(u32 frameSlot, D3D12FencePool& fence);

    /// 当前槽线性分配 count 个描述符；失败（容量耗尽）返回 false。
    bool AllocShaderVisible(u32 count, u32& outBaseSlot);

    // ------------------------------------------------------------------
    // CBV/SRV/UAV 持久槽位（纹理 SRV/UAV 源描述符）
    //
    // 纹理是长寿命对象——其 SRV/UAV 不能落在分帧线性区（3 帧后区域被轮转
    // 覆写）。持久槽位作为 CopyDescriptorsSimple 的拷贝源；set 实例区段
    // （线性区）在 UpdateDescriptorSets 时把内容拷入自己的表区。
    // ------------------------------------------------------------------
    u32 AllocCbvSrvUavPersistent();
    void FreeCbvSrvUavPersistent(u32 slot);

    /// 设备 Submit 时调用：记录护拦目标值（该值 Signal 后当前槽的分配
    /// 即视为不在飞行）。
    void OnSubmit(u64 fenceTarget) {
        if (currentSlot_ < perSlotGuardTarget_.size()) {
            perSlotGuardTarget_[currentSlot_] = fenceTarget;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CbvSrvUavCpuHandle(u32 slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE CbvSrvUavGpuHandle(u32 slot) const;

    // ------------------------------------------------------------------
    // shadow 堆（R13/T1 双写模式——语义见文件头）
    // ------------------------------------------------------------------
    /// shadow 堆 CPU 句柄（read-write：Create*View 目标 / 一切拷贝源）。
    D3D12_CPU_DESCRIPTOR_HANDLE CbvSrvUavShadowCpuHandle(u32 slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE SamplerShadowCpuHandle(u32 slot) const;

    /// 双写第二步：CopyDescriptorsSimple(shadow → visible) 同槽位起 count 个
    /// 连续描述符（唯一合法方向；Initialize 后可用）。
    void CopyCbvSrvUavShadowToVisible(u32 slot, u32 count = 1);
    void CopySamplerShadowToVisible(u32 slot, u32 count = 1);

    // ------------------------------------------------------------------
    // 持久槽位（RTV/DSV 非可见堆 + sampler shader-visible 堆）
    // ------------------------------------------------------------------
    u32 AllocRtv();
    void FreeRtv(u32 slot);
    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle(u32 slot) const;

    u32 AllocDsv();
    void FreeDsv(u32 slot);
    D3D12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle(u32 slot) const;

    u32 AllocSampler();
    void FreeSampler(u32 slot);
    D3D12_CPU_DESCRIPTOR_HANDLE SamplerCpuHandle(u32 slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SamplerGpuHandle(u32 slot) const;

    /// sampler 堆的帧槽线性区段（set 实例的 sampler 表需要连续槽位；
    /// 区段复用随帧槽整体回收，同 CBV/SRV/UAV 主堆的护栏判定）。
    bool AllocSamplerRange(u32 count, u32& outBaseSlot);

    // ------------------------------------------------------------------
    // 命令列表绑定（SetDescriptorHeaps 用；每帧/每列表一次）
    // ------------------------------------------------------------------
    ID3D12DescriptorHeap* GetCbvSrvUavHeap() const { return cbvSrvUavHeap_.Get(); }
    ID3D12DescriptorHeap* GetSamplerHeap() const { return samplerHeap_.Get(); }

    bool IsInitialized() const { return device_ != nullptr; }

    // ------------------------------------------------------------------
    // 容量访问（R19-4 断言布防用：写入/迁移点 assert(slot < capacity)，
    // 让 Windows Debug 下越界槽位崩在我们代码行而非 D3D12 运行时内部）
    // ------------------------------------------------------------------
    u32 CbvSrvUavCapacity() const { return cbvSrvUavCapacity_; }
    u32 SamplerCapacity() const { return samplerCapacity_; }

    // ------------------------------------------------------------------
    // 帧槽线性区状态（R21-C：set 戳记 + FRAME_TRACE 埋点。主堆/sampler
    // 堆两线性区同帧同 BeginFrame 推进，纪元恒同步——SlotEpoch 取主堆侧）
    // ------------------------------------------------------------------
    /// 指定帧槽的当前纪元（set 分配戳用；两线性区同步推进）。
    u64 SlotEpoch(u32 frameSlot) const { return mainLinear_.SlotEpoch(mainLinear_.SlotIndexOf(frameSlot)); }
    u32 MainLinearBump() const { return mainLinear_.Bump(); }
    u32 MainLinearCapacity() const { return mainLinear_.SlotCapacity(); }
    u32 MainLinearSlot() const { return mainLinear_.CurrentSlot(); }
    u32 MainHighWater(u32 slot) const { return mainLinear_.HighWaterFor(slot); }
    u32 SamplerLinearBump() const { return samplerLinear_.Bump(); }
    u32 SamplerLinearCapacity() const { return samplerLinear_.SlotCapacity(); }

private:
    /// 持久槽位 free-list（RTV/DSV/Sampler 共用实现）
    class SlotPool {
    public:
        void Init(u32 capacity);
        u32 Alloc();
        void Free(u32 slot);
    private:
        u32 next_ = 0;
        u32 capacity_ = 0;
        std::vector<u32> free_;
    };

    ID3D12Device* device_ = nullptr;  // 非持有（设备生命周期覆盖本管理器）

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cbvSrvUavHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> samplerHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    // shadow 堆（R13/T1）：与对应 visible 堆同容量/同槽位布局的非可见堆
    //（拷贝源 + Create*View 落点；语义见文件头双堆模型）。
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cbvSrvUavShadowHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> samplerShadowHeap_;

    u32 cbvSrvUavDescriptorSize_ = 0;
    u32 samplerDescriptorSize_ = 0;
    u32 rtvDescriptorSize_ = 0;
    u32 dsvDescriptorSize_ = 0;
    // 总容量镜像（R19-4：句柄/拷贝写入点断言上界；Initialize 从 config 落值）
    u32 cbvSrvUavCapacity_ = 0;
    u32 samplerCapacity_ = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cbvSrvUavCpuStart_{};
    D3D12_GPU_DESCRIPTOR_HANDLE cbvSrvUavGpuStart_{};
    D3D12_CPU_DESCRIPTOR_HANDLE samplerCpuStart_{};
    D3D12_GPU_DESCRIPTOR_HANDLE samplerGpuStart_{};
    D3D12_CPU_DESCRIPTOR_HANDLE cbvSrvUavShadowCpuStart_{};  // R13/T1
    D3D12_CPU_DESCRIPTOR_HANDLE samplerShadowCpuStart_{};    // R13/T1
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpuStart_{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvCpuStart_{};

    // 分帧槽线性分配状态（R21-C：bump/epoch/容量数学收拢进平台中立策略
    // 类——与 macOS 单测同源；currentSlot_ 与护拦表保留在管理器，
    // 供 OnSubmit 与 fence 复用判定使用）。
    u32 frameSlotCount_ = 3;
    u32 currentSlot_ = 0;
    // CBV/SRV/UAV 主堆布局：[0, cbvSrvUavPersistentCapacity_) 持久 SlotPool
    // （纹理 SRV/UAV 拷贝源）；其余为帧槽线性区段（set 实例表）。
    u32 cbvSrvUavLinearBase_ = 0;
    DescriptorFrameSlotPolicy mainLinear_;    ///< 主堆线性区（bump/epoch/high-water）
    // sampler 堆布局：[0, samplerPersistentCapacity_) 持久 SlotPool；
    // 其余为帧槽线性区段（set 实例的 sampler 表）。
    u32 samplerPersistentCapacity_ = 0;
    u32 samplerLinearBase_ = 0;
    DescriptorFrameSlotPolicy samplerLinear_; ///< sampler 堆线性区（同上）
    std::vector<u64> perSlotGuardTarget_;  ///< 各槽上次提交护拦（复用判定）

    SlotPool cbvSrvUavPersistentSlots_;
    SlotPool rtvSlots_;
    SlotPool dsvSlots_;
    SlotPool samplerSlots_;
};

} // namespace primal::graphics::rhi::d3d12
} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
