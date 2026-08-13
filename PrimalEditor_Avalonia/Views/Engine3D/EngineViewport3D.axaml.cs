using System;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Views.Engine3D;

public partial class EngineViewport3D : UserControl
{
    private IEditorViewportHost? _host;
    private uint _cameraId = NativeEngineBridge.InvalidCameraId;
    private uint _cameraEntityId = NativeEngineBridge.InvalidEntityId;  // entity the camera is bound to; mutated by orbit/pan/zoom
    private bool _viewportInitialized;
    private float _animTime;

    // When non-zero, the viewport renders through a scene Camera entity's
    // CameraComponent instead of the editor's Track A camera. The
    // render frame's entry point switches from
    // PipelineRenderFrameForEditorCamera (which sets IsEditorCamera=true and
    // skips SyncCamerasFromECS) to PipelineRenderFrame (which lets
    // SyncCamerasFromECS pick the scene camera's matrices). Orbit / pan
    // / zoom are still applied to the editor camera underneath so
    // switching back is seamless.
    private uint _lookThroughSceneEntityId;
    public uint LookThroughSceneEntityId => _lookThroughSceneEntityId;
    public bool IsLookingThroughSceneCamera => _lookThroughSceneEntityId != 0;

    // Per-frame icon overlay (light gizmo billboards, etc.) that draws
    // into the engine framebuffer. Update its camera basis alongside
    // GizmoOverlay so its world→screen projection matches the gizmo
    // handles the user can grab.
    private readonly LightIconRenderer _lightIconRenderer = new();

    // Orbit camera state. The camera is always looking at `_cameraTarget`
    // from a distance `_cameraDistance`, with `_cameraYaw` (around world up)
    // and `_cameraPitch` (above/below the horizon). Editing these via
    // ApplyCameraTransform is the only way the 3D viewport's view matrix
    // changes.
    private float[] _cameraTarget = new float[] { 0f, 0f, 0f };
    private float _cameraDistance = 12f;
    private float _cameraYaw = 0f;       // radians; 0 = looking down -Z (camera at +Z relative to target)
    private float _cameraPitch = 0f;     // radians; 0 = level with target
    private const float kMinPitch = -1.4f;  // ~-80°, prevent gimbal flip
    private const float kMaxPitch =  1.4f;  // ~+80°
    private const float kMinDistance = 0.5f;
    private const float kMaxDistance = 200f;
    private const float kOrbitSpeed = 0.01f;   // radians per pixel of drag delta
    private const float kPanSpeed = 0.0025f;   // world units per pixel of drag delta, scaled by distance
    private const float kZoomSpeed = 1.15f;    // multiplicative factor per wheel notch
    private const float kWheelZoomStep = 0.12f; // distance change per wheel notch (fraction of current)

    // Public camera state for managed overlays (Gizmo / picker / focus math).
    public float CameraYaw => _cameraYaw;
    public float CameraPitch => _cameraPitch;
    public float CameraDistance => _cameraDistance;
    public float CameraTargetX => _cameraTarget[0];
    public float CameraTargetY => _cameraTarget[1];
    public float CameraTargetZ => _cameraTarget[2];

    private bool _isOrthographic;
    public bool IsOrthographic
    {
        get => _isOrthographic;
        set
        {
            if (_isOrthographic != value)
            {
                _isOrthographic = value;
                InvalidateOverlay();
            }
        }
    }

