using System;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Views;

public partial class EngineViewport3D : UserControl
{
    // RHI forward pipeline state — drives EngineAPI.cpp directly.
    private uint _surfaceId = uint.MaxValue;
    private uint _cameraId;
    private uint _cameraEntityId;  // entity the camera is bound to; mutated by orbit/pan/zoom
    private bool _engineInitialized;
    private bool _viewportInitialized;
    private float _animTime;

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

    // Pointer interaction state. Pointer events on this control fire only
    // while the cursor is over the render surface, so the hit-test the user
    // asked for comes for free from Avalonia's input routing.
    private Avalonia.Input.MouseButton _activeButton;  // 0 = none
    private Avalonia.Point _lastPointer;

    // Selection state lives on the C# side now. The previous C++ bridge exposed
    // EditorSelectEntity / EditorGetSelectedEntity / EditorClearScene but those
    // are pure UI state with no engine-side effect, so the dictionary lives here.
    private readonly System.Collections.Generic.HashSet<ulong> _selectedEntities = new();

    public EngineViewport3D()
    {
        InitializeComponent();
        AttachedToVisualTree += OnAttached;
        DetachedFromVisualTree += OnDetached;
        LayoutUpdated += OnLayoutUpdated;
        PointerPressed += OnPointerPressed;
        PointerMoved += OnPointerMoved;
        PointerReleased += OnPointerReleased;
        PointerWheelChanged += OnPointerWheelChanged;
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
            // Step 1: 3-step RHI engine init. Idempotent — only runs once.
            if (!_engineInitialized)
            {
                if (!NativeEngineBridge.InitializeEngine(NativeEngineBridge.RHIPlatformMetal, enableDebug: false))
                {
                    Logger.Warning("InitializeEngine returned false");
                    ShowPlaceholder(true);
                    return false;
                }
                _engineInitialized = true;
                Logger.Info("Engine initialized via RHI (Metal)");
            }

            // Step 2: Embed MTKView into Avalonia's contentView via the engine's
            // RHI forward pipeline. The host handle is the NSView pointer of
            // Avalonia's main window contentView, obtained via the engine's
            // native API. The MTKView attaches as a subview with autoresizing
            // mask set so it tracks the host's bounds.
            IntPtr hostView = NativeEngineBridge.GetAvaloniaContentView();
            if (hostView == IntPtr.Zero)
            {
                Logger.Warning("GetAvaloniaContentView returned null");
                ShowPlaceholder(true);
                return false;
            }

            _surfaceId = NativeEngineBridge.CreateEmbeddedRenderSurface(hostView, width, height);
            if (_surfaceId == uint.MaxValue)
            {
                Logger.Warning("CreateEmbeddedRenderSurface failed");
                ShowPlaceholder(true);
                return false;
            }
            Logger.Info($"Embedded render surface {_surfaceId} created ({width}x{height})");

            // Position the MTKView over the EngineViewport3D control's bounds
            // (in the host's coordinate space — the Avalonia contentView's
            // top-left origin).
            PositionEmbeddedSurface(width, height);

            // Step 3: Set up a default editor camera so the forward pipeline
            // has a view to render from. Scene content (entities, lights,
            // render items) is set up via RenderPipelineAPI / EntityAPI.
            _cameraId = CreateEditorCamera(width, height);
            if (_cameraId != 0)
            {
                Logger.Info($"Editor camera {_cameraId} created");
            }

            // Step 4: Populate a small test scene so the viewport renders
            // something more interesting than the post-process clear color.
            // 4 spheres in a row + a directional-style point light above.
            CreateTestScene();

            // Push the initial orbit state to the engine so the camera
            // starts at the expected default view (looking at origin from
            // -Z, distance 12, level with target). The entity CreateEntity
            // already wrote (0, 1, -12) which is offset by +1 in Y from
            // our target=origin default, so we apply once to reconcile.
            ApplyCameraTransform();

            _viewportInitialized = true;
            ShowPlaceholder(false);
            StartRenderLoop();
            Logger.Info("3D viewport initialized via RHI forward pipeline");
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error("Failed to initialize 3D viewport", ex);
            ShowPlaceholder(true);
            return false;
        }
    }

    // Create a default editor camera pointing at the origin. Returns the
    // camera id (0 on failure). Uses ResourceAPI::CreateCamera which takes
    // flat scalars — no struct marshaling needed.
    private uint CreateEditorCamera(int width, int height)
    {
        // Create a real entity first — ResourceAPI::CreateCamera reads the
        // entity's Transform position via metal_camera::update(), and
        // entity_id 0 (id::invalid_id) would deref null.
        // Camera back along -Z. Camera direction = entity orientation =
        // (0,0,1) (default identity quat) so the view matrix looks toward
        // +Z. Camera position at (0, 1, -12) with FOV 60° so 4 spheres at
        // x = -6,-2,+2,+6 (radius 0.5) all fit comfortably.
        _cameraEntityId = NativeEngineBridge.CreateEntity(0f, 1f, -12f);
        const float fov = 1.0472f;  // 60 degrees
        float aspect = (float)width / (float)height;
        const float nearZ = 0.1f;
        const float farZ = 1000.0f;
        return NativeEngineBridge.CreateCamera(_cameraEntityId, fov, aspect, nearZ, farZ);
    }

    // Populate the world with a simple test scene. NOTE: We intentionally do
    // NOT call AddRenderItem here. The native AddRenderItem asserts
    // material_count > 0 && material_ids != null, and the C ABI has no
    // Create the default opauqe material that the sphere render items share.
    // Lazily compiled on first call inside the engine (xcrun metal + metallib).
    // If the compile step fails (e.g. missing xcrun) the test scene falls back
    // to no render items so the editor still runs without drawing geometry.
    private UInt64 _testSceneMaterial = NativeEngineBridge.InvalidContentId;
    private bool _testSceneMaterialAttempted;

    private bool EnsureTestSceneMaterial()
    {
        if (_testSceneMaterialAttempted) return NativeEngineBridge.IsValidHandle(_testSceneMaterial);
        _testSceneMaterialAttempted = true;
        _testSceneMaterial = NativeEngineBridge.CreateDefaultMaterial();
        if (!NativeEngineBridge.IsValidHandle(_testSceneMaterial))
        {
            Logger.Warning("CreateDefaultMaterial failed — test scene will not render geometry");
            return false;
        }
        Logger.Info($"Test scene material: 0x{_testSceneMaterial:X}");
        return true;
    }

    private void CreateTestScene()
    {
        const uint kSphereType = 0;  // ProceduralMeshAPI enum: 0=Sphere(r, segs, rings)
        float[] sphereParams = { 0.5f, 24, 16 };
        UInt64 sphereGeom = NativeEngineBridge.ProceduralMeshCreate(kSphereType, sphereParams);
        if (!NativeEngineBridge.IsValidHandle(sphereGeom))
        {
            Logger.Warning($"ProceduralMeshCreate(Sphere) returned 0x{sphereGeom:X} (invalid) — skipping test scene");
            return;
        }
        _testSceneGeometry = sphereGeom;

        if (!EnsureTestSceneMaterial()) return;

        // CreateTestScene does NOT call AddRenderItem even though the C ABI
        // is in place (CreateDefaultMaterial + RegisterProceduralMeshSubmesh
        // wired below). Reason: the engine's PSO creation in
        // Engine/Graphics/Metal/MetalContent.cpp::create_pso assumes a
        // vertex shader that uses the [[id(N)]] argument-buffer layout from
        // Engine/Graphics/Metal/shaders/buildin_shader.metal; one-off MSL
        // compiled by `xcrun metal` outside that metallib build lacks the
        // shared argument-buffer infrastructure and the resulting PSO is
        // rejected by the Apple GPU driver in prepass::prepass. To enable
        // drawing, add kEditorSphereVertexShader / kEditorSpherePixelShader
        // to the engine's main metallib build (alongside CommonTypes.metal)
        // and re-enable the AddRenderItem loop below.
        // RegisterProceduralMeshSubmesh now returns a NEW legacy
        // geometry_content_id that bridges the RHI-registered mesh into
        // the legacy add_render_item path. Use that for AddRenderItem.
        UInt64 legacyGeom = NativeEngineBridge.RegisterProceduralMeshSubmesh(sphereGeom);
        if (!NativeEngineBridge.IsValidHandle(legacyGeom))
        {
            Logger.Warning("RegisterProceduralMeshSubmesh failed — spheres will not render");
            return;
        }

        // The engine's cbuffer::offset() is returning corrupt byte offsets for
        // per-object data on this system, so per-entity World matrix propagation
        // is broken. Workaround: bake each sphere's world position into a
        // dedicated mesh by translating the mesh vertices in C++. The shader's
        // World matrix becomes identity for the test scene.
        // (World matrix transform path is left in the shader for future
        // re-enablement once the cbuffer issue is fixed upstream.)
        if (!NativeEngineBridge.IsValidHandle(legacyGeom))
        {
            Logger.Warning("RegisterProceduralMeshSubmesh failed — spheres will not render");
            return;
        }

        // 4 spheres with world positions baked into mesh vertices.
        // Note: engine bug causes 3 of 4 entity_ids to collapse onto slot 0x1,
        // so we use a single entity and 4 different meshes.
        float[] xs = { -6.0f, -2.0f, 2.0f, 6.0f };
        uint singleEntity = NativeEngineBridge.CreateEntity(0f, 0f, 0f);
        if (!NativeEngineBridge.IsValidEntity(singleEntity))
        {
            Logger.Warning("Failed to create single sphere entity");
            return;
        }
        _testSceneEntities.Add(singleEntity);
        for (int i = 0; i < 4; ++i)
        {
            float x = xs[i];
            UInt64 translatedGeom = NativeEngineBridge.CreateTranslatedProceduralMesh(legacyGeom, x, 0f, 0f);
            if (!NativeEngineBridge.IsValidHandle(translatedGeom))
            {
                Logger.Warning($"CreateTranslatedProceduralMesh failed for sphere {i}");
                continue;
            }

            Logger.Info($"[DBG_SPHERE] sphere {i}: translatedGeom=0x{translatedGeom:X} entity=0x{singleEntity:X}");

            UInt64[] materialIds = { _testSceneMaterial };
            uint renderItem = (uint)NativeEngineBridge.AddRenderItem(
                singleEntity, translatedGeom, 1, materialIds);
            if (!NativeEngineBridge.IsValidHandle(renderItem))
            {
                Logger.Warning($"AddRenderItem returned 0x{renderItem:X} (invalid) — skipping sphere {i}");
                continue;
            }
            _testSceneRenderItems.Add(renderItem);
        }
        Logger.Info($"Test scene: {_testSceneEntities.Count} entities, {_testSceneRenderItems.Count} render items, rhi_geom=0x{sphereGeom:X}, legacy_geom=0x{legacyGeom:X}, material=0x{_testSceneMaterial:X}");

        uint lightEntity = NativeEngineBridge.CreateEntity(0f, 3f, 0f);
        if (NativeEngineBridge.IsValidEntity(lightEntity))
        {
            _testSceneEntities.Add(lightEntity);
            float[] white = { 1.0f, 1.0f, 1.0f };
            uint added = NativeEngineBridge.AddEntityLight(
                lightEntity, 0 /* point */, 5.0f, white, 50f, 0f, 0f, true);
            if (added != 0) Logger.Info("Test scene point light added (intensity=5)");
        }
    }

    private void DestroyTestScene()
    {
        foreach (var ri in _testSceneRenderItems)
        {
            NativeEngineBridge.RemoveRenderItem(ri);
        }
        _testSceneRenderItems.Clear();

        foreach (var e in _testSceneEntities)
        {
            NativeEngineBridge.RemoveEntityLight(e);
        }

        if (_testSceneGeometry != 0 && _testSceneGeometry != NativeEngineBridge.InvalidContentId)
        {
            NativeEngineBridge.ProceduralMeshDestroy(_testSceneGeometry);
            _testSceneGeometry = 0;
        }

        // Test scene entities were created via the simplified CreateEntity
        // helper which has no matching DestroyEntity in the C ABI for the
        // scene-only flow — they're cleaned up on engine shutdown.
        _testSceneEntities.Clear();
    }

    private UInt64 _testSceneGeometry;
    private readonly System.Collections.Generic.List<uint> _testSceneEntities = new();
    private readonly System.Collections.Generic.List<uint> _testSceneRenderItems = new();

    private DispatcherTimer? _renderTimer;
    private DateTime _lastTick;

    private void StartRenderLoop()
    {
        if (_renderTimer != null) return;
        _lastTick = DateTime.UtcNow;
        _renderTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromMilliseconds(33)
        };
        _renderTimer.Tick += OnRenderTick;
        _renderTimer.Start();
        Logger.Info("Editor render loop started (30Hz, RHI forward pipeline)");
    }

    private void StopRenderLoop()
    {
        _renderTimer?.Stop();
        _renderTimer = null;
    }

    private void OnRenderTick(object? sender, EventArgs e)
    {
        if (!NativeEngineBridge.IsLoaded || !_viewportInitialized) return;
        var now = DateTime.UtcNow;
        var dt = (float)(now - _lastTick).TotalSeconds;
        if (dt <= 0f) dt = 0.016f;
        if (dt > 0.1f) dt = 0.1f;
        _lastTick = now;
        _animTime += dt;

        // Build the frame info and call RenderFrame. The engine drives the
        // full forward pipeline: depth prepass, gpass, shadow, compose,
        // TAA, SSAO, SSGI, post-process, then presents to the MTKView via
        // the surface's end_frame() which calls mtkView->draw() to commit
        // the presentable drawable to the layer.
        //
        // Skip the call entirely when there are no render items: with the
        // current test scene we have no materials to bind, so render_items
        // stays empty and there is nothing for RenderFrame to do. The
        // surface still displays its clear color and the tick continues
        // running so the editor stays responsive.
        if (_testSceneRenderItems.Count == 0) return;
        EnsureRenderItemArrayPinned();
        var p = new NativeEngineBridge.RenderFrameParams
        {
            SurfaceId = _surfaceId,
            CameraId = _cameraId,
            LightSetKey = 0,
            RenderItemCount = (uint)_testSceneRenderItems.Count,
            RenderItemIds = _renderItemIdsPtr,
            Thresholds = IntPtr.Zero,
            AverageFrameTime = dt * 1000f,
            LastFrameTime = dt * 1000f,
        };
        NativeEngineBridge.RenderFrame(p);
    }

    // Keep a pinned copy of the render-item-id array so the unmanaged
    // RenderFrame can read it without GC moving it. Refreshed whenever
    // the list mutates (which only happens in CreateTestScene/DestroyTestScene).
    private GCHandle _renderItemIdsHandle;
    private IntPtr _renderItemIdsPtr = IntPtr.Zero;
    private int _pinnedRenderItemCount = 0;
    private void EnsureRenderItemArrayPinned()
    {
        // Re-pin the array whenever the render-item list changes; the engine
        // reads the array contents each frame, so a stale pinned copy of an
        // earlier shorter list would cause only the first N render items to
        // be drawn even after new ones were added via AddRenderItem.
        if (_renderItemIdsPtr != IntPtr.Zero && _pinnedRenderItemCount == _testSceneRenderItems.Count)
            return;
        if (_renderItemIdsHandle.IsAllocated)
            _renderItemIdsHandle.Free();
        var arr = _testSceneRenderItems.ToArray();
        _renderItemIdsHandle = GCHandle.Alloc(arr, GCHandleType.Pinned);
        _renderItemIdsPtr = _renderItemIdsHandle.AddrOfPinnedObject();
        _pinnedRenderItemCount = arr.Length;
    }

    private void OnDetached(object? sender, VisualTreeAttachmentEventArgs e)
    {
        StopRenderLoop();
        LayoutUpdated -= OnLayoutUpdated;
        DestroyTestScene();
        if (_renderItemIdsHandle.IsAllocated)
        {
            _renderItemIdsHandle.Free();
            _renderItemIdsPtr = IntPtr.Zero;
        }
        if (_surfaceId != uint.MaxValue)
        {
            NativeEngineBridge.RemoveRenderSurface(_surfaceId);
            _surfaceId = uint.MaxValue;
        }
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
        if (_surfaceId == uint.MaxValue) return;
        var width = (int)Bounds.Width;
        var height = (int)Bounds.Height;
        if (width <= 0 || height <= 0) return;

        var currentOrigin = this.PointToScreen(new Point(0, 0));
        var sizeChanged = _lastWidth != width || _lastHeight != height;
        var positionChanged = _lastPositionX != (int)currentOrigin.X
                           || _lastPositionY != (int)currentOrigin.Y;

        if (sizeChanged)
        {
            _lastWidth = width;
            _lastHeight = height;
            NativeEngineBridge.ResizeRenderSurface(_surfaceId, (uint)width, (uint)height);
        }
        if (sizeChanged || positionChanged)
        {
            _lastPositionX = (int)currentOrigin.X;
            _lastPositionY = (int)currentOrigin.Y;
            PositionEmbeddedSurface(width, height);
        }
    }

    private int _lastPositionX = int.MinValue;
    private int _lastPositionY = int.MinValue;
    private int _lastWidth;
    private int _lastHeight;

    // LayoutUpdated drives init and positioning because OnSizeChanged can
    // fire before the parent Border has a real layout (Bounds = 0x0), which
    // would place the MTKView at the wrong screen coordinates.


    // Translate this control's Avalonia coordinates into the host NSView's
    // coordinate space and tell the engine to place the MTKView there.
    private void PositionEmbeddedSurface(int width, int height)
    {
        if (_surfaceId == uint.MaxValue) return;

        // PointToScreen returns screen coordinates (top-left origin).
        // The host NSView (Avalonia contentView) shares the window's
        // coordinate space, so we subtract the host view's screen origin
        // to get the MTKView's frame inside the host.
        var origin = this.PointToScreen(new Point(0, 0));
        var hostOrigin = GetHostViewScreenOrigin();
        int x = (int)(origin.X - hostOrigin.X);
        int y = (int)(origin.Y - hostOrigin.Y);

        NativeEngineBridge.SetEmbeddedSurfaceFrame(_surfaceId, x, y, (uint)width, (uint)height);
    }

    private Avalonia.PixelPoint GetHostViewScreenOrigin()
    {
        // Avalonia's contentView fills the window's content area starting at
        // the title bar's bottom. macOS screen coordinates have Y growing up
        // while NSView coordinates have Y growing down, but both use the
        // title bar bottom as the origin so the math works out.
        var window = TopLevel.GetTopLevel(this) as Window;
        if (window == null) return new Avalonia.PixelPoint(0, 0);
        return window.PointToScreen(new Point(0, 0));
    }

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
        _lastPointer = point.Position;
        Focus();
        e.Handled = true;
    }

    private void OnPointerMoved(object? sender, Avalonia.Input.PointerEventArgs e)
    {
        try { System.IO.File.AppendAllText("/tmp/wheel.log", $"[MOVE] button={_activeButton} pos={e.GetCurrentPoint(this).Position} timestamp={DateTime.UtcNow:HH:mm:ss.fff}\n"); } catch {}
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
        if (e.InitialPressMouseButton == _activeButton)
        {
            _activeButton = Avalonia.Input.MouseButton.None;
            e.Handled = true;
        }
    }

    private void OnPointerWheelChanged(object? sender, Avalonia.Input.PointerWheelEventArgs e)
    {
        try { System.IO.File.AppendAllText("/tmp/wheel.log", $"[WHEEL] delta=({e.Delta.X},{e.Delta.Y}) kind={e.KeyModifiers} timestamp={DateTime.UtcNow:HH:mm:ss.fff}\n"); } catch {}
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
        // F = frame the test scene (reset orbit state to the default view).
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
}
