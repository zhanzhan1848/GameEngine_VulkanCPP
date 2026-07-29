using System;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// macOS implementation of <see cref="IEditorViewportHost"/>. Owns the Metal
/// RHI initialization and the embedded render surface lifecycle. The UI layer
/// never references Metal or RHIPlatformMetal directly — that constant is
/// read once, here, when the surface is created.
/// </summary>
public sealed class MacMetalViewportHost : IEditorViewportHost
{
    private readonly MTKViewHost _mtkHost;
    private uint _surfaceId = uint.MaxValue;
    private int _lastWidth;
    private int _lastHeight;

    public MacMetalViewportHost(MTKViewHost mtkHost)
    {
        _mtkHost = mtkHost ?? throw new ArgumentNullException(nameof(mtkHost));
    }

    public string Name => "Metal";

    public IntPtr HostViewHandle => _mtkHost.HostViewHandle;

    public bool IsReady => _surfaceId != uint.MaxValue;

    public int SurfaceId => _surfaceId == uint.MaxValue ? 0 : (int)_surfaceId;

    public bool TryInitialize(int width, int height)
    {
        if (_surfaceId != uint.MaxValue) return true;
        if (width <= 0 || height <= 0) return false;
        if (!NativeEngineBridge.IsLoaded) return false;

        var hostView = _mtkHost.HostViewHandle;
        if (hostView == IntPtr.Zero) return false;

        if (!NativeEngineBridge.InitializeEngine(NativeEngineBridge.DefaultRHIPlatformForCurrentPlatform(), enableDebug: false))
        {
            Logger.Warning($"MacMetalViewportHost: InitializeEngine({NativeEngineBridge.DefaultRHIPlatformForCurrentPlatform()}) returned false");
            return false;
        }

        _surfaceId = NativeEngineBridge.CreateEmbeddedRenderSurface(hostView, width, height);
        if (_surfaceId == uint.MaxValue)
        {
            Logger.Warning("MacMetalViewportHost: CreateEmbeddedRenderSurface failed");
            return false;
        }

        // Defensive double-check: the C ABI may return a non-error id even when
        // the host view had 0x0 bounds at creation time. Refuse to mark ready
        // if no MTKView is actually attached — the caller will retry.
        if (NativeEngineBridge.GetSurfaceView(_surfaceId) == IntPtr.Zero)
        {
            Logger.Warning($"MacMetalViewportHost: surface {_surfaceId} created but no MTKView attached (host likely had 0x0 bounds)");
            NativeEngineBridge.RemoveRenderSurface(_surfaceId);
            _surfaceId = uint.MaxValue;
            return false;
        }

        _lastWidth = width;
        _lastHeight = height;
        Logger.Info($"MacMetalViewportHost: surface {_surfaceId} ready ({width}x{height})");
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

    public event EventHandler? DeviceLost
    {
        add { /* Metal has no such signal in this engine build */ }
        remove { }
    }
}
