// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12BackendLinks.h — 后端注册对象的链接锚点（M1；M0 §5.1 教训复刻）。
//
// 背景：注册式桥（RHICaptureBridge / RHIPassTimestampAudit）的静态注册对象
// 若无符号引用会被链接器从静态库裁剪（Vulkan 实测：archive 17 符号 /
// 最终二进制 0）。修法 = 本头声明锚点函数（定义在 D3D12BackendLinks.cpp），
// 由 D3D12Device::initializeImpl 显式调用——设备必然被链接，锚点引用强制
// 拉入注册 TU。调用幂等。
//
// M1 注册策略（不静默的 not-supported，见 D3D12BackendLinks.cpp 注释）：
//   - RHICaptureBridge：不注册 resolver（BackendCaptureSupported=false 是
//     中立层的"后端不支持"显式语义；注册一个恒 false 的 resolver 反而会把
//     该门翻成 true，令 capture 层产出空证据）。锚点调用时一次性日志
//     声明状态。
//   - RHIPassTimestampAudit：注册显式 not-supported Entry（Begin 返回 0
//     = "未启用/被拒" 契约语义；其余方法 no-op）——调用方可观测非静默。
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

namespace primal::graphics::rhi {

/// 强制拉入 D3D12BackendLinks.cpp（capture 状态声明 + timestamp
/// not-supported Entry 注册）。幂等；initializeImpl 调用。
void EnsureD3D12BackendLinksLinked();

} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
