using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Native.MaterialGraph;

namespace PrimalEditor_Avalonia.Editors.Material;

/// <summary>A user-facing material preview primitive.</summary>
public sealed record MaterialPreviewMeshOption(
    PreviewModelType Model,
    string DisplayName,
    string Glyph,
    string Description);

/// <summary>
/// Backs the 3D preview pane in the material editor. Owns the native
/// <c>MaterialPreviewRenderer</c> handle, an RGBA8 byte buffer pinned for
/// the lifetime of the VM, and the orbit-camera state.
///
/// <para>
/// Each tick (<see cref="DispatchTimerTick"/>) we ask the native side to
/// render the preview into the pinned buffer via
/// <see cref="MaterialPreviewNative.RenderToRGBA"/>, then swap the
/// <see cref="PreviewBitmap"/> property to a fresh <see cref="WriteableBitmap"/>
/// pointing at the same address. Avalonia reads the new content for the next paint.
/// </para>
/// </summary>
public sealed class MaterialPreviewViewModel : ObservableObject, IDisposable
{
    /// <summary>
    /// The standard primitive set exposed by Unreal's Material Editor.
    /// Additional native preview meshes remain available through the API.
    /// </summary>
    public static IReadOnlyList<MaterialPreviewMeshOption> StandardPreviewMeshes { get; } =
    [
        new(PreviewModelType.Sphere,   "Sphere",   "●", "Preview on a sphere"),
        new(PreviewModelType.Cylinder, "Cylinder", "▰", "Preview on a cylinder"),
        new(PreviewModelType.Plane,    "Plane",    "▱", "Preview on a flat plane"),
        new(PreviewModelType.Box,      "Cube",     "◆", "Preview on a cube / box"),
    ];

    public IReadOnlyList<MaterialPreviewMeshOption> AvailablePreviewMeshes => StandardPreviewMeshes;

    /// <summary>Native-side texture size. Keep ≤ 512 to keep the readback cheap.</summary>
    public const int PreviewWidth = 384;
    public const int PreviewHeight = 384;

    private const float DefaultRadius = 4.5f;
    private const float MinRadius = 1.5f;
    private const float MaxRadius = 12.0f;
    private const float MinPitch = -1.4f;   // ~ -80°
    private const string DefaultGraphJson =
        "{\"nodes\":[{\"type\":\"MaterialOutput\",\"x\":0,\"y\":0}],\"connections\":[]}";
    private const float MaxPitch = 1.4f;    // ~ +80°

    /// <summary>Yaw/pitch radians per pixel of pointer movement.</summary>
    private const double OrbitSensitivity = 0.005;
    /// <summary>Radius change per wheel-tick (positive = zoom in).</summary>
    private const double ZoomSensitivity = 0.001;

    private readonly uint _previewId;
    private readonly ulong _deviceHandle;
    private readonly byte[] _pixels;        // length = W*H*4
    private readonly IntPtr _pixelsAddr;
    private readonly DispatcherTimer _timer;
    private uint _frameCounter;
    private bool _hasGraph;
    private MaterialPreviewMeshOption _selectedPreviewMesh = StandardPreviewMeshes[0];

    public MaterialPreviewViewModel()
    {
        // Default to a sphere with a soft gradient sky — same starting view
        // the rest of the engine uses for new materials.
        _yaw = 0.6f;
        _pitch = 0.25f;
        _radius = DefaultRadius;

        // A material editor may be opened before any world viewport. The
        // regular startup only loads EngineDLL, so bootstrap the RHI device
        // here instead of permanently caching a zero device handle.
        if (NativeEngineBridge.IsLoaded && !NativeEngineBridge.IsEngineInitialized())
        {
            if (!NativeEngineBridge.InitializeEngine(
                    NativeEngineBridge.DefaultRHIPlatformForCurrentPlatform(),
                    enableDebug: false))
                Logger.Warning("MaterialPreviewViewModel: engine RHI initialization failed");
        }

        _deviceHandle = NativeEngineBridge.GetEngineDeviceHandle();
        if (_deviceHandle != 0)
        {
            try
            {
                _previewId = MaterialPreviewNative.Create(_deviceHandle, PreviewWidth, PreviewHeight);
                if (_previewId != 0)
                {
                    MaterialPreviewNative.SetModel(_previewId, (uint)PreviewModelType.Sphere);
                    ApplyCamera();
                    ApplyLight();
                    ApplyBackground();
                    MaterialPreviewNative.SetRotationSpeed(_previewId, 0.0f);
                }
            }
            catch (Exception ex)
            {
                Logger.Warning($"MaterialPreviewViewModel: preview init failed: {ex.Message}");
            }
        }

        _pixels = new byte[PreviewWidth * PreviewHeight * 4];
        _pixelsAddr = BitmapPinStore.PinAndAttach(this, _pixels);

        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
        _timer.Tick += (_, _) => DispatchTimerTick();
    }

    /// <summary>Caller hooks this when a fresh graph is ready to display.</summary>
    public void PushGraph(string? graphJson)
    {
        if (_previewId == 0 || string.IsNullOrEmpty(graphJson)) return;
        try { _hasGraph = MaterialPreviewNative.SetGraph(_previewId, graphJson); }
        catch (Exception ex) { Logger.Warning($"PushGraph: {ex.Message}"); }
    }

    public void Start() => _timer.Start();
    public void Stop() => _timer.Stop();

