# P4c-F2 Swapchain 人工验收清单

> 自动化覆盖:`TestVulkanSwapChain::SwapChainResizeRecovery`(程序化 resize
> 10 次,检查点 SSIM=1.0,重建真实触发)。本清单覆盖自动化无法验证的
> 人眼观感项(撕裂/黑帧/卡死)。CI 无显示环境时自动化用例 skip,本清单
> 成为唯一验收手段。

## 环境

- [ ] macOS + 窗口模式(RHIPlatform::Vulkan,MoltenVK)
- [ ] 构建配置:Debug + validation 开启(`DeviceDesc.enableValidation = true`)

## 步骤

1. 运行任一使用 VulkanSwapChain 的开窗用例(或最小 demo),保持 ≥60 FPS 渲染动画。
2. **拖拽 resize 窗口边缘 20 次**(不同方向、含快速来回),观察:
   - [ ] 无永久黑帧(短暂一帧的内容滞后可接受)
   - [ ] 无撕裂
   - [ ] 无卡死/断流(resize 结束后帧率恢复)
   - [ ] 控制台出现 `[VulkanSwapChain] Recreate: WxH` 日志且渲染继续
   - [ ] 零 validation error
3. **全屏切换 2 次**(绿色窗口按钮进出全屏),同上观察项。
4. 每种尺寸下截取一帧存入本目录:
   - [ ] `manual_resize_01.png` … `manual_resize_20.png`
   - [ ] `manual_fullscreen_enter.png` / `manual_fullscreen_exit.png`

## 结果记录

- 日期:
- 机器/GPU:
- MoltenVK 版本:
- 结论(通过/不通过 + 备注):
