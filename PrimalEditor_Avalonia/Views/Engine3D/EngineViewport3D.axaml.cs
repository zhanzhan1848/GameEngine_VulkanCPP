using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Views;

public partial class EngineViewport3D : UserControl
{
    private NativeControlHost? _nativeHost;
    private IntPtr _embeddedView;
    private bool _isInitialized;

    public EngineViewport3D()
    {
        InitializeComponent();
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
        _nativeHost = this.FindControl<NativeControlHost>("NativeHost");
        if (_nativeHost != null)
        {
            _nativeHost.AttachedToVisualTree += OnNativeHostAttached;
            _nativeHost.DetachedFromVisualTree += OnNativeHostDetached;
        }
    }

    private void OnNativeHostAttached(object? sender, VisualTreeAttachmentEventArgs e)
    {
        if (_isInitialized) return;
        if (_nativeHost == null) return;

        var placeholder = this.FindControl<TextBlock>("Placeholder");
        if (placeholder != null) placeholder.IsVisible = false;

        if (!NativeEngineBridge.IsLoaded)
        {
            Logger.Info("Engine not loaded - showing 3D placeholder");
            if (placeholder != null) placeholder.IsVisible = true;
            return;
        }

        try
        {
            var hostHandle = GetNativeHostHandle(_nativeHost);
            if (hostHandle == IntPtr.Zero)
            {
                Logger.Warning("NativeControlHost handle not yet available - deferring to next frame");
                Dispatcher.UIThread.Post(() => OnNativeHostAttached(sender, e),
                    Avalonia.Threading.DispatcherPriority.Background);
                return;
            }

            _isInitialized = true;

            var width = (int)Bounds.Width > 0 ? (int)Bounds.Width : 800;
            var height = (int)Bounds.Height > 0 ? (int)Bounds.Height : 600;

            _embeddedView = NativeEngineBridge.CreateEmbeddedRenderView(hostHandle, width, height);
            if (_embeddedView == IntPtr.Zero)
            {
                Logger.Warning("CreateEmbeddedRenderView failed - showing placeholder");
                _isInitialized = false;
                if (placeholder != null) placeholder.IsVisible = true;
            }
            else
            {
                Logger.Info($"3D viewport embedded: view 0x{_embeddedView:X} ({width}x{height})");
            }
        }
        catch (Exception ex)
        {
            Logger.Error("Failed to create embedded render view", ex);
            if (placeholder != null) placeholder.IsVisible = true;
        }
    }

    private static IntPtr GetNativeHostHandle(NativeControlHost host)
    {
        try
        {
            var field = typeof(NativeControlHost).GetField("_handle",
                System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
            if (field != null)
            {
                var ph = field.GetValue(host) as Avalonia.Platform.IPlatformHandle;
                if (ph != null && ph.Handle != IntPtr.Zero) return ph.Handle;
            }

            var prop = typeof(NativeControlHost).GetProperty("Handle",
                System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
            if (prop != null)
            {
                var ph = prop.GetValue(host) as Avalonia.Platform.IPlatformHandle;
                if (ph != null && ph.Handle != IntPtr.Zero) return ph.Handle;
            }

            foreach (var p in typeof(NativeControlHost).GetProperties(
                System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance))
            {
                if (p.Name.ToLower().Contains("handle") || p.Name.ToLower().Contains("host"))
                {
                    try
                    {
                        var v = p.GetValue(host);
                        if (v is Avalonia.Platform.IPlatformHandle ph && ph.Handle != IntPtr.Zero)
                            return ph.Handle;
                    }
                    catch { }
                }
            }
        }
        catch { }

        var topLevel = TopLevel.GetTopLevel(host);
        var topHandle = topLevel?.TryGetPlatformHandle();
        if (topHandle == null || topHandle.Handle == IntPtr.Zero)
            return IntPtr.Zero;

        return topHandle.Handle;
    }

    private void OnNativeHostDetached(object? sender, VisualTreeAttachmentEventArgs e)
    {
        if (_embeddedView != IntPtr.Zero)
        {
            NativeEngineBridge.DestroyEmbeddedRenderView(_embeddedView);
            _embeddedView = IntPtr.Zero;
        }
        _isInitialized = false;
    }

    protected override void OnSizeChanged(SizeChangedEventArgs e)
    {
        base.OnSizeChanged(e);
        if (_embeddedView != IntPtr.Zero && NativeEngineBridge.IsLoaded)
        {
            var width = (int)Bounds.Width > 0 ? (int)Bounds.Width : 800;
            var height = (int)Bounds.Height > 0 ? (int)Bounds.Height : 600;
            NativeEngineBridge.ResizeEmbeddedRenderView(_embeddedView, width, height);
        }
    }
}