using System;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

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

    private readonly LightIconRenderer _sceneIconRenderer = new();
    private bool _gizmoVisible;
    private (float x, float y, float z) _gizmoTarget;
    private GizmoOverlay.GizmoMode _gizmoMode = GizmoOverlay.GizmoMode.Translate;

    // Orbit camera state. The camera is always looking at `_cameraTarget`
    // from a distance `_cameraDistance`, with `_cameraYaw` (around world up)
    // and `_cameraPitch` (above/below the horizon). Editing these via
    // ApplyCameraTransform is the only way the 3D viewport's view matrix
    // changes.
    private float[] _cameraTarget = new float[] { 0f, 0f, 0f };
    private float _cameraDistance = 12f;
    private float _cameraYaw = 0f;       // radians; 0 = looking down +Z (camera at -Z relative to target)
    private float _cameraPitch = EditorCameraState.DefaultPitch;

    // A7 相机书签：5 个 orbit 位姿槽（yaw/pitch/distance/target），Ctrl+Shift+数字存、数字取。
    private readonly (float yaw, float pitch, float distance, float[] target)?[] _cameraBookmarks =
        new (float, float, float, float[])?[5];
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
        _isAttached = true;
        // OnDetached unsubscribes LayoutUpdated（标签页切换 / Dock 重布局会
        // detach）；不在此处补订阅的话，第二次 attach 之后 resize 处理永久
        // 失效——布局变化不再触发 _host.Resize / 渲染目标重建，原生表面就会
        // 停留在旧尺寸（错层 bug 的另一诱因）。
        LayoutUpdated -= OnLayoutUpdated;
        LayoutUpdated += OnLayoutUpdated;

        // 标签页往返（Sequencer ⇄ World Editor）：表面保持存活（见
        // OnDetached），这里直接恢复渲染循环，不做销毁级重建——
        // RemoveRenderSurface/DestroyRenderTarget 的会话中途调用会踩中
        // 引擎静态注册表断言（libEngineDLL 二次加载后 surfaces 为空）。
        if (_viewportInitialized && _host is { IsReady: true })
        {
            StartRenderLoop();
            SyncOverlay();
            return;
        }

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
    private bool _isAttached;
    private DispatcherTimer? _retryTimer;

    private bool TryInitializeFromLayout()
    {
        if (!_isAttached) return false;
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
        _renderTimer = new DispatcherTimer(DispatcherPriority.Background)  // TEMP-DEBUG：验证 Dock flush 饿死假说
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

            // N6：PipelineTick 驱动物理 + PFX 粒子框架 tick
            // （pfx::tick 幂等 ensure，未初始化时惰性启动）
            NativeEngineBridge.PipelineTick(dt);

            // Runtime 预览（Run 模式 Render Surface）下不画编辑器视觉
            // （网格 / 世界轴 / 光源图标 / gizmo 线）——与 PrimalPlayer 的画面
            // 口径一致。调试线队列是持久的（编辑器路径每帧 Clear+重提交），
            // 预览中必须每帧清空，否则上一帧残留的网格线会一直画。
            if (_runtimePreview)
            {
                NativeEngineBridge.ClearDebugLines();
            }
            else
            {
                _sceneIconRenderer.DrawEditorVisuals(
                    _scene ?? GetActiveSceneForIconRenderer(),
                    _gizmoVisible,
                    _gizmoTarget,
                    _gizmoMode);
            }

            var surfaceId = (uint)_host.SurfaceId;
            // Render through the editor's Track A camera by default
            // (IsEditorCamera=true, SyncCamerasFromECS bypassed). When the
            // user picks a scene Camera entity via LookThroughCamera, route
            // through the legacy entry so SyncCamerasFromECS picks that
            // entity's CameraComponent + Transform for the view matrices.
            if (NativeEngineBridge.PipelineGetPassDebugView() != 0)
            {
                // Pass view 激活期走 RWCB 路径（RG 输出才可落盘，见
                // Docs/2026-09-20-pass-debug-view.md）。
                NativeEngineBridge.PipelineRenderFrameRWCB(_renderTarget, _cameraId, surfaceId);
            }
            else if (IsLookingThroughSceneCamera)
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

    /// <summary>
    /// CLI/Agent 验证用（Docs/CLI_Agent_Interface_Plan.md Phase 0）：以编辑器
    /// 当前相机渲染一帧进现有离屏 RT 并存为 PNG。与 OnRenderFrameCore 共用
    /// <see cref="_renderInProgress"/> 互锁，绝不与渲染 tick 重入；跳过
    /// BlitRenderTargetToSurface，所以视口画面不受影响（下个 tick 正常刷新）。
    ///
    /// 必须在 UI 线程调用（调用方 CliHost 的编组保证这一点）。视口未初始化
    /// 或引擎忙时返回 false，成功返回 true 并带回实际分辨率。
    /// </summary>
    public bool TryCaptureScreenshot(string absolutePath, out int width, out int height)
        => TryCaptureScreenshot(absolutePath, 0, out width, out height);

    /// <summary>formatOverride 参数保留（0=常规 HDR RT 渲染）。Pass view 激活
    /// 期自动改走 RWCB 入口（见文件内 PipelineRenderFrameRWCB 注释）。</summary>
    public bool TryCaptureScreenshot(string absolutePath, int formatOverride, out int width, out int height)
    {
        width = _renderTargetWidth;
        height = _renderTargetHeight;
        if (!NativeEngineBridge.IsLoaded || !_viewportInitialized) return false;
        if (_renderTarget == 0 || _host == null || !_host.IsReady) return false;
        if (!NativeEngineBridge.IsValidCamera(_cameraId)) return false;
        if (System.Threading.Interlocked.CompareExchange(ref _renderInProgress, 1, 0) != 0) return false;
        try
        {
            var surfaceId = (uint)_host.SurfaceId;
            bool passViewActive = NativeEngineBridge.PipelineGetPassDebugView() != 0;
            try
            {
                // Pass view 激活期走 RWCB 入口（RenderWithCommandBuffer 孪生
                // 路径）：Render() 路径的 RG 输出在离屏 RGBA16F RT 上无法落盘
                // 是先于本功能的基建问题（FinalBlit 同样受影响），见
                // Docs/2026-09-20-pass-debug-view.md。
                if (passViewActive)
                {
                    if (!NativeEngineBridge.PipelineRenderFrameRWCB(
                            _renderTarget, _cameraId, surfaceId)) return false;
                }
                else if (IsLookingThroughSceneCamera)
                {
                    NativeEngineBridge.PipelineRenderFrameForSceneCamera(
                        _renderTarget, _cameraId, surfaceId, _lookThroughSceneEntityId);
                }
                else
                {
                    NativeEngineBridge.PipelineRenderFrameForEditorCamera(_renderTarget, _cameraId, surfaceId);
                }

                // SaveRenderTargetAsPng 内部先 FlushGPU（WaitIdle）再离线读回编码，
                // 与 OnRenderTick 的连续帧路径互不干扰。Pass view 激活期先过
                // 一次 blit：blit 内部对 RWCB 帧后的 RT 做采样状态处理，缺这步
                // 读回全零（PNG 黑屏）。
                if (passViewActive)
                {
                    NativeEngineBridge.BlitRenderTargetToSurface(_renderTarget, surfaceId);
                }
                if (!NativeEngineBridge.SaveRenderTargetAsPng(_renderTarget, absolutePath)) return false;
                width = _renderTargetWidth;
                height = _renderTargetHeight;
                return true;
            }
            finally
            {
                _ = formatOverride;
            }
        }
        finally
        {
            System.Threading.Interlocked.Exchange(ref _renderInProgress, 0);
        }
    }

    /// <summary>
    /// sequence.render 的底层（Part A）：渲染一帧进现有离屏 RT 并存 PNG，
    /// 但取景相机由调用方逐帧指定（sceneCameraEntityId > 0 = 场景相机实体，
    /// 0 = 编辑器相机）——相机 cut 由此实现逐帧切机位，不改动视口当前的
    /// look-through 状态。与 TryCaptureScreenshot 同一套互锁约束：UI 线程
    /// 专用，绝不与渲染 tick 重入；跳过 Blit，视口画面不受影响。
    /// </summary>
    public bool TryRenderSequencePng(string absolutePath, uint sceneCameraEntityId, out int width, out int height)
    {
        width = _renderTargetWidth;
        height = _renderTargetHeight;
        if (!NativeEngineBridge.IsLoaded || !_viewportInitialized) return false;
        if (_renderTarget == 0 || _host == null || !_host.IsReady) return false;
        if (!NativeEngineBridge.IsValidCamera(_cameraId)) return false;
        if (System.Threading.Interlocked.CompareExchange(ref _renderInProgress, 1, 0) != 0) return false;
        try
        {
            var surfaceId = (uint)_host.SurfaceId;
            if (sceneCameraEntityId != 0 && sceneCameraEntityId != NativeEngineBridge.InvalidEntityId)
            {
                NativeEngineBridge.PipelineRenderFrameForSceneCamera(
                    _renderTarget, _cameraId, surfaceId, sceneCameraEntityId);
            }
            else
            {
                NativeEngineBridge.PipelineRenderFrameForEditorCamera(_renderTarget, _cameraId, surfaceId);
            }
            if (!NativeEngineBridge.SaveRenderTargetAsPng(_renderTarget, absolutePath)) return false;
            width = _renderTargetWidth;
            height = _renderTargetHeight;
            return true;
        }
        finally
        {
            System.Threading.Interlocked.Exchange(ref _renderInProgress, 0);
        }
    }

    /// <summary>
    /// CLI/Agent 帧捕获（Phase 2，规划 §5.3）：渲染一帧进现有离屏 RT 并读回
    /// RGBA16F 原始像素（不写盘），供 editor.metrics 的标量遥测在托管侧统计。
    /// 渲染与回读整体持有互锁，绝不与渲染 tick 重入；UI 线程专用。
    /// </summary>
    public bool TryReadFramePixels(out byte[] rgba16f, out int width, out int height)
    {        width = _renderTargetWidth;
        height = _renderTargetHeight;
        rgba16f = Array.Empty<byte>();
        if (!NativeEngineBridge.IsLoaded || !_viewportInitialized) return false;
        if (_renderTarget == 0 || _host == null || !_host.IsReady) return false;
        if (!NativeEngineBridge.IsValidCamera(_cameraId)) return false;
        if (System.Threading.Interlocked.CompareExchange(ref _renderInProgress, 1, 0) != 0) return false;
        try
        {
            if (!RenderFrameWhileLocked((uint)_host.SurfaceId)) return false;

            long pixelBytes = (long)_renderTargetWidth * _renderTargetHeight * 8;
            var buffer = new byte[pixelBytes];
            // DebugReadRenderTarget 包装内部用 GCHandle 固定缓冲，无 unsafe。
            if (!NativeEngineBridge.DebugReadRenderTarget(_renderTarget, buffer))
                return false;
            rgba16f = buffer;
            width = _renderTargetWidth;
            height = _renderTargetHeight;
            return true;
        }
        finally
        {
            System.Threading.Interlocked.Exchange(ref _renderInProgress, 0);
        }
    }

    /// <summary>
    /// CLI/Agent 帧序列捕获（editor.record 的底层）：连续渲染 count 帧并逐帧
    /// 存 PNG 到目录。整段持有互锁保证绝不与渲染 tick 重入；帧间引擎照常
    /// 由渲染 tick 推进，每帧真实耗时记录在 capturedMs。
    /// 任何一帧失败即中止并返回 false（已写出的帧保留供取证）。
    /// </summary>
    public bool TryRecordFrames(int count, string directory, out System.Collections.Generic.List<string> files,
        out int width, out int height, out System.Collections.Generic.List<double> capturedMs)
    {
        files = new System.Collections.Generic.List<string>();
        capturedMs = new System.Collections.Generic.List<double>();
        width = _renderTargetWidth;
        height = _renderTargetHeight;
        if (count < 1) return false;
        if (!NativeEngineBridge.IsLoaded || !_viewportInitialized) return false;
        if (_renderTarget == 0 || _host == null || !_host.IsReady) return false;
        if (!NativeEngineBridge.IsValidCamera(_cameraId)) return false;
        if (!System.IO.Directory.Exists(directory))
            System.IO.Directory.CreateDirectory(directory);

        if (System.Threading.Interlocked.CompareExchange(ref _renderInProgress, 1, 0) != 0) return false;
        try
        {
            for (int i = 0; i < count; i++)
            {
                var sw = System.Diagnostics.Stopwatch.StartNew();
                if (!RenderFrameWhileLocked((uint)_host.SurfaceId))
                    return false;
                var path = System.IO.Path.Combine(directory, $"frame_{i:D4}.png");
                if (!NativeEngineBridge.SaveRenderTargetAsPng(_renderTarget, path))
                    return false;
                capturedMs.Add(sw.Elapsed.TotalMilliseconds);
                files.Add(path);
                width = _renderTargetWidth;
                height = _renderTargetHeight;
            }
            return true;
        }
        finally
        {
            System.Threading.Interlocked.Exchange(ref _renderInProgress, 0);
        }
    }

    /// <summary>调用方已持有 _renderInProgress 互锁时渲染一帧（不读回不写盘）。</summary>
    private bool RenderFrameWhileLocked(uint surfaceId)
    {
        if (IsLookingThroughSceneCamera)
        {
            NativeEngineBridge.PipelineRenderFrameForSceneCamera(
                _renderTarget, _cameraId, surfaceId, _lookThroughSceneEntityId);
        }
        else
        {
            NativeEngineBridge.PipelineRenderFrameForEditorCamera(_renderTarget, _cameraId, surfaceId);
        }
        return true;
    }

    private void ResyncActiveScene()
    {
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        if (lifetime?.MainWindow?.DataContext is not ViewModels.MainWindowViewModel mainVm) return;
        var weVm = mainVm.WorldEditor;
        if (weVm?.ActiveScene == null) return;
        weVm.SceneSynchronizer.Sync(weVm.ActiveScene);
        // ActiveScene is commonly attached before this control creates the
        // StandardRenderPipeline. Re-apply HDR/skybox state now that the
        // ForwardSceneRenderer exists; otherwise the first push was a no-op.
        weVm.SceneEnvironmentViewModel.ApplyToNative();
        // 同理重放 lightmap 页：setter 时管线不存在，PipelineSetLightmapPage
        // 返回 0 → 重启编辑器后烘焙结果永不恢复（2026-11-05 修复）。
        weVm.ApplyActiveSceneLightmapToNative();
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
        _isAttached = false;
        _pendingInit = false;
        _retryTimer?.Stop();
        _retryTimer = null;
        // 标签页切换（Sequencer ⇄ World Editor）会 detach 本控件。这里只
        // 暂停渲染循环，**保留**原生表面 / 渲染目标 / 相机状态——会话中途
        // 的 RemoveRenderSurface 会打中 libEngineDLL 二次加载副本的空
        // surfaces 注册表（断言崩溃），且销毁级重建会把用户相机位姿清掉。
        // 重新 attach 时 OnAttached 直接 StartRenderLoop 复用表面。
        // （进程退出的清理由 App.OnExit / dylib 卸载兜底。）
        StopPreviewFrameCapture();
        StopRenderLoop();
        LayoutUpdated -= OnLayoutUpdated;
        // _renderTarget / _host（原生表面）/ 相机与预览状态全部保留，
        // 供重新 attach 时复用（见 OnAttached 的 keep-alive 分支）。
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

            // 透过场景相机取景（runtime 预览 / LookThroughCamera）时，场景
            // 相机的纵横比也必须跟随 surface。否则任何 resize（典型：打开
            // Sequencer 面板挤压视口）后画面仍按开始预览时的旧纵横比投影，
            // 看起来被拉伸/压扁。编辑器相机在上面已同步，这里补场景相机。
            if (IsLookingThroughSceneCamera && width > 0 && height > 0)
            {
                NativeEngineBridge.SetEntityCameraAspectRatio(_lookThroughSceneEntityId, (float)width / height);
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
            overlay.Scene = _scene ?? GetActiveSceneForIconRenderer();
            // Selection outline (plan §6.1 A1): re-query per frame so the
            // boxes follow gizmo drags. Only the selection is queried
            // (a handful of AABB reads), not the whole scene.
            overlay.SelectionBounds = SelectionBoundsProvider?.Invoke()
                ?? System.Array.Empty<ViewportPicker.AABB>();
            // GI volume wireframe (plan §6.2 B1 L1).
            overlay.VolumeBoxes = VolumeBoxProvider?.Invoke()
                ?? System.Array.Empty<ViewportPicker.AABB>();
            // DDGI probe lattice dots (plan §6.2 B1 L1.5).
            overlay.ProbePoints = ProbePointsProvider?.Invoke()
                ?? System.Array.Empty<(float, float, float)>();
            // A7 视图模式: all-entity bounds wireframes (provider returns
            // null while the toggle is off — no per-frame AABB storm).
            overlay.AllBounds = AllBoundsProvider?.Invoke()
                ?? System.Array.Empty<ViewportPicker.AABB>();
            overlay.InvalidateVisual();
        }
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo)
        {
            gizmo.UpdateCamera(
                _cameraYaw, _cameraPitch, _cameraDistance,
                _cameraTarget[0], _cameraTarget[1], _cameraTarget[2],
                (float)Bounds.Width, (float)Bounds.Height);
        }
        _sceneIconRenderer.UpdateCamera(
            _cameraYaw, _cameraPitch, _cameraDistance,
            _cameraTarget[0], _cameraTarget[1], _cameraTarget[2]);
    }

    /// <summary>Move the gizmo anchor to <paramref name="worldX/Y/Z"/>.</summary>
    public void SetGizmoTarget(float worldX, float worldY, float worldZ)
    {
        _gizmoTarget = (worldX, worldY, worldZ);
        if (this.FindControl<ViewportOverlay>("Overlay") is ViewportOverlay overlay)
        {
            overlay.GizmoTarget = (worldX, worldY, worldZ);
        }
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo)
        {
            gizmo.SetTarget(worldX, worldY, worldZ);
        }
    }

    public void SetGizmoVisible(bool visible)
    {
        _gizmoVisible = visible;
        if (this.FindControl<ViewportOverlay>("Overlay") is ViewportOverlay overlay)
            overlay.ShowGizmo = visible;
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo)
        {
            gizmo.IsVisibleInViewport = visible;
            gizmo.InvalidateVisual();
        }
    }

    public void SetGizmoMode(GizmoOverlay.GizmoMode mode)
    {
        _gizmoMode = mode;
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo)
            gizmo.Mode = mode;
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
        if (!_isAttached || _retryCount > 200) return;
        _retryCount++;
        _retryTimer?.Stop();
        _retryTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(50) };
        _retryTimer.Tick += (s, e) =>
        {
            _retryTimer?.Stop();
            _retryTimer = null;
            if (!_isAttached || _viewportInitialized) return;
            if (TryInitializeFromLayout()) return;
            ScheduleRetry();
        };
        _retryTimer.Start();
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
        _activeButton = ResolvePressedMouseButton(
            point.Properties.IsLeftButtonPressed,
            point.Properties.IsMiddleButtonPressed,
            point.Properties.IsRightButtonPressed);
        if (_activeButton == Avalonia.Input.MouseButton.None) return;

        // Left click on the gizmo takes precedence over entity picking.
        if (_activeButton == Avalonia.Input.MouseButton.Left
            && this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo)
        {
            if (gizmo.TryStartDrag(point.Position))
            {
                // Gizmo owns this pointer sequence. Camera drag state must not
                // remain armed after release, otherwise the next plain pointer
                // movement can rotate the view without a button held.
                _activeButton = Avalonia.Input.MouseButton.None;
                // Let subscribers (WorldEditorView) snapshot the pre-drag
                // transforms before the first delta arrives.
                GizmoDragStart?.Invoke(this, EventArgs.Empty);
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

    /// <summary>
    /// Provides the world-space AABBs of the selected entities for the
    /// selection outline overlay; invoked once per rendered frame by
    /// <see cref="SyncOverlay"/>. Wired by WorldEditorView.
    /// </summary>
    public Func<System.Collections.Generic.IReadOnlyList<ViewportPicker.AABB>>? SelectionBoundsProvider { get; set; }

    /// <summary>GI 体积线框（plan §6.2 B1 L1）：每帧取选中体积的 AABB。</summary>
    public Func<System.Collections.Generic.IReadOnlyList<ViewportPicker.AABB>>? VolumeBoxProvider { get; set; }

    /// <summary>DDGI probe 格点（plan §6.2 B1 L1.5）：每帧取格点世界坐标。</summary>
    public Func<System.Collections.Generic.IReadOnlyList<(float, float, float)>>? ProbePointsProvider { get; set; }

    /// <summary>
    /// A7 视图模式: AABBs of every mesh-bearing entity for the "Bounds"
    /// show-all overlay; invoked once per rendered frame by
    /// <see cref="SyncOverlay"/> only while enabled. Return null (or wire
    /// the provider to null) to disable the query.
    /// </summary>
    public Func<System.Collections.Generic.IReadOnlyList<ViewportPicker.AABB>>? AllBoundsProvider { get; set; }

    /// <summary>Fires when the user left-clicks an entity in the viewport.</summary>
    public event EventHandler<uint>? EntityPicked;

    /// <summary>Fires after a gizmo drag begins (pointer captured by the
    /// gizmo). WorldEditorView captures pre-drag transforms here for undo.</summary>
    public event EventHandler? GizmoDragStart;

    /// <summary>Fires when a gizmo drag ends (pointer released). Subscribers
    /// commit their captured transforms as an undo action here.</summary>
    public event EventHandler? GizmoDragEnd;

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

        if (IsLookingThroughSceneCamera)
        {
            ApplyLookThroughDrag(dx, dy);
        }
        else
        {
            ApplyEditorDrag(dx, dy);
        }
        e.Handled = true;
    }

    /// <summary>Editor-camera drag (LMB orbit, MMB/RMB pan). The editor camera
    /// is a private Track A camera used when no scene Camera is selected.
    /// Its orbit state (_cameraYaw/_cameraPitch/_cameraTarget/_cameraDistance)
    /// lives on this viewport and is independent of any scene entity.</summary>
    private void ApplyEditorDrag(float dx, float dy)
    {
        if (_activeButton == Avalonia.Input.MouseButton.Left)
        {
            // Orbit: horizontal drag = yaw, vertical drag = pitch.
            _cameraYaw -= dx * kOrbitSpeed;
            _cameraPitch -= dy * kOrbitSpeed;
            if (_cameraPitch < kMinPitch) _cameraPitch = kMinPitch;
            if (_cameraPitch > kMaxPitch) _cameraPitch = kMaxPitch;
        }
        else if (IsTranslationDragButton(_activeButton))
        {
            // Pan: translate target and camera in the camera's local screen
            // plane. Right vector is camera-right, up vector is world up
            // (good enough for an editor camera; proper screen-up via the
            // actual view basis is left as a TODO if precision matters).
            float cy = MathF.Cos(_cameraYaw);
            float sy = MathF.Sin(_cameraYaw);
            float cp = MathF.Cos(_cameraPitch);
            float sp = MathF.Sin(_cameraPitch);
            // forward = (sy*cp, sp, cy*cp), from camera to target.
            float[] forward = new[] { sy * cp, sp, cy * cp };
            float[] worldUp = new[] { 0f, 1f, 0f };
            float[] right = Normalize(Cross(worldUp, forward));
            float[] screenUp = Normalize(Cross(forward, right));
            float panScale = _cameraDistance * kPanSpeed;
            _cameraTarget[0] -= right[0] * dx * panScale;
            _cameraTarget[1] -= right[1] * dx * panScale;
            _cameraTarget[2] -= right[2] * dx * panScale;
            _cameraTarget[0] += screenUp[0] * dy * panScale;
            _cameraTarget[1] += screenUp[1] * dy * panScale;
            _cameraTarget[2] += screenUp[2] * dy * panScale;
        }
        ApplyCameraTransform();
    }

    /// <summary>Look-through-scene-camera drag (LMB orbit around the
    /// scene camera's current view, MMB/RMB dolly). The scene Camera
    /// entity's transform is mutated in place. The editor's orbit
    /// state continues to update in the background so switching back
    /// to editor mode is seamless.</summary>
    private void ApplyLookThroughDrag(float dx, float dy)
    {
        if (NativeEngineBridge.IsLoaded == false) return;
        var pos = NativeEngineBridge.GetEntityPosition(_lookThroughSceneEntityId);
        var ori = NativeEngineBridge.GetEntityOrientation(_lookThroughSceneEntityId);

        if (_activeButton == Avalonia.Input.MouseButton.Left)
        {
            // Orbit: derive yaw/pitch from the scene camera's current
            // orientation, apply the drag delta, write back the resulting
            // Euler angles. We use the same kOrbitSpeed as the editor
            // camera so the feel is consistent across modes.
            float yaw = MathF.Atan2(ori.x, ori.z);
            float horiz = MathF.Sqrt(ori.x * ori.x + ori.z * ori.z);
            float pitch = MathF.Atan2(ori.y, horiz);
            yaw -= dx * kOrbitSpeed;
            pitch -= dy * kOrbitSpeed;
            if (pitch < kMinPitch) pitch = kMinPitch;
            if (pitch > kMaxPitch) pitch = kMaxPitch;
            // Reconstruct forward direction from new yaw/pitch. The
            // engine stores orientation as a forward vector (not Euler),
            // so this update keeps the entity's transform conventions.
            float cy = MathF.Cos(yaw);
            float sy = MathF.Sin(yaw);
            float cp = MathF.Cos(pitch);
            float sp = MathF.Sin(pitch);
            NativeEngineBridge.SetEntityOrientation(
                _lookThroughSceneEntityId, new[] { sy * cp, sp, cy * cp });
            _ = pos; // suppress unused
        }
        else if (IsTranslationDragButton(_activeButton))
        {
            // Dolly: move the scene camera along its forward direction.
            // Distance is approximated by the length of the forward
            // vector the orientation encodes; for a unit forward the
            // sign of the forward z component gives forward vs back.
            float dollySpeed = _cameraDistance * kPanSpeed;
            float fx = ori.x, fy = ori.y, fz = ori.z;
            float flen = MathF.Sqrt(fx * fx + fy * fy + fz * fz);
            if (flen > 1e-4f) { fx /= flen; fy /= flen; fz /= flen; }
            // Drag down (dy > 0) pulls camera forward, drag up pushes back.
            // Negative dy means moving up on screen = pushing camera forward.
            pos.x += -dy * fx * dollySpeed;
            pos.y += -dy * fy * dollySpeed;
            pos.z += -dy * fz * dollySpeed;
            // Roll over X axis is unused; the scene camera is rigid-body
            // (rotation is encoded in its forward vector). Pass zeros.
            NativeEngineBridge.SetEntityTransform(
                _lookThroughSceneEntityId,
                new[] { pos.x, pos.y, pos.z },
                new[] { ori.x, ori.y, ori.z },
                new[] { 1f, 1f, 1f });
        }
        // The editor camera underneath is left in place; orbit / pan on
        // the editor state continue so switching back is seamless.
    }

    private void OnPointerReleased(object? sender, Avalonia.Input.PointerReleasedEventArgs e)
    {
        if (this.FindControl<GizmoOverlay>("Gizmo") is GizmoOverlay gizmo && gizmo.IsDragging)
        {
            gizmo.EndDrag();
            _activeButton = Avalonia.Input.MouseButton.None;
            // Subscribers commit their captured pre-drag transforms as an
            // undo action now that the final state is known.
            GizmoDragEnd?.Invoke(this, EventArgs.Empty);
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

    internal static Avalonia.Input.MouseButton ResolvePressedMouseButton(
        bool leftPressed, bool middlePressed, bool rightPressed)
    {
        if (leftPressed) return Avalonia.Input.MouseButton.Left;
        if (middlePressed) return Avalonia.Input.MouseButton.Middle;
        if (rightPressed) return Avalonia.Input.MouseButton.Right;
        return Avalonia.Input.MouseButton.None;
    }

    internal static bool IsTranslationDragButton(Avalonia.Input.MouseButton button) =>
        button is Avalonia.Input.MouseButton.Middle or Avalonia.Input.MouseButton.Right;

    private void OnPointerWheelChanged(object? sender, Avalonia.Input.PointerWheelEventArgs e)
    {
        if (!_viewportInitialized) return;
        // Use the dominant axis: a real mouse wheel on a mouse sends
        // (0, ±step). A trackpad pinch or two-finger scroll sends both
        // axes — vertical (DeltaY) for forward/back, horizontal (DeltaX)
        // for side-to-side. Treating the larger-magnitude axis as the
        // zoom delta gives a single gesture that "scrolls in" or
        // "scrolls out" the camera no matter which direction the
        // fingers move, which is what the user expects on a touchpad.
        // Falls back to dy if both are zero (some trackpads emit a
        // near-zero event for inertial scroll end).
        float dx = (float)e.Delta.X;
        float dy = (float)e.Delta.Y;
        float steps = MathF.Abs(dy) >= MathF.Abs(dx) ? dy : dx;
        if (MathF.Abs(steps) < 0.001f) { e.Handled = true; return; }

        if (IsLookingThroughSceneCamera)
        {
            // Wheel = dolly the scene camera along its forward direction.
            // In editor mode the wheel zooms; in look-through mode it
            // dollies the same scene camera the drag dollies, so the
            // feel is consistent (touchpad gestures map to camera
            // position changes, not abstract orbit distance).
            if (!NativeEngineBridge.IsLoaded) return;
            var pos = NativeEngineBridge.GetEntityPosition(_lookThroughSceneEntityId);
            var ori = NativeEngineBridge.GetEntityOrientation(_lookThroughSceneEntityId);
            float fx = ori.x, fy = ori.y, fz = ori.z;
            float flen = MathF.Sqrt(fx * fx + fy * fy + fz * fz);
            if (flen > 1e-4f) { fx /= flen; fy /= flen; fz /= flen; }
            // Wheel up (steps < 0) = move forward along forward axis.
            float dollyAmount = -steps * 0.5f;  // 0.5 world units per notch
            pos.x += fx * dollyAmount;
            pos.y += fy * dollyAmount;
            pos.z += fz * dollyAmount;
            NativeEngineBridge.SetEntityTransform(
                _lookThroughSceneEntityId,
                new[] { pos.x, pos.y, pos.z },
                new[] { ori.x, ori.y, ori.z },
                new[] { 1f, 1f, 1f });
        }
        else
        {
            // Editor mode: wheel zooms (changes the orbit distance).
            float factor = MathF.Pow(1f - kWheelZoomStep, steps);
            _cameraDistance *= factor;
            if (_cameraDistance < kMinDistance) _cameraDistance = kMinDistance;
            if (_cameraDistance > kMaxDistance) _cameraDistance = kMaxDistance;
            ApplyCameraTransform();
        }
        e.Handled = true;
    }

    // Compute the camera entity's world position and look-direction from the
    // orbit state, then push both into the engine via SetEntityTransform.
    // Position uses spherical coords around _cameraTarget; rotation is
    // passed as Euler angles that the engine converts into a quaternion.
    private void ApplyCameraTransform()
    {
        if (_cameraEntityId == NativeEngineBridge.InvalidEntityId) return;
        WorldProjector.ComputeCameraTransform(
            _cameraTarget[0], _cameraTarget[1], _cameraTarget[2],
            _cameraYaw, _cameraPitch, _cameraDistance,
            out var posX, out var posY, out var posZ,
            out var rotX, out var rotY, out var rotZ);
        float[] pos = new[] { posX, posY, posZ };
        float[] rot = new[] { rotX, rotY, rotZ };
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
		if (e.Key == Avalonia.Input.Key.Delete || e.Key == Avalonia.Input.Key.Back)
		{
			if (ResolveWorldEditorViewModel()?.DeleteSelectedEntities() > 0)
			{
				e.Handled = true;
			}
			return;
		}

        // A7 相机书签: Ctrl+Shift+D1..D5 保存当前 orbit 位姿，D1..D5 恢复。
        // 空槽恢复为 no-op（不消费按键）。书签仅存于编辑器会话内存。
        int? bookmarkIndex = e.Key switch
        {
            Avalonia.Input.Key.D1 => 0,
            Avalonia.Input.Key.D2 => 1,
            Avalonia.Input.Key.D3 => 2,
            Avalonia.Input.Key.D4 => 3,
            Avalonia.Input.Key.D5 => 4,
            _ => null,
        };
        if (bookmarkIndex.HasValue)
        {
            if (e.KeyModifiers.HasFlag(Avalonia.Input.KeyModifiers.Control) &&
                e.KeyModifiers.HasFlag(Avalonia.Input.KeyModifiers.Shift))
            {
                _cameraBookmarks[bookmarkIndex.Value] =
                    (_cameraYaw, _cameraPitch, _cameraDistance, (float[])_cameraTarget.Clone());
                Logger.Info($"Camera bookmark {bookmarkIndex.Value + 1} saved");
                e.Handled = true;
                return;
            }
            if (e.KeyModifiers == Avalonia.Input.KeyModifiers.None)
            {
                var bm = _cameraBookmarks[bookmarkIndex.Value];
                if (bm.HasValue)
                {
                    _cameraYaw = bm.Value.yaw;
                    _cameraPitch = bm.Value.pitch;
                    _cameraDistance = bm.Value.distance;
                    _cameraTarget = (float[])bm.Value.target.Clone();
                    ApplyCameraTransform();
                    Logger.Info($"Camera bookmark {bookmarkIndex.Value + 1} restored");
                    e.Handled = true;
                }
                return;
            }
        }

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
            _cameraPitch = EditorCameraState.DefaultPitch;
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

    // =======================================================================
    // Runtime 预览（Run 模式 "Render Surface"，§Run 三选一）：不开外部进程，
    // 在编辑器内嵌 render surface 上跑真正的 runtime 渲染路径。与 PrimalPlayer
    // 的两个对齐点：
    //   1. PipelineSetEditorMode(false) —— 管线从"编辑器轻量前向"切到 runtime
    //      （deferred/RenderGraph）路径，与 player main.cpp 的 PipelineSetEditorMode(0)
    //      同一语义；
    //   2. LookThroughCamera(场景相机实体) —— SyncCamerasFromECS 读该实体的
    //      CameraComponent + Transform 作为视矩阵，与 player 用场景相机取景一致。
    // 管线功能开关（RENDER 组合框的 pass 预设 / GI 面板）在 runtime 路径下同样
    // 生效，所以"按图形功能跑实际渲染"在本模式下完整成立。Stop 退出并恢复
    // 编辑器状态（PipelineSetEditorMode(true) + 编辑器相机 + 叠层显示）。
    // =======================================================================

    private bool _runtimePreview;

    /// <summary>当前是否处于 runtime 预览（Run 模式 Render Surface 进行中）。</summary>
    public bool IsRuntimePreview => _runtimePreview;

    /// <summary>
    /// 进入 runtime 预览：切 runtime 渲染路径 + 透过场景相机取景 + 隐藏编辑器
    /// 叠层。要求场景中存在 Camera 实体且已同步到引擎（与 player 的取景约束
    /// 相同）。已在预览中时幂等返回 true。
    /// </summary>
    public bool BeginRuntimePreview(ViewModels.WorldEditorViewModel vm)
    {
        if (_runtimePreview) return true;
        if (!NativeEngineBridge.IsLoaded || !_viewportInitialized || _renderTarget == 0)
        {
            Logger.Warning("[Run] render surface 未就绪，无法进入 runtime 预览");
            return false;
        }
        var scene = _scene ?? vm.ActiveScene;
        if (scene == null)
        {
            Logger.Warning("[Run] 没有激活场景，无法进入 runtime 预览");
            return false;
        }
        var cameraEntity = FindCameraEntity(scene.Entities);
        if (cameraEntity == null)
        {
            Logger.Warning($"[Run] 场景 '{scene.Name}' 没有 Camera 实体 —— runtime 预览需要至少一个相机" +
                           "（entity.create kind=Camera 或 Inspector 添加）");
            return false;
        }
        var nativeId = vm.GetNativeEntityId(cameraEntity);
        if (nativeId == 0)
        {
            Logger.Warning($"[Run] 相机实体 '{cameraEntity.Name}' 尚未同步到引擎（engine.tick 后重试）");
            return false;
        }

        // 场景相机纵横比跟随内嵌 surface，避免拉伸（与 player 的窗口纵横比同步对应）。
        if (Bounds.Width > 0 && Bounds.Height > 0)
            NativeEngineBridge.SetEntityCameraAspectRatio(nativeId, (float)Bounds.Width / (float)Bounds.Height);

        // 先以场景相机取景（仍处编辑器模式），渲染一帧"干净底图"：前向路径每帧
        // LoadAction::Clear 会清掉 RT 里残留的编辑器画面（网格/图标），而 runtime
        // （deferred/RenderGraph）路径对既有 RT 内容做合成、从不清背景——不清底
        // 的话旧画面会在背景像素上永久残留。清空调试线队列保证底图无网格/图标；
        // RT 身份不变（重建会让 deferred 路径输出全黑，见 2026-09-06 实验）。
        LookThroughCamera(nativeId);
        NativeEngineBridge.ClearDebugLines();
        if (System.Threading.Interlocked.CompareExchange(ref _renderInProgress, 1, 0) == 0)
        {
            try
            {
                NativeEngineBridge.PipelineRenderFrameForSceneCamera(
                    _renderTarget, _cameraId, (uint)_host.SurfaceId, nativeId);
            }
            finally
            {
                System.Threading.Interlocked.Exchange(ref _renderInProgress, 0);
            }
        }

        NativeEngineBridge.PipelineSetEditorMode(false);
        SetEditorOverlaysVisible(false);
        _runtimePreview = true;
        Logger.Info($"[Run] runtime 预览开始：editor mode=false (pipeline readback={NativeEngineBridge.PipelineIsEditorMode()}), " +
                    $"camera='{cameraEntity.Name}' (native {nativeId})");
        // 已知引擎侧缺口（2026-09-06 实验）：runtime（deferred/GPUDriven）路径消费
        // RenderSceneSnapshot（runtime 注册链），编辑器 SceneRuntimeSynchronizer
        // 喂的是前向路径的 RenderScene——预览画面因此不含场景实体（仅环境/后处理）。
        // 实体进 runtime 链属引擎集成项，落地后本模式即为完整的"内嵌 runtime 画面"。
        Logger.Info("[Run] runtime 预览限制：编辑器同步链尚未接入 runtime 实体注册（RenderSceneSnapshot），画面暂不含场景实体");
        return true;
    }

    /// <summary>退出 runtime 预览并恢复编辑器状态。未在预览中时为 no-op。</summary>
    public void EndRuntimePreview()
    {
        if (!_runtimePreview) return;
        StopPreviewFrameCapture();
        if (NativeEngineBridge.IsLoaded)
            NativeEngineBridge.PipelineSetEditorMode(true);
        LookThroughCamera(0);
        SetEditorOverlaysVisible(true);
        _runtimePreview = false;
        Logger.Info("[Run] runtime 预览结束：editor mode=true, 编辑器相机恢复");
        RuntimePreviewEnded?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>
    /// runtime 预览结束时触发（Stop 按钮 / 切场景两条退出路径都会经过
    /// <see cref="EndRuntimePreview"/>）。WorldEditorView 用它恢复工具栏上
    /// 预览期间禁用的编辑器视口开关（VIEW/Bounds/Ortho/Rec）。
    /// </summary>
    public event System.EventHandler? RuntimePreviewEnded;

    private void SetEditorOverlaysVisible(bool visible)
    {
        if (this.FindControl<ViewportOverlay>("Overlay") is { } overlay) overlay.IsVisible = visible;
        if (this.FindControl<GizmoOverlay>("Gizmo") is { } gizmo) gizmo.IsVisible = visible;
    }

    private static GameEntity? FindCameraEntity(System.Collections.Generic.IEnumerable<GameEntity> entities)
    {
        foreach (var entity in entities)
        {
            if (entity.GetComponent<CameraComponent>() != null) return entity;
            var inChildren = FindCameraEntity(entity.Children);
            if (inChildren != null) return inChildren;
        }
        return null;
    }

    // --- Runtime 预览期间的逐帧捕获（Rec）---
    // PrimalPlayer 的录制在 player 进程内逐帧落盘；Render Surface 模式没有
    // player 进程，改由编辑器侧 DispatcherTimer 驱动：每 tick 以当前（场景）
    // 相机渲染一帧 → SaveRenderTargetAsPng。走 TryCaptureScreenshot 的
    // _renderInProgress 互锁，绝不与渲染 tick 重入。PNG 编码含 GPU WaitIdle，
    // 实际捕获率低于 30fps（与 editor.record 的"每帧真实耗时"口径一致）。

    private DispatcherTimer? _captureTimer;
    private string? _captureDir;
    private int _captureTotal;
    private int _captureIndex;

    /// <summary>预览帧捕获进行中（Run+Rec 的 Render Surface 路线）。</summary>
    public bool IsPreviewCapturing => _captureTimer != null;

    /// <summary>
    /// 开始对 runtime 预览逐帧捕获 PNG（frame_%04d.png）。要求已处于
    /// runtime 预览；返回 false 表示未能启动（参数/状态无效）。
    /// </summary>
    public bool StartPreviewFrameCapture(string directory, int frames)
    {
        if (!_runtimePreview)
        {
            Logger.Warning("[Run] 帧捕获只在 runtime 预览（Render Surface 模式）下可用");
            return false;
        }
        if (frames < 1)
        {
            Logger.Warning("[Run] 录制帧数必须 ≥ 1");
            return false;
        }
        StopPreviewFrameCapture();
        try { System.IO.Directory.CreateDirectory(directory); }
        catch (Exception ex)
        {
            Logger.Error($"[Run] 录制目录创建失败: {directory}", ex);
            return false;
        }
        _captureDir = directory;
        _captureTotal = frames;
        _captureIndex = 0;
        _captureTimer = new DispatcherTimer(DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromMilliseconds(33)
        };
        _captureTimer.Tick += OnPreviewCaptureTick;
        _captureTimer.Start();
        Logger.Info($"[Run] 预览帧捕获启动 → {directory} (frames={frames})");
        return true;
    }

    public void StopPreviewFrameCapture()
    {
        if (_captureTimer == null) return;
        _captureTimer.Stop();
        _captureTimer.Tick -= OnPreviewCaptureTick;
        _captureTimer = null;
        Logger.Info($"[Run] 预览帧捕获结束：captured={_captureIndex}/{_captureTotal} → {_captureDir}");
        _captureDir = null;
        _captureTotal = 0;
        _captureIndex = 0;
    }

    private void OnPreviewCaptureTick(object? sender, EventArgs e)
    {
        if (_captureDir == null || _captureIndex >= _captureTotal)
        {
            StopPreviewFrameCapture();
            return;
        }
        var path = System.IO.Path.Combine(_captureDir, $"frame_{_captureIndex:D4}.png");
        if (TryCaptureScreenshot(path, out _, out _))
            _captureIndex++;
        if (_captureIndex >= _captureTotal)
            StopPreviewFrameCapture();
    }

    /// <summary>Attach the scene whose editor camera this viewport edits.</summary>
    public void SetScene(Scene? scene)
    {
        if (ReferenceEquals(_scene, scene)) return;
        // 换场景时 runtime 预览的原生相机 id 已失效，先退回编辑器模式。
        if (_runtimePreview) EndRuntimePreview();
        SaveEditorCameraState();
        _scene = scene;
        _lookThroughSceneEntityId = 0;
        if (_scene != null)
        {
            var camera = _scene.EditorCamera;
            camera.UpgradeLegacyDefaultView();
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
        // Forward points from the camera to its orbit target.
        float[] forward = new[] { sy * cp, sp, cy * cp };
        float[] worldUp = new[] { 0f, 1f, 0f };
        float[] right = Cross(worldUp, forward);
        float rlen = MathF.Sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        if (rlen > 1e-6f) { right[0] /= rlen; right[1] /= rlen; right[2] /= rlen; }
        float[] screenUp = Cross(forward, right);

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
            float focal = WorldProjector.PerspectiveFocalLength(viewportHeight);
            ux = (screenX - viewportWidth * 0.5f) / focal;
            vy = -(screenY - viewportHeight * 0.5f) / focal; // invert Y
            depth = 1f;
        }

        float dx = forward[0] * depth + right[0] * ux + screenUp[0] * vy;
        float dy = forward[1] * depth + right[1] * ux + screenUp[1] * vy;
        float dz = forward[2] * depth + right[2] * ux + screenUp[2] * vy;
        return (camX, camY, camZ, dx, dy, dz);
    }

    private void OnAssetDragOver(object? sender, Avalonia.Input.DragEventArgs e)
    {
        var vm = ResolveWorldEditorViewModel();
        var effects = AssetDropHandler.EffectsFor(e, vm);
        e.DragEffects = effects;
        e.Handled = true;
        // DragOver fires for every pointer move during the drag, so log
        // at Debug level to avoid spamming the editor console. The user
        // can raise the level to see it.
        Logger.Debug($"EngineViewport3D.OnAssetDragOver: effects={effects} scene={vm?.ActiveScene?.Name ?? "<null>"}");
    }

    private async void OnAssetDrop(object? sender, Avalonia.Input.DragEventArgs e)
    {
        // Routed-event dispatch does not await async-void handlers. Mark the
        // event handled before the first await so it cannot bubble to the
        // WorldEditorView drop target and create the same entity twice.
        e.Handled = true;
        Logger.Info($"EngineViewport3D.OnAssetDrop: invoked");
        await AssetDropHandler.RunAsync(e, ResolveWorldEditorViewModel(), 0f, 0f, 0f);
    }

    /// <summary>
    /// Walk the visual tree to the MainWindow to find the active
    /// <see cref="WorldEditorViewModel"/>. The viewport sits inside the
    /// MainWindow's data-context chain but has no DataContext of its own;
    /// this matches the pattern used by <see cref="LightIconRenderer"/>
    /// and <see cref="ResyncActiveScene"/>.
    /// </summary>
    private WorldEditorViewModel? ResolveWorldEditorViewModel()
    {
        if (DataContext is WorldEditorViewModel inherited) return inherited;

        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        var main = lifetime?.MainWindow?.DataContext as MainWindowViewModel;
        return main?.WorldEditor;
    }
}
