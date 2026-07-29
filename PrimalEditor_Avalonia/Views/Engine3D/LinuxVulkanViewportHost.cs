using System;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// Linux/X11 placeholder for the Vulkan host. Not yet implemented. See
/// <see cref="WindowsD3D12ViewportHost"/> for the equivalent Windows stub —
/// the factory returns platform-appropriate stubs so cross-platform builds
/// compile.
/// </summary>
public sealed class LinuxVulkanViewportHost : IEditorViewportHost
{
    public string Name => "Vulkan";
    public IntPtr HostViewHandle => IntPtr.Zero;
    public bool IsReady => false;
    public int SurfaceId => 0;

    public bool TryInitialize(int width, int height)
        => throw new NotImplementedException("Vulkan viewport not yet wired in Avalonia editor");

    public void Resize(int width, int height) { }
    public void Destroy() { }
    public event EventHandler? DeviceLost
    {
        add { }
        remove { }
    }
}
