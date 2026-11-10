// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Pipeline.h — PSO + root signature（M1 F1-4）。
//
// ============================================================================
// 绑定模型契约：RHI descriptor set / binding → root signature 翻译
// （M1 冻结；与 Vulkan set0-global/set1-材质 + push constant 语义等价）
// ============================================================================
//
//   1. 每个 RHI descriptor set N ↔ HLSL register space N（与主链
//      glslang→spirv-cross 的 HLSL 输出一致：register(b/t/u/s<binding>,
//      space<set>)——反射契约测试按此断言）。
//   2. set N 的 bindings 先按 stageFlags 精确值分组，组内按类分组、类内
//      按 binding 升序切连续段：
//        CBV     (UniformBuffer / UniformBufferDynamic)          → b# range
//        SRV     (SampledImage / readonly Storage / texel)       → t# range
//        UAV     (StorageImage / StorageBuffer / StorageTexel)   → u# range
//        Sampler                                              → s# range（独立表）
//      每连续段 = 一个 D3D12_DESCRIPTOR_RANGE（RangeType/NumDescriptors/
//      BaseShaderRegister=段首 binding/RegisterSpace=N）。
//      H1 修复（stage 拆组）：连续段合并要求 stageFlags 完全相同——参数级
//      ShaderVisibility 是唯一的 stage 过滤面，异 stage 绑定并入同一
//      ALL 可见表会让 VS 可见 b1 与 VS-only root 常量 b1 同 stage 重叠
//      （CreateRootSignature 直接拒绝）；异 stage 一律拆独立表参数，
//      可见性 = ToVisibility(组 stageFlags)。sampler 段与 root 常量（b
//      空间）寄存器字母不同，恒无重叠，保持单表不拆。
//   3. Root parameter 排列顺序（root param index 由此确定）：
//        对每个 set（按 PipelineLayoutDesc.setLayouts 顺序）：
//          [CBV/SRV/UAV 表参数 × stageFlags 组数]（连续区间，组序=组内
//          首个 (类型序, binding)；该 set 无 main 绑定时省略）
//          [sampler 表参数]（该 set 无 sampler 绑定时省略）
//        尾部：[root constants]（push constant；M1 取首个非空 range，
//        ceil(size/4) 个 32 位常量，b<pushStage 可见 set0 CBV 数>/space0
//        ——与 DXC 对 spirv-cross HLSL push constant 的 next-free 落位
//        严格一致，见 BuildRootSignature 内注）。
//   4. 表参数在绑定期的取值：SetGraphics/ComputeRootDescriptorTable(
//        paramIndex, 该 D3D12DescriptorSet 在 shader-visible 堆中实例区段的
//        GPU 句柄)；set 实例区段内的描述符排列与段顺序一致（b 段、t 段、
//        u 段顺序 = range 声明顺序）。stage 拆组后同 set 各组表参数绑
//        同一区段起点句柄——range 的 OffsetInDescriptorsFromTableStart
//        是区段内绝对偏移，与表参数怎么拆分无关。
//   5. 64-DWORD 预算门禁：构造前经 D3D12RootSignatureBudget::
//      EvaluateRootSignature 显式校验，超限失败（不静默截断）。
//   6. 动态偏移（DescriptorSetBinding.dynamicOffsets）：M1 显式忽略
//      （一次性 Warn 日志）——D3D12 表内偏移在描述符写入时烘焙；帧槽化
//      更新路径（每帧 UpdateDescriptorSets）语义下等价。
//
// ============================================================================
// 跨后端三陷阱契约落点（阶段文档 §7.2 冻策）
// ============================================================================
//
//   - CullMode（§7.2-1 修订，H2 修复）：D3D12 侧 viewport 为负高度
//     {0,H,W,-H}（CommandBuffer::SetViewport）补偿 NDC Y 语义差
//     （Vulkan Y-down vs D3D12 Y-up），光栅化绕序与 Vulkan
//     （frontFace=CW）完全一致；D3D12 默认 FrontCounterClockwise=FALSE
//     即 CW=正面，同语义 → PSO cull mode 直译：RHI Front→D3D12_FRONT，
//     RHI Back→D3D12_BACK，None→NONE。历史版本曾在此再翻一次 cull
//     （双重翻转）导致正面被整体剔除（全黑帧根因 H2），已移除。
//   - 深度（§7.2-2）：[0,1] 正向 Z，Less（清 1.0）直译，无 reverse-Z。
//   - 色彩空间（§7.2-3）：离屏 BGRA8 UNorm 直读，sRGB 呈现随 M7。
// ============================================================================
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include "../../Core/RHIDevice.h"
#include "../../Core/RHIPipelineLayout.h"