    private void InvalidateOverlay()
    {
        if (this.FindControl<ViewportOverlay>("Overlay") is ViewportOverlay overlay)
            overlay.InvalidateVisual();
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo)
            gizmo.InvalidateVisual();
    }

    // Pointer interaction state. Pointer events on this control fire only
    // while the cursor is over the render surface, so the hit-test the user
    // asked for comes for free from Avalonia's input routing.
    private Avalonia.Input.MouseButton _activeButton;  // 0 = none
    private Avalonia.Point _lastPointer;
    private Border? _inputCapture;
    private Scene? _scene;

    // Selection state lives on the C# side now. The previous C++ bridge exposed
    // EditorSelectEntity / EditorGetSelectedEntity / EditorClearScene but those
    // are pure UI state with no engine-side effect, so the dictionary lives here.
    private readonly System.Collections.Generic.HashSet<ulong> _selectedEntities = new();

    public EngineViewport3D()
    {
        InitializeComponent();
        ConnectGizmo();
        AttachedToVisualTree += OnAttached;
        DetachedFromVisualTree += OnDetached;
        LayoutUpdated += OnLayoutUpdated;
        // Register on the transparent topmost overlay itself. NativeControlHost
        // owns a real NSView on macOS; relying on bubbling from that native view
        // is unreliable and was the reason mouse/trackpad input appeared dead.
        _inputCapture = this.FindControl<Border>("InputCapture");
        if (_inputCapture != null)
        {
            _inputCapture.PointerPressed += OnPointerPressed;
            _inputCapture.PointerMoved += OnPointerMoved;
            _inputCapture.PointerReleased += OnPointerReleased;
            _inputCapture.PointerWheelChanged += OnPointerWheelChanged;
        }
        KeyDown += OnKeyDown;
        Focusable = true;
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    private void OnAttached(object? sender, VisualTreeAttachmentEventArgs e)
    {
        if (_viewportInitialized) return;
        if (!NativeEngineBridge.IsLoaded)
        {
            ShowPlaceholder(true);
            return;
        }
        // Defer init until the first real layout pass — OnSizeChanged is
        // invoked after Avalonia finishes stretching the control to its
        // container's bounds, so its width/height are accurate there.
        // Calling TryInitializeFromLayout() in OnAttached typically sees
        // the design-time size (624x482 from the AXAML attribute).
        _pendingInit = true;
    }

    private bool _pendingInit;

    private bool TryInitializeFromLayout()
    {
        if (_viewportInitialized) return true;
        if (!NativeEngineBridge.IsLoaded) return false;

        int width = (int)Bounds.Width;
        int height = (int)Bounds.Height;
        if (width <= 0 || height <= 0) return false;

        try
        {
            if (_host == null)
            {
                var mtkHost = this.FindControl<PrimalEditor_Avalonia.Views.Engine3D.MTKViewHost>("MTKHostView");
                if (mtkHost == null)
                {
                    Logger.Warning("MTKViewHost not in visual tree; will retry on next layout pass");
                    return false;
                }
                _host = EditorViewportHostFactory.CreateForCurrentPlatform(mtkHost);
                Logger.Info($"Editor viewport host: {_host.Name}");
            }

            if (!_host.TryInitialize(width, height))
            {
                // Host reported the underlying view is not yet ready
                // (host view handle zero, or surface creation refused).
                // The host has already cleaned up; caller may retry on
                // the next layout pass.
                return false;
            }

            // The RHI is now bound; size the StandardRenderPipeline to
            // match. StandardRenderPipeline owns its own render targets
            // keyed off the viewport size.
            NativeEngineBridge.SetPipelineViewportSize(width, height);

            UInt64 deviceHandle = NativeEngineBridge.GetEngineDeviceHandle();
            if (deviceHandle != 0 && NativeEngineBridge.CreateStandardRenderPipeline(deviceHandle))
            {
                // CreateStandardRenderPipeline initializes the forward
                // renderer.  Set editor mode only after that succeeds;
                // calling it before creation is a silent no-op in the C ABI
                // and leaves PipelineRenderFrame on the deferred/GPU-driven
                // path, which cannot see the four forward-renderer spheres.
                NativeEngineBridge.PipelineSetEditorMode(true);
                Logger.Info("StandardRenderPipeline created (Lumen=Low)");
            }
            else
            {
                Logger.Warning("StandardRenderPipeline creation failed — falling back to legacy gpass path");
            }

            // Set up a default editor camera so the forward pipeline has
            // a view to render from. Scene content (entities, lights,
            // render items) is pushed separately by SceneRuntimeSynchronizer
            // whenever the user opens a scene or adds / edits an entity.
            _cameraId = CreateEditorCamera(width, height);
            if (NativeEngineBridge.IsValidCamera(_cameraId))
            {
                Logger.Info($"Editor camera {_cameraId} created");
            }

            // Push the initial orbit state to the engine so the camera
            // starts at the expected default view.
            ApplyCameraTransform();

            _renderTarget = NativeEngineBridge.CreateRenderTarget(
                width, height, NativeEngineBridge.RenderTargetFormat.RGBA16F);
            if (_renderTarget == 0)
            {
                Logger.Warning("CreateRenderTarget failed — viewport will show clear color only");
            }
            else
            {
                _renderTargetWidth = width;
                _renderTargetHeight = height;
            }

            _viewportInitialized = true;
            ShowPlaceholder(false);

            // Re-sync the active scene NOW that the render pipeline
            // exists. The initial SceneSynchronizer.Sync ran inside
            // WorldEditorViewModel.ActiveScene setter when the project
            // opened — at that point CreateStandardRenderPipeline had
            // NOT been called yet, so the C++ entities were created
            // without a pipeline to manage them. Calling Sync again
            // here tears down those orphan entities and re-creates
            // them with the pipeline active, which avoids the
            // "mutex lock failed: Invalid argument" crash inside
            // PipelineRenderFrame on the first frame.
            ResyncActiveScene();

            StartRenderLoop();

            Logger.Info($"3D viewport initialized via {_host.Name} forward pipeline");
            return true;
        }
        catch (NotImplementedException ex)
        {
            Logger.Warning($"3D viewport host not implemented on this platform: {ex.Message}");
            ShowPlaceholder(true);
            _host = null;
            return false;
        }
        catch (Exception ex)
        {
            Logger.Error("Failed to initialize 3D viewport", ex);
            ShowPlaceholder(true);
            return false;
        }
    }

    // Create a default editor camera for THIS viewport. Returns the
    // camera id (0 on failure). Uses ResourceAPI::CreateCamera which
    // creates a Track A graphics::camera wrapping a fresh entity — the
    // entity's transform drives MetalCamera::update() and feeds
    // view/projection into the render view. The render pipeline tags
    // views built from this id as IsEditorCamera=true so ECS Camera
    // sync does not overwrite its matrices when a scene Camera entity
    // exists.
    private uint CreateEditorCamera(int width, int height)
    {
        // ResourceAPI::CreateCamera stores fov in Camera::field_of_view, but
        // MetalCamera::update() multiplies it by π before passing to
        // createPerspectiveFovLH — i.e. the engine treats the value as a
        // fraction of π, not radians. 60° → fov = 60° / 180° = 1/3.
        _cameraEntityId = NativeEngineBridge.CreateEntity(0f, 1f, -12f);
        if (_cameraEntityId == NativeEngineBridge.InvalidEntityId)
        {
            Logger.Warning($"Editor camera: CreateEntity returned 0x{_cameraEntityId:X} — orbit/zoom disabled");
            return 0;
        }
        const float fov = 1f / 3f;
        float aspect = (float)width / (float)height;
        const float nearZ = 0.1f;
        const float farZ = 1000.0f;
        var cameraId = NativeEngineBridge.CreateCamera(_cameraEntityId, fov, aspect, nearZ, farZ);
        if (!NativeEngineBridge.IsValidCamera(cameraId))
        {
            Logger.Warning($"Editor camera: CreateCamera returned 0x{cameraId:X} — orbit/zoom disabled");
            return 0;
        }
        return cameraId;
    }
    private UInt64 _renderTarget;
    private int _renderTargetWidth;
    private int _renderTargetHeight;

    private DispatcherTimer? _renderTimer;
    private DispatcherTimer? _uiRefreshTimer;
    private DateTime _lastTick;
    private int _renderInProgress;
    private uint _nativeRenderLoopId;

    // Tracks every live native render loop id so App.OnExit can Destroy
    // each one with the correct id. EditorRenderLoop.Stop(0)/Destroy(0)
    // are no-ops by design, so App can't simply call them — it needs the
    // concrete id(s) the viewports allocated. A static set is sufficient
    // because there is exactly one EngineViewport3D instance in the editor.
    private static readonly System.Collections.Generic.HashSet<uint> s_liveLoopIds = new();

    private void StartRenderLoop()
    {
        if (_renderTimer != null) return;
        _lastTick = DateTime.UtcNow;

        // Only use the Avalonia DispatcherTimer. The native 60Hz render
        // loop (EditorRenderLoop.Start + GCD timer + InvokeAsync) races
        // with the timer on the C++ mutex inside PipelineRenderFrame,
        // causing "mutex lock failed: Invalid argument" crashes. The
        // DispatcherTimer runs entirely on the UI thread — no
        // cross-thread marshalling, no mutex contention, no crash.
        _renderTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromMilliseconds(33)
        };
        _renderTimer.Tick += OnRenderTick;
        _renderTimer.Start();
        Logger.Info("Render loop started (DispatcherTimer 30Hz, UI thread only)");
    }

    /// <summary>
    /// A standalone Avalonia timer that forces a render frame + overlay
    /// repaint at 30Hz. Only runs when the native render loop is NOT
    /// active — the native loop already calls OnRenderFrameCore at 60Hz
    /// via OnNativeFrame, so running both simultaneously causes a
    /// mutex lock failure inside the C++ renderer (two threads enter
    /// PipelineRenderFrame at the same time).
    /// </summary>
    private void StartUIRefreshTimer()
    {
        // If the native loop is running, don't start the fallback —
        // it would race the native thread and crash with
        // "mutex lock failed: Invalid argument".
        if (_nativeRenderLoopId != 0) return;

        _uiRefreshTimer = new DispatcherTimer(DispatcherPriority.Normal)
        {
            Interval = TimeSpan.FromMilliseconds(33)
        };
        _uiRefreshTimer.Tick += (_, _) =>
        {
            if (!_viewportInitialized) return;
            OnRenderFrameCore(0.033f);
            InvalidateOverlays();
        };
        _uiRefreshTimer.Start();
        Logger.Info("UI refresh timer started (30Hz fallback, native loop not running)");
    }

    private void StopUIRefreshTimer()
    {
        _uiRefreshTimer?.Stop();
        _uiRefreshTimer = null;
    }

    private void InvalidateOverlays()
    {
        if (this.FindControl<ViewportOverlay>("Overlay") is ViewportOverlay ov) ov.InvalidateVisual();
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gz) gz.InvalidateVisual();
    }

    private void StopRenderLoop()
    {
        StopUIRefreshTimer();
        if (_nativeRenderLoopId != 0)
        {
            // Stop invalidates the timer but the C++ slot in g_render_loops
            // is freed only by Destroy. Without Destroy, repeated init/teardown
            // of the viewport leaks one slot per cycle and grows g_render_loops
            // without bound. Destroy internally calls Stop first so calling
            // Stop here would be redundant.
            NativeEngineBridge.EditorRenderLoop.Destroy(_nativeRenderLoopId);
            s_liveLoopIds.Remove(_nativeRenderLoopId);
            _nativeRenderLoopId = 0;
        }
        _renderTimer?.Stop();
        _renderTimer = null;
    }

    /// <summary>
    /// Returns the set of native render-loop ids currently owned by any live
    /// EngineViewport3D instance. App.OnExit iterates this set and Destroy()s
    /// each id (Destroy internally calls Stop, so the loop is fully cleaned
    /// up before the engine shuts down).
    /// </summary>
    public static System.Collections.Generic.IReadOnlyCollection<uint> LiveNativeLoopIds => s_liveLoopIds;

    private void OnNativeFrame(float dt)
    {
        // The native render loop fires from a GCD worker thread. Avalonia's
        // visual tree controls are not thread-safe and PipelineRenderFrame +
        // BlitRenderTargetToSurface touch CAMetalDrawable-backed state. Marshal
        // to Avalonia's UI thread via InvokeAsync. PostAsync would silently
        // drop or coalesce frames on the dispatcher; direct InvokeAsync keeps
        // each invocation serialized like fire_timer already is.
        //
        // Catch exceptions on the UI thread side too — otherwise an exception
        // inside InvokeAsync's continuation gets posted back to the dispatcher
        // and triggers Avalonia's async-exception path, which on some builds
        // hard-kills the editor window with an unhandled exception.
        if (Avalonia.Threading.Dispatcher.UIThread.CheckAccess())
        {
            try { OnRenderFrameCore(dt); }
            catch (Exception ex) { Logger.Error("OnRenderFrameCore (UI) failed", ex); }
        }
        else
        {
            Avalonia.Threading.Dispatcher.UIThread.InvokeAsync(() =>
            {
                try { OnRenderFrameCore(dt); }
                catch (Exception ex) { Logger.Error("OnRenderFrameCore (UI) failed", ex); }
            });
        }
    }

    private void OnRenderTick(object? sender, EventArgs e)
    {
        var now = DateTime.UtcNow;
        var dt = (float)(now - _lastTick).TotalSeconds;
        if (dt <= 0f) dt = 0.016f;
        if (dt > 0.1f) dt = 0.1f;
        _lastTick = now;
        OnRenderFrameCore(dt);
    }

    private void OnRenderFrameCore(float dt)
    {
        if (!NativeEngineBridge.IsLoaded || !_viewportInitialized) return;
        if (_renderTarget == 0) return;
        if (_host == null || !_host.IsReady) return;
        if (!NativeEngineBridge.IsValidCamera(_cameraId)) return;

        // PipelineRenderFrame is NOT thread-safe — the C++ renderer
        // uses a mutex internally that throws "mutex lock failed:
        // Invalid argument" when two threads enter simultaneously.
        // Both the native 60Hz loop (via InvokeAsync) and the
        // DispatcherTimer fallback land on the UI thread, but
        // InvokeAsync re-entrancy can still cause overlap. The lock
        // is a no-op on the fast path (no contention) and a
        // correctness guard on the slow path.
        if (System.Threading.Interlocked.CompareExchange(ref _renderInProgress, 1, 0) != 0) return;
        try
        {
            _animTime += dt;

            // TEMPORARILY DISABLED: LightIconRenderer.DrawSceneIcons
            // calls ClearDebugLines + DrawDebugWorldQuad + DrawDebugLightBulb
            // which interact with the C++ debug-draw queue. If
            // PipelineRenderFrame drains the same queue from a job-system
            // worker thread, the mutex inside debug_draw can fail with
            // "Invalid argument". Comment out until the threading model
            // is verified safe.
            // var activeScene = GetActiveSceneForIconRenderer();
            // if (activeScene != null) _lightIconRenderer.DrawSceneIcons(activeScene);

            var surfaceId = (uint)_host.SurfaceId;
            // Render through the editor's Track A camera by default
            // (IsEditorCamera=true, SyncCamerasFromECS bypassed). When the
            // user picks a scene Camera entity via LookThroughCamera, route
            // through the legacy entry so SyncCamerasFromECS picks that
            // entity's CameraComponent + Transform for the view matrices.
            if (IsLookingThroughSceneCamera)
            {
                NativeEngineBridge.PipelineRenderFrameForSceneCamera(
                    _renderTarget, _cameraId, surfaceId, _lookThroughSceneEntityId);
            }
            else
            {
                NativeEngineBridge.PipelineRenderFrameForEditorCamera(_renderTarget, _cameraId, surfaceId);
            }
            NativeEngineBridge.BlitRenderTargetToSurface(_renderTarget, surfaceId);

            SyncOverlay();
        }
        finally
        {
            System.Threading.Interlocked.Exchange(ref _renderInProgress, 0);
        }
    }

    private void ResyncActiveScene()
    {
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        if (lifetime?.MainWindow?.DataContext is not ViewModels.MainWindowViewModel mainVm) return;
        var weVm = mainVm.WorldEditor;
        if (weVm?.ActiveScene == null) return;
        weVm.SceneSynchronizer.Sync(weVm.ActiveScene);
        Logger.Info("Active scene re-synced after render pipeline creation");
    }

    /// <summary>
    /// Walk the MainWindow data-context chain to find the active scene.
    /// Returns null when the editor's MainWindow isn't constructed yet
    /// (first frames after engine init) — LightIconRenderer handles null
    /// gracefully.
    /// </summary>
    private GameProject.Scene? GetActiveSceneForIconRenderer()
    {
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        var main = lifetime?.MainWindow?.DataContext as ViewModels.MainWindowViewModel;
        return main?.WorldEditor?.ActiveScene;
    }

    private void OnDetached(object? sender, VisualTreeAttachmentEventArgs e)
    {
        StopRenderLoop();
        LayoutUpdated -= OnLayoutUpdated;
        if (_renderTarget != 0)
        {
            NativeEngineBridge.DestroyRenderTarget(_renderTarget);
            _renderTarget = 0;
        }
        _host?.Destroy();
        _viewportInitialized = false;
    }

    // LayoutUpdated fires after the visual tree's layout pass completes. The
    // first OnSizeChanged may see the design-time size because the Border
    // parent hasn't been laid out yet (its Bounds = 0x0). LayoutUpdated gives
    // us a reliable signal that real layout has happened, even when the
    // control's own size hasn't changed.
    private void OnLayoutUpdated(object? sender, EventArgs e)
    {
        if (!NativeEngineBridge.IsLoaded) return;

        // First-time init: wait for layout to actually settle (parent has
        // non-zero bounds) before allocating the MTKView. OnSizeChanged
        // can fire with a valid child size but a 0x0 parent, which puts
        // the MTKView at the wrong location.
        if (_pendingInit && !_viewportInitialized)
        {
            var parentReady = this.Parent is Control p && p.Bounds.Width > 0 && p.Bounds.Height > 0;
            if (parentReady && Bounds.Width > 0 && Bounds.Height > 0)
            {
                _pendingInit = false;
                if (!TryInitializeFromLayout())
                {
                    ScheduleRetry();
                }
            }
            return;
        }

        if (!_viewportInitialized) return;
        if (_host == null || !_host.IsReady) return;
        var width = (int)Bounds.Width;
        var height = (int)Bounds.Height;
        if (width <= 0 || height <= 0) return;

        var sizeChanged = _lastWidth != width || _lastHeight != height;
        if (sizeChanged)
        {
            _lastWidth = width;
            _lastHeight = height;
            _host.Resize(width, height);
            // StandardRenderPipeline maintains its own render targets
            // keyed off the viewport size; update it in lockstep with
            // the surface resize so internal buffers don't go stale.
            NativeEngineBridge.SetPipelineViewportSize(width, height);

            // The offscreen _renderTarget (RGBA16F, where the forward
            // pipeline renders before blitting into the MTKView drawable)
            // is sized once at init and never rebuilt. On viewport resize
            // the target keeps the old dimensions and the pipeline either
            // stretches or samples outside it. Tear it down and recreate at
            // the new size.
            if (_renderTarget != 0)
            {
                NativeEngineBridge.DestroyRenderTarget(_renderTarget);
                _renderTarget = NativeEngineBridge.CreateRenderTarget(
                    width, height, NativeEngineBridge.RenderTargetFormat.RGBA16F);
                if (_renderTarget == 0)
                {
                    Logger.Warning("CreateRenderTarget (resize) failed — viewport may show stale content");
                }
                else
                {
                    _renderTargetWidth = width;
                    _renderTargetHeight = height;
                }
            }

            // Camera aspect ratio must follow the surface aspect so the
            // projected view doesn't stretch on resize. The constructor
            // captured the initial aspect; update it here.
            if (_cameraEntityId != NativeEngineBridge.InvalidEntityId && width > 0 && height > 0)
            {
                NativeEngineBridge.SetEntityCameraAspectRatio(_cameraEntityId, (float)width / height);
            }
        }

        // Sync camera state to the managed overlay (grid/axes/gizmo) on
        // every layout pass. Cheap — the overlay only invalidates if
        // anything actually changed.
        SyncOverlay();
    }

    private void SyncOverlay()
    {
        if (this.FindControl<ViewportOverlay>("Overlay") is ViewportOverlay overlay)
        {
            overlay.CameraYaw = _cameraYaw;
            overlay.CameraPitch = _cameraPitch;
            overlay.CameraDistance = _cameraDistance;
            overlay.CameraTarget = (_cameraTarget[0], _cameraTarget[1], _cameraTarget[2]);
        }
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo)
        {
            gizmo.UpdateCamera(_cameraYaw, _cameraPitch, _cameraDistance, (float)Bounds.Width, (float)Bounds.Height);
        }
        _lightIconRenderer.UpdateCamera(_cameraYaw, _cameraPitch, _cameraDistance);
    }

    /// <summary>Move the gizmo anchor to <paramref name="worldX/Y/Z"/>.</summary>
    public void SetGizmoTarget(float worldX, float worldY, float worldZ)
    {
        if (this.FindControl<ViewportOverlay>("Overlay") is ViewportOverlay overlay)
        {
            overlay.GizmoTarget = (worldX, worldY, worldZ);
        }
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo)
        {
            gizmo.SetTarget(worldX, worldY, worldZ);
        }
    }

    /// <summary>Subscribe to gizmo drag. The handler receives (axis, delta) where
    /// delta is a screen-space unit. The handler is responsible for
    /// translating delta to world-space and updating the selected entity.</summary>
    public event System.EventHandler<(GizmoOverlay.GizmoMode mode, GizmoOverlay.GizmoAxis axis, float delta)>? GizmoDrag;

    private void ConnectGizmo()
    {
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo)
        {
            gizmo.OnDelta += (s, e) => GizmoDrag?.Invoke(this, e);
            // Forward pointer events to the gizmo overlay so its
            // hit-test can run. The gizmo itself is IsHitTestVisible=False
            // (declared in XAML), so its callbacks fire when the user
            // pressed down on the viewport — the parent Grid routes
            // events to the topmost child, which is the gizmo overlay.
        }
    }

    private int _lastWidth;
    private int _lastHeight;

    // LayoutUpdated drives init and positioning because OnSizeChanged can
    // fire before the parent Border has a real layout (Bounds = 0x0), which
    // would place the MTKView at the wrong screen coordinates.

    private int _retryCount;
    private void ScheduleRetry()
    {
        if (_retryCount > 200) return;
        _retryCount++;
        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(50) };
        timer.Tick += (s, e) =>
        {
            timer.Stop();
            if (_viewportInitialized) return;
            if (TryInitializeFromLayout()) return;
            ScheduleRetry();
        };
        timer.Start();
    }

    private void ShowPlaceholder(bool show)
    {
        var placeholder = this.FindControl<TextBlock>("Placeholder");
        if (placeholder != null) placeholder.IsVisible = show;
    }

    private void OnPointerPressed(object? sender, Avalonia.Input.PointerPressedEventArgs e)
    {
        if (!_viewportInitialized) return;
        var point = e.GetCurrentPoint(this);
        if (!point.Properties.IsLeftButtonPressed && !point.Properties.IsRightButtonPressed) return;
        _activeButton = point.Properties.IsLeftButtonPressed
            ? Avalonia.Input.MouseButton.Left
            : Avalonia.Input.MouseButton.Right;

        // Left click on the gizmo takes precedence over entity picking.
        if (_activeButton == Avalonia.Input.MouseButton.Left
            && this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo)
        {
            if (gizmo.TryStartDrag(point.Position))
            {
                // Capture the pointer so that PointerMoved / PointerReleased
                // keep firing even when the cursor leaves the viewport bounds
                // (e.g. user drags past the edge of the render surface).
                // Without capture, Avalonia stops routing PointerMoved to this
                // control when the cursor exits Bounds, leaving the gizmo in
                // a permanent _dragging=true state.
                e.Pointer.Capture(_inputCapture is not null ? _inputCapture : this);
                Focus();
                e.Handled = true;
                return;
            }
        }

        // Left click: ray-pick an entity. If a hit is found, raise
        // EntityPicked for the click event but still allow orbit-on-drag
        // — the previous behaviour cleared _activeButton immediately,
        // forcing the user to start drag from empty space. We now pick
        // on press and keep _activeButton = Left, so dragging after the
        // pick still orbits. The pick event fires once at press time and
        // the drag (orbit) takes over from the same press.
        if (_activeButton == Avalonia.Input.MouseButton.Left && _entityCandidates != null)
        {
            var (ox, oy, oz, dx, dy, dz) = ScreenToRay(
                (float)point.Position.X, (float)point.Position.Y,
                (float)Bounds.Width, (float)Bounds.Height);
            var hit = ViewportPicker.PickNearest(_entityCandidates, ox, oy, oz, dx, dy, dz);
            if (hit.HasValue)
            {
                EntityPicked?.Invoke(this, hit.Value.EntityId);
                // Keep _activeButton = Left so the subsequent drag orbits
                // the camera instead of doing nothing. Selection already
                // fired on press; the drag is purely a view operation.
            }
        }

        _lastPointer = point.Position;
        e.Pointer.Capture(_inputCapture is not null ? _inputCapture : this);
        Focus();
        e.Handled = true;
    }

    /// <summary>
    /// Set the world-space entity list that the picker tests against.
    /// Pass <c>null</c> to disable picking. Each entry is
    /// <c>(entityId, worldX, worldY, worldZ, boundingRadius)</c>.
    /// </summary>
    public void SetPickerCandidates(System.Collections.Generic.IReadOnlyList<(uint id, ViewportPicker.AABB bounds)>? candidates)
    {
        _entityCandidates = candidates;
    }

    private System.Collections.Generic.IReadOnlyList<(uint id, ViewportPicker.AABB bounds)>? _entityCandidates;

    /// <summary>Fires when the user left-clicks an entity in the viewport.</summary>
    public event EventHandler<uint>? EntityPicked;

    private void OnPointerMoved(object? sender, Avalonia.Input.PointerEventArgs e)
    {
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo && gizmo.IsDragging)
        {
            gizmo.DragDelta(e.GetCurrentPoint(this).Position);
            e.Handled = true;
            return;
        }
        if (_activeButton == Avalonia.Input.MouseButton.None) return;
        var p = e.GetCurrentPoint(this).Position;
        float dx = (float)(p.X - _lastPointer.X);
        float dy = (float)(p.Y - _lastPointer.Y);
        _lastPointer = p;
        if (dx == 0f && dy == 0f) return;

        if (_activeButton == Avalonia.Input.MouseButton.Left)
        {
            // Orbit: horizontal drag = yaw, vertical drag = pitch.
            _cameraYaw -= dx * kOrbitSpeed;
            _cameraPitch -= dy * kOrbitSpeed;
            if (_cameraPitch < kMinPitch) _cameraPitch = kMinPitch;
            if (_cameraPitch > kMaxPitch) _cameraPitch = kMaxPitch;
        }
        else if (_activeButton == Avalonia.Input.MouseButton.Right)
        {
            // Pan: translate target and camera in the camera's local screen
            // plane. Right vector is camera-right, up vector is world up
            // (good enough for an editor camera; proper screen-up via the
            // actual view basis is left as a TODO if precision matters).
            float cy = MathF.Cos(_cameraYaw);
            float sy = MathF.Sin(_cameraYaw);
            float cp = MathF.Cos(_cameraPitch);
            float sp = MathF.Sin(_cameraPitch);
            // forward = (sy*cp, sp, cy*cp) (camera looks at -forward, away
            // from the target). right = normalize(cross(forward, up)).
            float[] forward = new[] { sy * cp, sp, cy * cp };
            float[] worldUp = new[] { 0f, 1f, 0f };
            float[] right = Normalize(Cross(forward, worldUp));
            // screen-up = cross(right, forward)
            float[] screenUp = Normalize(Cross(right, forward));
            float panScale = _cameraDistance * kPanSpeed;
            _cameraTarget[0] -= right[0] * dx * panScale;
            _cameraTarget[1] -= right[1] * dx * panScale;
            _cameraTarget[2] -= right[2] * dx * panScale;
            _cameraTarget[0] += screenUp[0] * dy * panScale;
            _cameraTarget[1] += screenUp[1] * dy * panScale;
            _cameraTarget[2] += screenUp[2] * dy * panScale;
        }
        ApplyCameraTransform();
        e.Handled = true;
    }

    private void OnPointerReleased(object? sender, Avalonia.Input.PointerReleasedEventArgs e)
    {
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo && gizmo.IsDragging)
        {
            gizmo.EndDrag();
            // Release the pointer capture acquired in OnPointerPressed so
            // normal event routing resumes after the drag ends.
            e.Pointer.Capture(null);
            e.Handled = true;
            return;
        }
        if (e.InitialPressMouseButton == _activeButton)
        {
            _activeButton = Avalonia.Input.MouseButton.None;
            e.Pointer.Capture(null);
            Logger.Info($"Editor camera updated: yaw={_cameraYaw:0.###}, pitch={_cameraPitch:0.###}, distance={_cameraDistance:0.###}");
            e.Handled = true;
        }
    }

    private void OnPointerWheelChanged(object? sender, Avalonia.Input.PointerWheelEventArgs e)
    {
        if (!_viewportInitialized) return;
        // Wheel up (DeltaY < 0) = zoom in (decrease distance). We translate
        // the wheel notch count into a multiplicative distance change so the
        // zoom feels uniform regardless of the user's wheel sensitivity.
        float steps = (float)e.Delta.Y;
        float factor = MathF.Pow(1f - kWheelZoomStep, steps);
        _cameraDistance *= factor;
        if (_cameraDistance < kMinDistance) _cameraDistance = kMinDistance;
        if (_cameraDistance > kMaxDistance) _cameraDistance = kMaxDistance;
        ApplyCameraTransform();
        e.Handled = true;
    }

    // Compute the camera entity's world position and look-direction from the
    // orbit state, then push both into the engine via SetEntityTransform.
    // Position uses spherical coords around _cameraTarget; rotation is
    // passed as Euler angles that the engine converts into a quaternion.
    private void ApplyCameraTransform()
    {
        if (_cameraEntityId == NativeEngineBridge.InvalidEntityId) return;
        float cy = MathF.Cos(_cameraYaw);
        float sy = MathF.Sin(_cameraYaw);
        float cp = MathF.Cos(_cameraPitch);
        float sp = MathF.Sin(_cameraPitch);
        // Position: orbit sphere around _cameraTarget. Yaw=0 puts the
        // camera at -Z relative to the target (matching the original
        // "camera back along -Z" default).
        float ox = -sy * cp;
        float oy = -sp;
        float oz = -cy * cp;
        float[] pos = new[]
        {
            _cameraTarget[0] + ox * _cameraDistance,
            _cameraTarget[1] + oy * _cameraDistance,
            _cameraTarget[2] + oz * _cameraDistance,
        };
        // The engine's euler_to_quat on macOS (clang path) takes
        // [roll, pitch, yaw] in that order, with the quat built as
        // quat_z * quat_y * quat_x (apply X first, then Y, then Z in the
        // local frame). To make the entity's default +Z forward direction
        // point back at the target, we set pitch = +_cameraPitch (the
        // camera should pitch UP to look down at a target below, and
        // pitch DOWN to look up — the Y rotation around the camera's
        // right axis maps to that), and yaw = +_cameraYaw.
        float[] rot = new[] { 0f, _cameraPitch, _cameraYaw };
        float[] scl = new[] { 1f, 1f, 1f };
        NativeEngineBridge.SetEntityTransform(_cameraEntityId, pos, rot, scl);
        SaveEditorCameraState();
    }

    private static float[] Cross(float[] a, float[] b) => new[]
    {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    };

    private static float[] Normalize(float[] v)
    {
        float len = MathF.Sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len < 1e-6f) return new[] { 0f, 1f, 0f };
        return new[] { v[0] / len, v[1] / len, v[2] / len };
    }

    private void OnKeyDown(object? sender, Avalonia.Input.KeyEventArgs e)
    {
        // F = reset orbit state to default view. Only fires when this
        // control is focused (e.g. user clicked into the viewport and
        // pressed F with no entity selected). The outer WorldEditorView
        // consumes F when an entity is selected, so this branch is the
        // "no selection, reset view" path.
        if (e.Key == Avalonia.Input.Key.F)
        {
            _cameraTarget = new float[] { 0f, 0f, 0f };
            _cameraDistance = 12f;
            _cameraYaw = 0f;
            _cameraPitch = 0f;
            ApplyCameraTransform();
            Logger.Info("Camera reset to default view (F)");
            e.Handled = true;
        }
    }

    /// <summary>
    /// Move the orbit camera to frame the entity at <paramref name="entityId"/>.
    /// Reads the entity's world position via <see cref="NativeEngineBridge.GetEntityPosition"/>
    /// and re-aims the orbit camera at that point with a default framing distance.
    /// </summary>
    public void FocusOnEntity(uint entityId)
    {
        if (!NativeEngineBridge.IsLoaded) return;
        if (entityId == 0 || entityId == NativeEngineBridge.InvalidEntityId) return;
        var pos = NativeEngineBridge.GetEntityPosition(entityId);
        _cameraTarget = new[] { pos.x, pos.y, pos.z };
        _cameraDistance = 8f;
        ApplyCameraTransform();
        SetGizmoTarget(pos.x, pos.y, pos.z);
        Logger.Info($"Focus on entity {entityId} at ({pos.x:0.##},{pos.y:0.##},{pos.z:0.##})");
    }

    /// <summary>
    /// Switch the viewport into "look through this scene Camera" mode. The
    /// next render frame uses the legacy <c>PipelineRenderFrame</c> entry
    /// (without the editor-camera flag) so <c>SyncCamerasFromECS</c> runs
    /// and reads this entity's CameraComponent + Transform. Pass
    /// <paramref name="entityId"/> = 0 to return to editor-camera mode.
    /// The editor's orbit / pan / zoom continue to update the underlying
    /// Track A camera so switching back restores the user's last view.
    /// </summary>
    public void LookThroughCamera(uint entityId)
    {
        if (entityId == 0 || entityId == NativeEngineBridge.InvalidEntityId)
        {
            _lookThroughSceneEntityId = 0;
            Logger.Info("Viewport camera mode: editor (default)");
        }
        else
        {
            _lookThroughSceneEntityId = entityId;
            if (Bounds.Width > 0 && Bounds.Height > 0)
                NativeEngineBridge.SetEntityCameraAspectRatio(entityId, (float)Bounds.Width / (float)Bounds.Height);
            Logger.Info($"Viewport camera mode: scene Camera entity {entityId}");
        }
    }

    /// <summary>Attach the scene whose editor camera this viewport edits.</summary>
    public void SetScene(Scene? scene)
    {
        if (ReferenceEquals(_scene, scene)) return;
        SaveEditorCameraState();
        _scene = scene;
        _lookThroughSceneEntityId = 0;
        if (_scene != null)
        {
            var camera = _scene.EditorCamera;
            _cameraTarget = new[] { camera.TargetX, camera.TargetY, camera.TargetZ };
            _cameraDistance = Math.Clamp(camera.Distance, kMinDistance, kMaxDistance);
            _cameraYaw = camera.Yaw;
            _cameraPitch = Math.Clamp(camera.Pitch, kMinPitch, kMaxPitch);
        }
        ApplyCameraTransform();
        SyncOverlay();
    }

    private void SaveEditorCameraState()
    {
        if (_scene == null) return;
        var camera = _scene.EditorCamera;
        var changed = camera.TargetX != _cameraTarget[0] ||
                      camera.TargetY != _cameraTarget[1] ||
                      camera.TargetZ != _cameraTarget[2] ||
                      camera.Distance != _cameraDistance ||
                      camera.Yaw != _cameraYaw ||
                      camera.Pitch != _cameraPitch;
        if (!changed) return;
        camera.TargetX = _cameraTarget[0];
        camera.TargetY = _cameraTarget[1];
        camera.TargetZ = _cameraTarget[2];
        camera.Distance = _cameraDistance;
        camera.Yaw = _cameraYaw;
        camera.Pitch = _cameraPitch;
        _scene.MarkDirty();
    }

    /// <summary>
    /// Cast a ray from a screen-space point into the world. Used by
    /// managed pickers and Gizmo unproject.
    /// </summary>
    public (float ox, float oy, float oz, float dx, float dy, float dz) ScreenToRay(float screenX, float screenY, float viewportWidth, float viewportHeight)
    {
        float cy = MathF.Cos(_cameraYaw);
        float sy = MathF.Sin(_cameraYaw);
        float cp = MathF.Cos(_cameraPitch);
        float sp = MathF.Sin(_cameraPitch);
        // forward = (sy*cp, sp, cy*cp) (camera looks at -forward).
        float[] forward = new[] { sy * cp, sp, cy * cp };
        float[] worldUp = new[] { 0f, 1f, 0f };
        float[] right = new[] { forward[1] * worldUp[2] - forward[2] * worldUp[1], forward[2] * worldUp[0] - forward[0] * worldUp[2], forward[0] * worldUp[1] - forward[1] * worldUp[0] };
        float rlen = MathF.Sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        if (rlen > 1e-6f) { right[0] /= rlen; right[1] /= rlen; right[2] /= rlen; }
        float[] screenUp = new[] { right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2], right[0] * forward[1] - right[1] * forward[0] };

        // Camera position: in perspective mode the camera orbits at
        // target - forward*distance; in orthographic mode the camera
        // sits at target and the ray direction is purely forward (no
        // divergence). The origin matters for ray-AABB math: in ortho
        // we anchor the origin at the target plane so entry/exit tests
        // work against world-space bounds.
        float camX = _cameraTarget[0] - (_isOrthographic ? 0f : forward[0] * _cameraDistance);
        float camY = _cameraTarget[1] - (_isOrthographic ? 0f : forward[1] * _cameraDistance);
        float camZ = _cameraTarget[2] - (_isOrthographic ? 0f : forward[2] * _cameraDistance);

        // In ortho mode each world unit maps to roughly viewportHeight /
        // (2 * distance) pixels. Use that ratio so the picker hits the
        // same world-space position regardless of zoom.
        float ux, vy, depth;
        if (_isOrthographic)
        {
            float scale = viewportHeight / MathF.Max(_cameraDistance, 0.1f) * 0.5f;
            ux = (screenX - viewportWidth * 0.5f) / scale;
            vy = -(screenY - viewportHeight * 0.5f) / scale;
            depth = 1f; // direction only, magnitude normalized by caller
        }
        else
        {
            float focal = MathF.Min(viewportWidth, viewportHeight) * 0.5f;
            ux = (screenX - viewportWidth * 0.5f) / focal;
            vy = -(screenY - viewportHeight * 0.5f) / focal; // invert Y
            depth = 1f;
        }

        float dx = forward[0] * depth + right[0] * ux + screenUp[0] * vy;
        float dy = forward[1] * depth + right[1] * ux + screenUp[1] * vy;
        float dz = forward[2] * depth + right[2] * ux + screenUp[2] * vy;
        return (camX, camY, camZ, dx, dy, dz);
    }
}
