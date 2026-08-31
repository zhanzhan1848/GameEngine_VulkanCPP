using System;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// Windows placeholder for the D3D12 host. The Avalonia editor on Windows
/// has not yet been wired up (no HWND-binding code path). The factory
/// returns this stub so cross-platform builds compile; at runtime the
/// EngineViewport3D catches the NotImplementedException and degrades the
/// viewport to a placeholder.
/// </summary>
public sealed class WindowsD3D12ViewportHost : IEditorViewportHost
{
    public string Name => "D3D12";
    public IntPtr HostViewHandle => IntPtr.Zero;
    public bool IsReady => false;
    public int SurfaceId => 0;

    public bool TryInitialize(int width, int height)
        => throw new NotImplementedException("D3D12 viewport not yet wired in Avalonia editor");

    public void Resize(int width, int height) { }
    public void Destroy() { }
    public event EventHandler? DeviceLost
    {
        add { }
        remove { }
    }
}