namespace primal::graphics::rhi {

class D3D12Device;

namespace d3d12 {

/// set → root parameter 映射信息（BindDescriptorSets 的翻译表）。
struct D3D12SetRootParams {
    u32 tableParam = kInvalidRootParam;          ///< 首个 CBV/SRV/UAV 表参数
                                                 ///<（H1 stage 拆组后为
                                                 ///< 连续区间起点）
    u32 tableParamCount = 0;                     ///< main 表参数个数（区间
                                                 ///< 长度；同 set 各组在
                                                 ///< 绑定期设同一区段句柄）
    u32 samplerTableParam = kInvalidRootParam;   ///< sampler 表参数
    u32 descriptorCount = 0;                     ///< 合并表描述符数
    u32 samplerCount = 0;                        ///< sampler 表描述符数
    static constexpr u32 kInvalidRootParam = 0xFFFFFFFFu;
};

/// 从 PipelineLayoutDesc 构建的 root signature + 参数映射。
struct D3D12RootSignatureLayout {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    std::vector<D3D12SetRootParams> sets;   ///< 下标 = PipelineLayoutDesc 序
    u32 constantsParam = D3D12SetRootParams::kInvalidRootParam;
    u32 constantsCount32 = 0;
    bool isCompute = false;
};

/// 构建 + 序列化 root signature（图形/计算共用；预算门禁内置）。
/// setLayouts 句柄经 device 解析为 D3D12DescriptorSetLayout 绑定表。
bool BuildRootSignature(D3D12Device& device, const PipelineLayoutDesc& desc,
                        bool isCompute, D3D12RootSignatureLayout& out);

/// D3D12PipelineLayout — RHIPipelineLayout 承载体（CreatePipelineLayout 句柄）。
class D3D12PipelineLayout : public RHIPipelineLayout {
    friend class primal::graphics::rhi::D3D12Device;
    friend class D3D12CommandBufferImpl;
public:
    D3D12PipelineLayout(D3D12Device& device, const PipelineLayoutDesc& desc);
    D3D12PipelineLayout(D3D12PipelineLayout&&) = delete;
    D3D12PipelineLayout& operator=(D3D12PipelineLayout&&) = delete;
    D3D12PipelineLayout(const D3D12PipelineLayout&) = delete;
    D3D12PipelineLayout& operator=(const D3D12PipelineLayout&) = delete;
    virtual ~D3D12PipelineLayout() { destroyImpl(); }

    bool Initialize() override;

    ID3D12RootSignature* GetRootSignature() const {
        return layout_.rootSignature.Get();
    }
    const D3D12RootSignatureLayout& GetRootLayout() const { return layout_; }

protected:
    void destroyImpl() override;
    void* mapImpl(u64, u64) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, u64, u64) override { return false; }

private:
    D3D12RootSignatureLayout layout_;
};

/// D3D12Pipeline — 图形/计算 PSO。
class D3D12Pipeline {
    friend class primal::graphics::rhi::D3D12Device;
    friend class D3D12CommandBufferImpl;
public:
    explicit D3D12Pipeline(D3D12Device& device);
    D3D12Pipeline(D3D12Pipeline&& other) noexcept;
    D3D12Pipeline& operator=(D3D12Pipeline&& other) noexcept;
    D3D12Pipeline(const D3D12Pipeline&) = delete;
    D3D12Pipeline& operator=(const D3D12Pipeline&) = delete;
    ~D3D12Pipeline();

