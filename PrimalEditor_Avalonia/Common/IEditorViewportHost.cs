using System;
using PrimalEditor_Avalonia.Views.Engine3D;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Cross-platform abstraction over a single editor 3D viewport. The Avalonia
/// control layer (EngineViewport3D.axaml.cs) holds an IEditorViewportHost and
/// drives Init/Resize/Destroy through it. Each platform implementation owns
/// the platform-specific steps: which RHI backend to bind, how to talk to the
/// native child-window handle, how to handle GPU device loss. The UI layer
/// never references RHIPlatformMetal / D3D12 / Vulkan directly.
///
/// Lifecycle:
///   1. Avalonia creates MTKViewHost (or platform equivalent) via XAML.
///   2. EngineViewport3D asks the factory for an IEditorViewportHost bound
///      to that native control host.
///   3. EngineViewport3D calls TryInitialize when the host has non-zero bounds.
///      Returns false if the underlying view is not yet ready (caller may
///      schedule a retry on the next layout pass).
///   4. On size change, call Resize.
///   5. On detached, call Destroy.
/// </summary>
public interface IEditorViewportHost
{
    /// <summary>Human-readable backend name ("Metal" / "D3D12" / "Vulkan").</summary>
    string Name { get; }

    /// <summary>
    /// Native child window handle (NSView / HWND / X11 Window). Zero before
    /// the host has been attached to the visual tree.
    /// </summary>
    IntPtr HostViewHandle { get; }

    /// <summary>True once the host view exists and TryInitialize succeeded.</summary>
    bool IsReady { get; }

    /// <summary>Engine-side surface id (0 if not yet created).</summary>
    int SurfaceId { get; }

    /// <summary>
    /// Create the engine render surface attached to <see cref="HostViewHandle"/>.
    /// Returns false when the host view is not yet attached or the engine
    /// refused the surface. Idempotent: subsequent calls with the same size
    /// return true without re-allocating.
    /// </summary>
    bool TryInitialize(int width, int height);

    /// <summary>Push the new size to the engine surface. No-op if not yet initialized.</summary>
    void Resize(int width, int height);

    /// <summary>Tear down the engine surface. Safe to call multiple times.</summary>
    void Destroy();

    /// <summary>
    /// Fires when the underlying GPU device is lost. Currently only D3D12 has
    /// a meaningful signal; macOS Metal does not raise it. Subscribers should
    /// re-create the surface and any cached GPU resources.
    /// </summary>
    event EventHandler? DeviceLost;
}

public static class EditorViewportHostFactory
{
    /// <summary>
    /// Create the platform-appropriate IEditorViewportHost bound to the
    /// given Avalonia native control host. The host's job is to expose the
    /// native child window handle (NSView / HWND / X11 Window).
    /// </summary>
    public static IEditorViewportHost CreateForCurrentPlatform(MTKViewHost mtkHost)
    {
        if (OperatingSystem.IsMacOS()) return new MacMetalViewportHost(mtkHost);
        if (OperatingSystem.IsWindows()) return new WindowsD3D12ViewportHost();
        return new LinuxVulkanViewportHost();
    }
}
