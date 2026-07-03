using System;
using Avalonia.Controls;
using Avalonia.Platform;

namespace PrimalEditor_Avalonia.Views.Engine3D;

/// <summary>
/// A <see cref="NativeControlHost"/> that hosts the engine's MTKView as a
/// child NSView. The engine receives the host's child NSView handle and
/// addSubview:'s its PassThroughMTKView onto it. This is the proper
/// Avalonia 11 way to embed a platform-specific view: the host participates
/// in Avalonia's layout (the engine can query its bounds via
/// <c>Bounds</c>), the host is in the visual tree so AppKit knows where
/// to draw it, and the underlying NSView is sized correctly. Attaching
/// the MTKView directly to the window's contentView (the legacy path)
/// bypasses Avalonia's layout system, which is the root cause of the
/// macOS 26.5.1 <c>com.apple.FramePacing.LayerStateSyncQueue</c>
/// NULL function pointer crash and the 10-frame-then-deadlock
/// pattern observed in the editor.
///
/// On macOS the <c>CreateNativeControlCore</c> override returns
/// <c>base.CreateNativeControlCore(parent)</c> — Avalonia creates a
/// child NSView inside its offscreen parent pool, which we then hand
/// to the engine. The engine addSubview:'s its PassThroughMTKView
/// onto that child NSView, so the MTKView is a proper subview of
/// Avalonia's managed native subtree and inherits the correct
/// coordinate system + layout pass.
/// </summary>
public sealed class MTKViewHost : NativeControlHost
{
    /// <summary>
    /// The NSView handle (id / void*) of the child NSView that Avalonia
    /// created for this host. Set by <see cref="CreateNativeControlCore"/>.
    /// The engine receives this handle via
    /// <c>CreateEmbeddedRenderSurface(host, w, h)</c> and addSubview:'s
    /// its PassThroughMTKView to it.
    /// </summary>
    public IntPtr HostViewHandle { get; private set; } = IntPtr.Zero;

    protected override IPlatformHandle CreateNativeControlCore(IPlatformHandle parent)
    {
        // Avalonia creates a child NSView and returns its handle.
        // The "default" child is an NSView with a CALayer backing; the
        // engine will addSubview: a PassThroughMTKView on top of it.
        // Returning base.CreateNativeControlCore(parent) gives us the
        // official Avalonia-handled NSView (the same one Avalonia
        // would use for a "default" native control) — this NSView
        // participates in Avalonia's layout and is positioned at the
        // host control's bounds.
        var child = base.CreateNativeControlCore(parent);
        HostViewHandle = child.Handle;
        return child;
    }
}
