# PrimalEditor Avalonia — P1 跨平台视口/RHI 抽象

> **For Claude:** P0 修复之后，跨平台视口是 Avalonia 迁移的核心价值。
> 本计划聚焦"分离 Avalonia 控件 / 原生窗口句柄 / RHI 后端"——让 UI 层不出现 `RHIPlatformMetal` 字面量。
> 不替代 `2026-07-28-avalonia-feature-parity.md` 中的任何已实现项；属于补完 P0 计划第 5 项。

## 背景

当前 `PrimalEditor_Avalonia/Views/Engine3D/EngineViewport3D.axaml.cs` 中：
- line 102：`NativeEngineBridge.InitializeEngine(NativeEngineBridge.RHIPlatformMetal, enableDebug: false)` 硬编码 Metal
- line 168：`NativeEngineBridge.CreateEmbeddedRenderSurface(hostView, w, h)` 隐含 macOS host
- line 514：`NativeEngineBridge.RemoveRenderSurface(surfaceId)` macOS path
- line 558：`NativeEngineBridge.ResizeRenderSurface(...)` macOS path
- `Program.cs:93`：`NativeEngineBridge.InitializeEngine(NativeEngineBridge.RHIPlatformMetal, false)`

`RHIPlatformMetal` 是 C ABI 表面常量（值 3，见 `NativeEngineBridge.cs:908`）。UI 层不应**直接读**这个常量——它属于 RHI 后端选择。Windows 的 `RHIPlatformD3D12=1`、Linux 的 `RHIPlatformVulkan=2` 都未实现，但接口应该为它们留位置。

## 目标

1. UI 层（`EngineViewport3D.axaml.cs`、`Program.cs`）**不出现** `RHIPlatformMetal` 字面量、不出现 `RHIPlatform` 枚举。
2. 抽出 `IEditorViewportHost`：UI 持有 `IEditorViewportHost`，通过它 Init/Resize/Destroy。
3. `MacMetalViewportHost` 内部决定 RHI 后端，封装 Metal 特定的所有 native 调用。
4. `WindowsD3D12ViewportHost`、`LinuxVulkanViewportHost` 提供**接口契约的编译期占位**——保证跨平台构建时所有平台实现都"存在"，但运行期由 `EditorViewportHostFactory` 按平台选择。
5. `DeviceLost` 事件存在于接口中（Windows D3D12 需要；macOS Metal 当前不需要，但接口必须预留）。

## 不做

- **不**重写 C++ 引擎端 C ABI。
- **不**实现 Windows D3D12 / Linux Vulkan 的 host（仅占位 + `NotImplementedException`）。
- **不**改 `MTKViewHost` 本身（它继续负责 Avalonia 11 `NativeControlHost` 集成，跨平台概念无影响）。
- **不**触碰 RHI device lost 信号在 macOS 上的具体实现（Metal 不需要）。
- **不**改 `EngineViewport3D` 的鼠标/键盘交互（oribit/pan/zoom/key handler）——只解耦 RHI/平台代码。

## 接口

`PrimalEditor_Avalonia/Common/IEditorViewportHost.cs`：

```csharp
public interface IEditorViewportHost
{
    string Name { get; }                  // "Metal" / "D3D12" / "Vulkan"
    IntPtr HostViewHandle { get; }        // native child window handle (NSView/HWND/X11)
    bool IsReady { get; }                 // host view has non-zero bounds + surface created
    int SurfaceId { get; }                // engine-side surface id, or 0 if not yet created

    bool TryInitialize(int width, int height);  // returns false if host view not ready (caller may retry)
    void Resize(int width, int height);
    void Destroy();
    event EventHandler? DeviceLost;       // fires when GPU device is reset (D3D12 needs this; Metal not yet)
}
```

工厂：

```csharp
public static class EditorViewportHostFactory
{
    public static IEditorViewportHost CreateForCurrentPlatform();
}
```

按 `OperatingSystem` 决定：`OSX` → `MacMetalViewportHost`；`Windows` → `WindowsD3D12ViewportHost`（占位）；其他 → `LinuxVulkanViewportHost`（占位）。

## 文件清单

### 新建

- `PrimalEditor_Avalonia/Common/IEditorViewportHost.cs` — 接口 + 工厂
- `PrimalEditor_Avalonia/Views/Engine3D/MacMetalViewportHost.cs` — macOS Metal 实现（封装所有 macOS-specific 调用）
- `PrimalEditor_Avalonia/Views/Engine3D/WindowsD3D12ViewportHost.cs` — Windows 占位
- `PrimalEditor_Avalonia/Views/Engine3D/LinuxVulkanViewportHost.cs` — Linux 占位

### 修改

- `PrimalEditor_Avalonia/Views/Engine3D/EngineViewport3D.axaml.cs` — 移除硬编码；持有 `IEditorViewportHost`
- `PrimalEditor_Avalonia/Program.cs:93` — `InitializeEngine(RHIPlatformMetal)` 调用保持（C ABI 端初始化，不在 UI 层；这是合法使用）。但 `EngineViewport3D.axaml.cs:102` 删除（host 自己初始化）。

## MacMetalViewportHost 实现要点