    // --- Bindings ---

    private WriteableBitmap? _previewBitmapBacking;
    public WriteableBitmap? PreviewBitmap
    {
        get => _previewBitmapBacking;
        private set { _previewBitmapBacking = value; OnPropertyChanged(); }
    }

    public bool ShowPlaceholder => _previewBitmapBacking == null;
    public string PlaceholderText => _previewId == 0
        ? "(engine not loaded)"
        : "(initializing preview…)";

    /// <summary>
    /// Selected preview primitive. Switching it updates the native renderer
    /// immediately; the next timer tick presents the new mesh.
    /// </summary>
    public MaterialPreviewMeshOption SelectedPreviewMesh
    {
        get => _selectedPreviewMesh;
        set
        {
            if (value is null || ReferenceEquals(_selectedPreviewMesh, value)) return;
            _selectedPreviewMesh = value;
            OnPropertyChanged();
            ApplyPreviewMesh();
        }
    }

    private void ApplyPreviewMesh()
    {
        if (_previewId == 0) return;
        try
        {
            // Primitive selection takes precedence over a previous custom
            // geometry override and always uses the normal 3D preview path.
            MaterialPreviewNative.ClearMesh(_previewId);
            MaterialPreviewNative.SetMode2D(_previewId, false);
            MaterialPreviewNative.SetModel(_previewId, (uint)_selectedPreviewMesh.Model);
        }
        catch (Exception ex)
        {
            Logger.Warning($"Set preview mesh failed: {ex.Message}");
        }
    }

    // --- Orbit camera state (driven by View's pointer/wheel handlers) ---

    private float _yaw;
    private float _pitch;
    private float _radius;

    /// <summary>Camera yaw in radians. Updated by the View's drag handler.</summary>
    public void SetYaw(float yaw) { _yaw = yaw; ApplyCamera(); }

    /// <summary>Camera pitch in radians (clamped to ±80°).</summary>
    public void SetPitch(float pitch)
    {
        _pitch = Math.Clamp(pitch, MinPitch, MaxPitch);
        ApplyCamera();
    }

    /// <summary>Camera distance from origin. Updated by the wheel handler.</summary>
    public void SetRadius(double radiusDelta)
    {
        var r = _radius + (float)radiusDelta;
        if (r < MinRadius) r = MinRadius;
        if (r > MaxRadius) r = MaxRadius;
        _radius = r;
        ApplyCamera();
    }

    public void OnDragDelta(double dxPixels, double dyPixels)
    {
        SetYaw(_yaw - (float)(dxPixels * OrbitSensitivity));
        SetPitch(_pitch - (float)(dyPixels * OrbitSensitivity));
    }

    private void ApplyCamera()
    {
        if (_previewId == 0) return;
        var cy = MathF.Cos(_yaw);
        var sy = MathF.Sin(_yaw);
        var cp = MathF.Cos(_pitch);
        var sp = MathF.Sin(_pitch);
        var x = _radius * cp * sy;
        var y = _radius * sp;
        var z = _radius * cp * cy;
        try { MaterialPreviewNative.SetCameraPosition(_previewId, x, y, z); }
        catch (Exception ex) { Logger.Warning($"SetCameraPosition: {ex.Message}"); }
    }

    private static void ApplyLight()
    {
        // Static for now — full light customization UI is a follow-up.
        // Direction: above and slightly to the right. Color: warm sun.
    }

    private static void ApplyBackground()
    {
        // Gradient mode + slate-blue top + dark-violet bottom matches the
        // existing Phase-6 placeholder color, so the first preview frame
        // looks like a continuation of the editor theme.
    }

    // --- Timer-driven render ---

    private void DispatchTimerTick()
    {
        if (_previewId == 0 || _deviceHandle == 0) return;

        // First render needs the graph; the VM caller may not have called
        // PushGraph yet. Default to a valid output-only graph (gray sphere) so the user
        // sees *something* rather than a black screen.
        if (_frameCounter == 0 && !_hasGraph)
        {
            try { _hasGraph = MaterialPreviewNative.SetGraph(_previewId, DefaultGraphJson); }
            catch { /* ignore — second tick will retry */ }
        }
        _frameCounter++;

        ulong bytesWritten;
        try { bytesWritten = MaterialPreviewNative.RenderToRGBA(_previewId, _deviceHandle, _pixels); }
        catch (Exception ex)
        {
            Logger.Warning($"RenderToRGBA: {ex.Message}");
            return;
        }
        if (bytesWritten == 0) return;

        // Build a fresh WriteableBitmap pointing at the same pinned address.
        // The bitmap captures the address but the pin lifetime is owned by
        // BitmapPinStore (keyed on `this`), so the previous bitmap can be
        // collected without invalidating the buffer.
        var bitmap = new WriteableBitmap(
            Avalonia.Platform.PixelFormat.Rgba8888,
            AlphaFormat.Premul,
            _pixelsAddr,
            new Avalonia.PixelSize(PreviewWidth, PreviewHeight),
            new Avalonia.Vector(96, 96),
            PreviewWidth * 4);
        PreviewBitmap = bitmap;
        OnPropertyChanged(nameof(ShowPlaceholder));
    }

    public void Dispose()
    {
        try { _timer.Stop(); } catch { /* ignore */ }
        if (_previewId != 0)
        {
            try { MaterialPreviewNative.Destroy(_previewId); }
            catch { /* ignore */ }
        }
    }
}
