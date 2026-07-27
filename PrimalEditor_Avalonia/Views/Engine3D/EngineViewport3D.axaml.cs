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

                UInt64 deviceHandle = NativeEngineBridge.GetEngineDeviceHandle();
                if (deviceHandle != 0 && NativeEngineBridge.CreateStandardRenderPipeline(deviceHandle))
                {
                    Logger.Info("StandardRenderPipeline created (Lumen=Low)");
                    NativeEngineBridge.PipelineSetEditorMode(true);
                    NativeEngineBridge.SetPipelineViewportSize(width, height);
                }
                else
                {
                    Logger.Warning("StandardRenderPipeline creation failed — falling back to legacy gpass path");
                }
            }

            // Step 2: Embed MTKView into the MTKViewHost's child NSView via the
            // engine's RHI forward pipeline. The MTKViewHost is a NativeControlHost
            // that Avalonia created inside its visual tree; its child NSView is
            // positioned and sized by Avalonia's layout system. The MTKView
            // attaches as a subview of that child NSView, so it inherits
            // correct layout from Avalonia instead of fighting it (which was
            // the root cause of the macOS 26.5.1 FramePacing crash and the
            // 10-frame-then-deadlock pattern).
            //
            // The MTKViewHost is the **only** supported host. There is no
            // fallback to NSWindow.contentView (a previous legacy path that
            // did `NS::Application::sharedApplication()->windows()[0]->contentView()`):
            // that path is unreliable because (a) `windows()[0]` is not
            // guaranteed to be the editor's MainWindow (Avalonia may register
            // other hidden windows), (b) the contentView is the Avalonia
            // root, not a NativeControlHost's child NSView, and (c) its
            // bounds may be 0×0 at the time the engine first tries to draw
            // (mid-layout). All three cases produce a `currentDrawable` that
            // is either nil or has a 0×0 backing texture, and a
            // `presentDrawable:` block then fires on the freed drawable
            // when the MTKView is later reparented by Avalonia's layout
            // pass — the EXC_BAD_ACCESS at
            // -[_MTLCommandBuffer presentDrawable:options:]_block_invoke
            // signature we've seen. Failing fast here is safer than
            // silently using the wrong host.
            //
            // The MTKViewHost is looked up by name via FindControl since
            // the project does not use Avalonia's XAML compiler
            // (Microsoft.NET.Sdk, not Avalonia.NET.Sdk), so x:Name does
            // not auto-generate fields. The Name="MTKHostView" attribute
            // on the XAML element is the Avalonia-level Name (not the WPF
            // x:Name), and FindControl<T>("name") walks the visual tree
            // looking for a control whose Name matches.
            var mtkHost = this.FindControl<PrimalEditor_Avalonia.Views.Engine3D.MTKViewHost>("MTKHostView");
            IntPtr hostView = mtkHost?.HostViewHandle ?? IntPtr.Zero;
            if (hostView == IntPtr.Zero)
            {
                // Schedule a retry — the MTKViewHost's CreateNativeControlCore
                // may fire on the next layout pass. Don't fall back to the
                // legacy windows()[0] path; that was the original bug.
                Logger.Warning("MTKViewHost child is null; will retry on next layout pass");
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

            // Defensive double-check: verify the engine actually attached an
            // MTKView to the host. The C ABI returns a non-error id even when
            // the host view had 0x0 bounds at creation time (the surface is
            // created on the next layout pass), so a non-error id alone is not
            // a guarantee that a drawable is reachable.
            var surfaceView = NativeEngineBridge.GetSurfaceView(_surfaceId);
            if (surfaceView == IntPtr.Zero)
            {
                Logger.Warning($"CreateEmbeddedRenderSurface returned id {_surfaceId} but no MTKView is attached (host view likely had 0x0 bounds); will retry");
                _surfaceId = uint.MaxValue;
                ShowPlaceholder(true);
                return false;
            }

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
            StartRenderLoop();

            // Configure the StandardRenderPipeline for editor preview.
            // Editor mode uses the lightweight forward path; viewport
            // size must mirror the surface size so internal buffers
            // scale correctly. Both calls are no-ops if the pipeline
            // singleton is not yet created (graceful early start).
            NativeEngineBridge.PipelineSetEditorMode(true);
            NativeEngineBridge.SetPipelineViewportSize(width, height);

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
        // ResourceAPI::CreateCamera stores fov in Camera::field_of_view, but
        // MetalCamera::update() multiplies it by π before passing to
        // createPerspectiveFovLH — i.e. the engine treats the value as a
        // fraction of π, not radians. 60° → fov = 60° / 180° = 1/3.
        _cameraEntityId = NativeEngineBridge.CreateEntity(0f, 1f, -12f);
        const float fov = 1f / 3f;
        float aspect = (float)width / (float)height;
        const float nearZ = 0.1f;
        const float farZ = 1000.0f;
        return NativeEngineBridge.CreateCamera(_cameraEntityId, fov, aspect, nearZ, farZ);
    }

    // Populate the world with a simple test scene. NOTE: We intentionally do
    // NOT call AddRenderItem here. The native AddRenderItem asserts
    // material_count > 0 && material_ids != null, and the C ABI has no
    // way to associate a legacy material with a StandardRenderPipeline mesh
    // entity — ForwardSceneRenderer uses its internal default materials
    // when RegisterMeshResource is called with no texture ids. See the
    // comment at the top of CreateTestScene for why we use a single shared
    // RHI geometry with per-sphere entity transforms instead.
    private void CreateTestScene()
    {
        const uint kSphereType = 0;
        float[] sphereParams = { 0.5f, 24, 16 };
        UInt64 sphereGeom = NativeEngineBridge.ProceduralMeshCreate(kSphereType, sphereParams);
        if (!NativeEngineBridge.IsValidHandle(sphereGeom))
        {
            Logger.Warning($"ProceduralMeshCreate(Sphere) returned 0x{sphereGeom:X} (invalid) — skipping test scene");
            return;
        }
        Logger.Info($"sphere geometry allocated at slot {sphereGeom} (0x{sphereGeom:X16})");
        _testSceneGeometry = sphereGeom;

        // StandardRenderPipeline::RegisterMeshEntity creates an ECS entity with
        // an identity transform — distinct spheres need distinct entity
        // transforms, not distinct geometry blobs. The previous
        // CreateTranslatedProceduralMesh workaround baked offsets into the
        // vertex buffer; that path is legacy-only and produces RHI ids that
        // ForwardSceneRenderer::RegisterMeshResource can't resolve.
        float[] xs = { -6.0f, -2.0f, 2.0f, 6.0f };
        for (int i = 0; i < 4; ++i)
        {
            UInt64 meshEntity = NativeEngineBridge.PipelineRegisterMeshEntity(sphereGeom, (UInt64[]?)null);
            if (meshEntity == 0 || meshEntity == UInt64.MaxValue)
            {
                Logger.Warning($"PipelineRegisterMeshEntity returned 0x{meshEntity:X} (invalid) — sphere {i} skipped");
                continue;
            }
            // Push world position via ECS transform component. SetEntityTransform
            // treats rotation as Euler angles (EngineAPI calls euler_to_quat),
            // not a quaternion — pass all-zero for identity orientation.
            float[] pos = { xs[i], 0f, 0f };
            float[] rot = { 0f, 0f, 0f };
            float[] scl = { 1f, 1f, 1f };
            NativeEngineBridge.SetEntityTransform((uint)meshEntity, pos, rot, scl);

            _testSceneRenderItems.Add(meshEntity);
            Logger.Info($"[DBG_SPHERE] sphere {i}: geom=0x{sphereGeom:X} meshEntity=0x{meshEntity:X} pos=({xs[i]},0,0)");
        }

        byte[] sunDesc = new byte[NativeEngineBridge.PipelineLightDescSize];
        WritePipelineLightDesc(sunDesc,
            type: 0,
            r: 1.0f, g: 1.0f, b: 1.0f,
            intensity: 5.0f,
            posX: 0f, posY: 3f, posZ: 0f,
            dirX: 0.3f, dirY: -1.0f, dirZ: 0.2f,
            range: 0f, umbra: 0f, penumbra: 0f,
            enabled: 1);
        UInt64 lightId = NativeEngineBridge.PipelineRegisterLightEntity(sunDesc);
        if (lightId != 0 && lightId != UInt64.MaxValue)
        {
            _testSceneLights.Add(lightId);
            Logger.Info("Test scene directional light added (intensity=5)");
        }
    }

    // Mirrors EngineDLL/PipelineLightDesc.h layout (64 bytes):
    //   u32 type; f32 color[3]; f32 intensity;
    //   f32 position[3]; f32 direction[3];
    //   f32 range; f32 umbra; f32 penumbra;
    //   u32 is_enabled; u32 _pad[3];
    private static void WritePipelineLightDesc(
        byte[] buf,
        uint type, float r, float g, float b, float intensity,
        float posX, float posY, float posZ,
        float dirX, float dirY, float dirZ,
        float range, float umbra, float penumbra,
        uint enabled)
    {
        int o = 0;
        BitConverter.GetBytes(type).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(r).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(g).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(b).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(intensity).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(posX).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(posY).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(posZ).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(dirX).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(dirY).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(dirZ).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(range).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(umbra).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(penumbra).CopyTo(buf, o); o += 4;
        BitConverter.GetBytes(enabled).CopyTo(buf, o); o += 4;
    }

    private void DestroyTestScene()
    {
        foreach (var meshEntity in _testSceneRenderItems)
        {
            NativeEngineBridge.PipelineUnregisterMeshEntity(meshEntity);
        }
        _testSceneRenderItems.Clear();

        foreach (var lightId in _testSceneLights)
        {
            NativeEngineBridge.PipelineUnregisterLightEntity(lightId);
        }
        _testSceneLights.Clear();

        if (_renderTarget != 0)
        {
            NativeEngineBridge.DestroyRenderTarget(_renderTarget);
            _renderTarget = 0;
        }

        if (_testSceneGeometry != 0 && _testSceneGeometry != NativeEngineBridge.InvalidContentId)
        {
            NativeEngineBridge.ProceduralMeshDestroy(_testSceneGeometry);
            _testSceneGeometry = 0;
        }

        _testSceneEntities.Clear();
    }

    private UInt64 _testSceneGeometry;
    private readonly System.Collections.Generic.List<uint> _testSceneEntities = new();
    private readonly System.Collections.Generic.List<UInt64> _testSceneRenderItems = new();
    private readonly System.Collections.Generic.List<UInt64> _testSceneLights = new();
    private UInt64 _renderTarget;
    private int _renderTargetWidth;
    private int _renderTargetHeight;

    private DispatcherTimer? _renderTimer;
    private DateTime _lastTick;
    private uint _nativeRenderLoopId;

    // Tracks every live native render loop id so App.OnExit can Destroy
    // each one with the correct id. EditorRenderLoop.Stop(0)/Destroy(0)
    // are no-ops by design, so App can't simply call them — it needs the
    // concrete id(s) the viewports allocated. A static set is sufficient
    // because there is exactly one EngineViewport3D instance in the editor.
    private static readonly System.Collections.Generic.HashSet<uint> s_liveLoopIds = new();

    private void StartRenderLoop()
    {
        if (_nativeRenderLoopId != 0) return;
        _lastTick = DateTime.UtcNow;
        _nativeRenderLoopId = NativeEngineBridge.EditorRenderLoop.Start(60, OnNativeFrame);
        if (_nativeRenderLoopId != 0)
        {
            s_liveLoopIds.Add(_nativeRenderLoopId);
            Logger.Info("Editor render loop started (60Hz native, RHI forward pipeline)");
            return;
        }
        _renderTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromMilliseconds(33)
        };
        _renderTimer.Tick += OnRenderTick;
        _renderTimer.Start();
        Logger.Warning("Native render loop unavailable — fell back to DispatcherTimer 30Hz");
    }

    private void StopRenderLoop()
    {
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
        if (_testSceneRenderItems.Count == 0) return;
        _animTime += dt;

        NativeEngineBridge.PipelineRenderFrame(_renderTarget, _cameraId, _surfaceId);
        NativeEngineBridge.BlitRenderTargetToSurface(_renderTarget, _surfaceId);
    }

    private void OnDetached(object? sender, VisualTreeAttachmentEventArgs e)
    {
        StopRenderLoop();
        LayoutUpdated -= OnLayoutUpdated;
        DestroyTestScene();
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

        var sizeChanged = _lastWidth != width || _lastHeight != height;
        if (sizeChanged)
        {
            _lastWidth = width;
            _lastHeight = height;
            NativeEngineBridge.ResizeRenderSurface(_surfaceId, (uint)width, (uint)height);
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
            if (_cameraEntityId != 0 && width > 0 && height > 0)
            {
                NativeEngineBridge.SetEntityCameraAspectRatio(_cameraEntityId, (float)width / height);
            }

            PositionEmbeddedSurface(width, height);
        }
    }

    private int _lastWidth;
    private int _lastHeight;

    // LayoutUpdated drives init and positioning because OnSizeChanged can
    // fire before the parent Border has a real layout (Bounds = 0x0), which
    // would place the MTKView at the wrong screen coordinates.

    // The MTKView is attached as a subview of the NativeControlHost's child
    // NSView (MTKViewHost). That host NSView's bounds already match this
    // control's Bounds thanks to Avalonia's layout pass, so the MTKView
    // should always fill its host at host-local origin (0, 0). Translating
    // screen coordinates to host-local was double-counting the host's
    // already-correct position and was clipping the MTKView to the top-left
    // corner or hiding it entirely on resize.
    private void PositionEmbeddedSurface(int width, int height)
    {
        if (_surfaceId == uint.MaxValue) return;
        NativeEngineBridge.SetEmbeddedSurfaceFrame(_surfaceId, 0, 0, (uint)width, (uint)height);
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