```csharp
public sealed class MacMetalViewportHost : IEditorViewportHost
{
    private readonly MTKViewHost _mtkHost;  // Avalonia 11 NativeControlHost
    private uint _surfaceId = uint.MaxValue;
    private int _lastWidth, _lastHeight;

    public MacMetalViewportHost(MTKViewHost mtkHost) { _mtkHost = mtkHost; }

    public string Name => "Metal";
    public IntPtr HostViewHandle => _mtkHost.HostViewHandle;
    public bool IsReady => _surfaceId != uint.MaxValue;
    public int SurfaceId => (int)_surfaceId;

    public bool TryInitialize(int width, int height)
    {
        if (_surfaceId != uint.MaxValue) return true;  // already initialized
        var hostView = _mtkHost.HostViewHandle;
        if (hostView == IntPtr.Zero) return false;

        // Ensure engine + pipeline are up. Engine init is idempotent.
        if (!NativeEngineBridge.InitializeEngine(NativeEngineBridge.RHIPlatformMetal, false))
            return false;

        _surfaceId = NativeEngineBridge.CreateEmbeddedRenderSurface(hostView, width, height);
        if (_surfaceId == uint.MaxValue) return false;

        // Defensive: confirm MTKView is actually attached.
        if (NativeEngineBridge.GetSurfaceView(_surfaceId) == IntPtr.Zero)
        {
            NativeEngineBridge.RemoveRenderSurface(_surfaceId);
            _surfaceId = uint.MaxValue;
            return false;
        }

        _lastWidth = width;
        _lastHeight = height;
        return true;
    }

    public void Resize(int width, int height)
    {
        if (_surfaceId == uint.MaxValue) return;
        if (width <= 0 || height <= 0) return;
        if (width == _lastWidth && height == _lastHeight) return;
        NativeEngineBridge.ResizeRenderSurface(_surfaceId, (uint)width, (uint)height);
        NativeEngineBridge.SetEmbeddedSurfaceFrame(_surfaceId, 0, 0, (uint)width, (uint)height);
        _lastWidth = width;
        _lastHeight = height;
    }

    public void Destroy()
    {
        if (_surfaceId == uint.MaxValue) return;
        NativeEngineBridge.RemoveRenderSurface(_surfaceId);
        _surfaceId = uint.MaxValue;
    }

    public event EventHandler? DeviceLost;  // Metal currently has no such signal
}
```

注意：此处**保留** `NativeEngineBridge.RHIPlatformMetal` 字面量——它在 `Native` 命名空间内、属于 native 桥接层调用。`EngineViewport3D.axaml.cs` 和 `Program.cs` 中的 `RHIPlatformMetal` 都删除。

## EngineViewport3D 解耦要点

- 构造函数中 `MTKViewHost` 改名为 `MTKHostView`（XAML 里已有，保留）。
- 持有 `IEditorViewportHost _host;` 字段。
- 构造函数末尾：`if (NativeEngineBridge.IsLoaded) _host = EditorViewportHostFactory.CreateForCurrentPlatform(MTKHostView);`
- `TryInitializeFromLayout` 中替换为 `_host.TryInitialize(width, height)`。
- `OnLayoutUpdated` 中替换为 `_host.Resize(width, height)`。
- `OnDetached` 中替换为 `_host.Destroy()`。
- 暴露 `public IEditorViewportHost? Host => _host;` 给外部（如 render loop）使用。

## Windows / Linux 占位

```csharp
public sealed class WindowsD3D12ViewportHost : IEditorViewportHost
{
    public WindowsD3D12ViewportHost() { }
    public string Name => "D3D12";
    public IntPtr HostViewHandle => IntPtr.Zero;
    public bool IsReady => false;
    public int SurfaceId => 0;
    public bool TryInitialize(int w, int h) => throw new NotImplementedException("D3D12 not yet wired in Avalonia editor");
    public void Resize(int w, int h) { }
    public void Destroy() { }
    public event EventHandler? DeviceLost;
}
```

不抛 `PlatformNotSupportedException`——`NotImplementedException` 让 `EngineViewport3D` 在 Windows 上 `catch` 后优雅降级（"viewport not supported on this platform"）而不是崩溃 UI。

## 验证

1. `dotnet build` 0 错误。
2. 视觉/运行时验证：与 P0 一样，smoke test 在 macOS 上跑。但 `avares:` URI 错误是已存在，**不预期本轮修复**。
3. `grep -rn "RHIPlatformMetal" PrimalEditor_Avalonia --include='*.cs'` 应该只在 `Native/NativeEngineBridge.cs`（定义）和 `Views/Engine3D/MacMetalViewportHost.cs`（host 内部使用）出现；**`Program.cs` 和 `EngineViewport3D.axaml.cs` 不应出现**。
4. 单元自测：构造 `MacMetalViewportHost` with fake host → 调用顺序在 engine 不可用时正确短路（不抛）。

## 风险

- **`MTKViewHost` 在 `EngineViewport3D` XAML 中以 `Name="MTKHostView"` 引用**，本轮保留 XAML 不改；只是把它的"使用"从 EngineViewport3D 移到 host 内部。
- **Engine init 顺序**：原来 `Program.cs:93` 先 InitializeEngine（早于 UI 启动），`EngineViewport3D.axaml.cs:102` 是冗余幂等调用。改造后 MacMetalViewportHost.TryInitialize 内部调用 `InitializeEngine(RHIPlatformMetal, false)`，并依赖 `NativeEngineBridge.IsLoaded` 在 host 调用前为 true（由 `Program.cs:93` 早于 UI 保证）。
- **重新打开 tab / 重新 init**：原来通过 `LayoutUpdated` retry；新方案 host.TryInitialize 返回 false 时 EngineViewport3D 仍然 schedule retry（host 不知道也不需要知道 retry 策略）。

## 实施顺序

1. IEditorViewportHost + factory（Common）
2. MacMetalViewportHost（封装 macOS path）
3. Windows / Linux 占位（throw NotImplementedException）
4. EngineViewport3D 解耦
5. Program.cs 不需改（InitializeEngine 在 Program 早期仍是合法调用）
6. 编译 + 验证 grep
7. P1 lessons 追加