    bool InitializeGraphics(const GraphicsPipelineDesc& desc);
    bool InitializeCompute(const ComputePipelineDesc& desc);
    void Destroy();

    ID3D12PipelineState* GetPso() const { return pipeline_.Get(); }
    ID3D12RootSignature* GetRootSignature() const;
    /// 布局现查（fail-closed）：每次调用经 device_.GetPipelineLayout(
    /// layoutHandle_) 解析——pipeline layout pool 扩容会整体搬迁槽位
    /// （RHIAllocator::Get 返回的地址仅单次调用内有效），本类不缓存
    /// D3D12PipelineLayout* 裸地址；句柄有效但解析失败（布局已销毁）
    /// → nullptr + Error 日志。
    D3D12PipelineLayout* GetPipelineLayout() const;
    /// 布局句柄直读（不解析、不落地址）——CommandBuffer 绑定镜像存句柄用。
    PipelineLayoutHandle GetLayoutHandle() const { return layoutHandle_; }

    /// BindGraphicsPipeline/BindComputePipeline 时的拓扑设置依据。
    D3D_PRIMITIVE_TOPOLOGY GetIaTopology() const { return iaTopology_; }

    bool IsCompute() const { return isCompute_; }
    PipelineHandle GetHandle() const { return handle_; }
    void SetHandle(PipelineHandle h) { handle_ = h; }
    const GraphicsPipelineDesc& GetGraphicsDesc() const { return graphicsDesc_; }

private:
    /// PSO 创建失败路径诊断（仅 Create*PipelineState FAILED 分支调用，
    /// 成功路径零开销）：一次聚合输出失败 hr + pso 关键面（root signature
    /// null 性与句柄现查对照 / 五级字节码指针非空性与长度 / RT-DSV-拓扑-
    /// 光栅化-深度-混合-输入布局-采样-杂项全字段）+ InfoQueue
    /// [storedBefore, now) 窗口非破坏性排水——debug layer 对 0x80070057
    /// 必有具体违规消息，是决定性证据。窗口基线在 Create 调用前采集；
    /// 只读 GetMessage，不清存储、不动设备侧 drain 周期的计数基线
    ///（D3D12Device::DrainInfoQueueMessages 为 private 且 drain 后清空，
    /// 不可复用——见 D3D12Pipeline.cpp 实现处注释）。
    void DumpGraphicsPsoFailureDiag(HRESULT hr,
                                    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pso,
                                    ID3D12InfoQueue* infoQueue,
                                    u64 storedBefore) const;
    void DumpComputePsoFailureDiag(HRESULT hr,
                                   const D3D12_COMPUTE_PIPELINE_STATE_DESC& pso,
                                   ID3D12InfoQueue* infoQueue,
                                   u64 storedBefore) const;

    D3D12Device& device_;
    PipelineHandle handle_{handles::INVALID_PIPELINE};
    bool isCompute_{false};
    /// desc.layout 原句柄（非拥有；设备 pool 持有布局本体）。pool 扩容
    /// （如第 4 个布局触发 3→6 realloc）会整体搬迁槽位，缓存的
    /// D3D12PipelineLayout* 裸指针随即悬垂（M3 BindGraphicsPipeline null
    /// root signature 崩溃根因）——一律存句柄，用时经
    /// device_.GetPipelineLayout(layoutHandle_) 现查取新地址。
    PipelineLayoutHandle layoutHandle_{handles::INVALID_PIPELINE_LAYOUT};
    /// desc.layout 无效时自建空布局。该路径经 BuildRootSignature 直建
    /// ID3D12RootSignature（不经 device 的 pipeline layout pool，无句柄），
    /// 生命周期由本类 emptyRootSignature_ ComPtr 自持——保持现状。
    bool ownsLayout_ = false;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> emptyRootSignature_;  ///< ownsLayout_ 时的空 RS
    D3D12_PRIMITIVE_TOPOLOGY iaTopology_{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    GraphicsPipelineDesc graphicsDesc_{};
    ComputePipelineDesc computeDesc_{};
};

} // namespace primal::graphics::rhi::d3d12
} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
