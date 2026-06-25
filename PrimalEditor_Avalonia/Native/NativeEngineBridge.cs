using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native;

/// <summary>
/// P/Invoke bridge to the C++ PrimalEngine (EngineDLL).
/// Mirrors the EDITOR_INTERFACE functions in EngineDLL/EngineAPI.cpp and EntityAPI.cpp.
/// See: EngineDLL/EngineAPI.cpp and EngineDLL/EntityAPI.cpp
/// </summary>
public static class NativeEngineBridge
{
    private const string LibraryMacOS = "libPrimalEngine.dylib";
    private const string LibraryLinux = "libPrimalEngine.so";
    private const string LibraryWindows = "PrimalEngine.dll";

    private static bool _isLoaded;
    private static IntPtr _libraryHandle = IntPtr.Zero;

    public static bool IsLoaded => _isLoaded;
    public static string? LoadedPath { get; private set; }
    public static string? LoadError { get; private set; }

    /// <summary>
    /// Load the native engine library (libPrimalEngine.dylib / .so / .dll).
    /// The library must be in the search path or absolute path provided.
    /// </summary>
    public static bool LoadEngine(string? libraryPath = null)
    {
        if (_isLoaded)
        {
            Logger.Warning("Engine already loaded");
            return true;
        }

        try
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
            {
                NativeLibrary.SetDllImportResolver(typeof(NativeEngineBridge).Assembly, DllImportResolver);
                var resolvedPath = ResolveLibraryPath(libraryPath ?? LibraryMacOS);
                _libraryHandle = NativeLibrary.Load(resolvedPath);
                Logger.Debug($"Engine handle: {_libraryHandle}, path: {resolvedPath}");

                PrimeLibraryForPInvoke("PrimalEngine");
                PrimeLibraryForPInvoke("libPrimalEngine");
                PrimeLibraryForPInvoke("libContentTools");
            }
            else if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
            {
                var resolvedPath = ResolveLibraryPath(libraryPath ?? LibraryLinux);
                _libraryHandle = NativeLibrary.Load(resolvedPath);
                PrimeLibraryForPInvoke("PrimalEngine");
                PrimeLibraryForPInvoke("libPrimalEngine");
                PrimeLibraryForPInvoke("libContentTools");
            }
            else if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                _libraryHandle = NativeLibrary.Load(ResolveLibraryPath(libraryPath ?? LibraryWindows));
            }
            else
            {
                LoadError = "Unsupported OS platform";
                Logger.Error(LoadError);
                return false;
            }

            _isLoaded = true;
            LoadedPath = libraryPath;
            Logger.Info($"Native engine loaded: {libraryPath ?? "default"}");
            return true;
        }
        catch (Exception ex)
        {
            LoadError = ex.Message;
            Logger.Warning($"Native engine not available: {ex.Message}. Running in editor-only mode.");
            _isLoaded = false;
            return false;
        }
    }

    /// <summary>
    /// Try-load a library using multiple naming conventions. Used as a DllImport resolver fallback.
    /// </summary>
    private static IntPtr DllImportResolver(string libraryName, System.Reflection.Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (NativeLibrary.TryLoad(libraryName, assembly, searchPath, out var lib))
            return lib;

        var candidates = new[] {
            libraryName,
            "lib" + libraryName,
            libraryName.Replace("lib", "").Replace(".dylib", ""),
        };
        foreach (var name in candidates)
        {
            if (NativeLibrary.TryLoad(name, assembly, searchPath, out lib))
                return lib;
        }
        return IntPtr.Zero;
    }

    public static void UnloadEngine()
    {
        if (!_isLoaded) return;
        if (_libraryHandle != IntPtr.Zero)
        {
            NativeLibrary.Free(_libraryHandle);
            _libraryHandle = IntPtr.Zero;
        }
        _isLoaded = false;
        LoadedPath = null;
        Logger.Info("Native engine unloaded");
    }

    private static void PrimeLibraryForPInvoke(string name)
    {
        try
        {
            var path = ResolveLibraryPath(name);
            if (File.Exists(path) && NativeLibrary.TryLoad(path, out _))
            {
                Logger.Debug($"Primed library for P/Invoke: {path}");
            }
        }
        catch (Exception ex)
        {
            Logger.Debug($"PrimeLibraryForPInvoke({name}) failed: {ex.Message}");
        }
    }

    private static string ResolveLibraryPath(string libraryName)
    {
        if (Path.IsPathRooted(libraryName) && File.Exists(libraryName))
        {
            return libraryName;
        }

        var candidates = new List<string>();

        try
        {
            var mainModule = System.Diagnostics.Process.GetCurrentProcess().MainModule;
            if (mainModule != null && !string.IsNullOrEmpty(mainModule.FileName))
            {
                var exeDir = Path.GetDirectoryName(mainModule.FileName);
                if (!string.IsNullOrEmpty(exeDir))
                {
                    candidates.Add(Path.Combine(exeDir, libraryName));
                }
            }
        }
        catch { }

        try
        {
            var entry = System.Reflection.Assembly.GetEntryAssembly();
            if (entry != null && !string.IsNullOrEmpty(entry.Location))
            {
                var asmDir = Path.GetDirectoryName(entry.Location);
                if (!string.IsNullOrEmpty(asmDir))
                {
                    candidates.Add(Path.Combine(asmDir, libraryName));
                }
            }
        }
        catch { }

        var cwd = Directory.GetCurrentDirectory();
        candidates.Add(Path.Combine(cwd, libraryName));

        var baseDir = AppContext.BaseDirectory;
        candidates.Add(Path.Combine(baseDir, libraryName));

        var probeRoots = new[] { baseDir, cwd };
        foreach (var root in probeRoots)
        {
            try
            {
                var dir = new DirectoryInfo(root);
                for (var i = 0; i < 6 && dir != null; i++, dir = dir.Parent)
                {
                    var buildOutput = Path.Combine(dir.FullName, "Darwin", "DebugEditor", libraryName);
                    candidates.Add(buildOutput);

                    var directBuild = Path.Combine(dir.FullName, "build", "Darwin", "DebugEditor", libraryName);
                    candidates.Add(directBuild);
                }
            }
            catch { }
        }

        var envLibPath = Environment.GetEnvironmentVariable(
            RuntimeInformation.IsOSPlatform(OSPlatform.OSX) ? "DYLD_LIBRARY_PATH" :
            RuntimeInformation.IsOSPlatform(OSPlatform.Linux) ? "LD_LIBRARY_PATH" : "PATH");
        if (!string.IsNullOrEmpty(envLibPath))
        {
            foreach (var p in envLibPath.Split(Path.PathSeparator))
            {
                if (!string.IsNullOrEmpty(p))
                {
                    candidates.Add(Path.Combine(p, libraryName));
                }
            }
        }

        foreach (var candidate in candidates)
        {
            if (File.Exists(candidate))
            {
                Logger.Debug($"Found native library at: {candidate}");
                return candidate;
            }
        }

        return libraryName;
    }

    // ====== Engine API (EngineAPI.cpp) ======

    [DllImport("PrimalEngine", EntryPoint = "LoadGameCodeDll", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _LoadGameCodeDll(IntPtr dllPath);

    [DllImport("PrimalEngine", EntryPoint = "UnloadGameCodeDll", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _UnloadGameCodeDll(IntPtr dllPath);

    [DllImport("PrimalEngine", EntryPoint = "CreateRenderSurface", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CreateRenderSurface(IntPtr host, int width, int height);

    [DllImport("PrimalEngine", EntryPoint = "CreateEmbeddedRenderSurface", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CreateEmbeddedRenderSurface(IntPtr hostView, int width, int height);

    [DllImport("PrimalEngine", EntryPoint = "RemoveRenderSurface", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _RemoveRenderSurface(uint id);

    [DllImport("PrimalEngine", EntryPoint = "GetWindowHandle", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr _GetWindowHandle(uint id);

    [DllImport("PrimalEngine", EntryPoint = "GetSurfaceView", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr _GetSurfaceView(uint id);

    [DllImport("PrimalEngine", EntryPoint = "GetAvaloniaContentView", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr _GetAvaloniaContentView();

    [DllImport("PrimalEngine", EntryPoint = "SetEmbeddedSurfaceFrame", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetEmbeddedSurfaceFrame(uint id, int x, int y, uint w, uint h);

    [DllImport("PrimalEngine", EntryPoint = "ResizeRenderSurface", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ResizeRenderSurface(uint id, uint width, uint height);

    // ====== RHI Engine Lifecycle (EngineAPI.cpp InitializeEngine/ShutdownEngine) ======

    [DllImport("PrimalEngine", EntryPoint = "InitializeEngine", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _InitializeEngine(uint rhiPlatform, uint enableDebug);

    [DllImport("PrimalEngine", EntryPoint = "ShutdownEngine", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ShutdownEngine();

    [DllImport("PrimalEngine", EntryPoint = "IsEngineInitialized", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _IsEngineInitialized();

    [DllImport("PrimalEngine", EntryPoint = "GetEngineDeviceHandle", CallingConvention = CallingConvention.Cdecl)]
    private static extern UInt64 _GetEngineDeviceHandle();

    // ====== Frame Driver (FrameAPI.cpp RenderFrame) ======

    [StructLayout(LayoutKind.Sequential)]
    public struct RenderFrameParams
    {
        public uint SurfaceId;
        public uint CameraId;
        public UInt64 LightSetKey;
        public uint RenderItemCount;
        public IntPtr RenderItemIds;   // const u32* (id_eng::id_type = u32) - array of render_item ids
        public IntPtr Thresholds;       // const f32* (nullable)
        public float AverageFrameTime;
        public float LastFrameTime;
    }

    [DllImport("PrimalEngine", EntryPoint = "RenderFrame", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _RenderFrame(IntPtr paramsPtr);

    // ====== Scene Setup (ResourceAPI: CreateCamera/Entity/RenderItem) ======

[DllImport("PrimalEngine", EntryPoint = "CreateEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CreateEntity(float pos_x, float pos_y, float pos_z);

    public static uint CreateEntity(float x, float y, float z)
    {
        if (!_isLoaded) return 0;
        try { return _CreateEntity(x, y, z); }
        catch (Exception ex) { Logger.Error("CreateEntity failed", ex); return 0; }
    }

    [DllImport("PrimalEngine", EntryPoint = "CreateCamera", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CreateCamera(uint entity_id, float fov, float aspect, float near_z, float far_z);

    public static uint CreateCamera(uint entityId, float fov, float aspect, float nearZ, float farZ)
    {
        if (!_isLoaded) return 0;
        try { return _CreateCamera(entityId, fov, aspect, nearZ, farZ); }
        catch (Exception ex) { Logger.Error("CreateCamera failed", ex); return 0; }
    }

    [DllImport("PrimalEngine", EntryPoint = "SetEntityTransform", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetEntityTransform(uint entityId, float[] position, float[] rotation, float[] scale);

    public static void SetEntityTransform(uint entityId, float[] position, float[] rotation, float[] scale)
    {
        if (!_isLoaded) return;
        try
        {
            _SetEntityTransform(entityId, position, rotation, scale);
        }
        catch (Exception ex) { Logger.Error("SetEntityTransform failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "CreateLightSet", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _CreateLightSet(UInt64 key);

    [DllImport("PrimalEngine", EntryPoint = "AddRenderItem", CallingConvention = CallingConvention.Cdecl)]
    private static extern UInt64 _AddRenderItem(uint entityId, UInt64 geometryContentId, uint materialCount, UInt64[] materialIds);

    public static UInt64 AddRenderItem(uint entityId, UInt64 geometryContentId, uint materialCount, UInt64[] materialIds)
    {
        if (!_isLoaded) return 0;
        try { return _AddRenderItem(entityId, geometryContentId, materialCount, materialIds); }
        catch (Exception ex) { Logger.Error("AddRenderItem failed", ex); return 0; }
    }

    // Lazy-compiles the editor sphere MSL (kEditorSphereVertexShader +
    // kEditorSpherePixelShader) into a metallib, registers the resulting
    // shader group, and constructs an opauqe material that references it.
    // Subsequent calls return the cached material id; the compile +
    // xcrun invocation only happens on the first call.
    [DllImport("PrimalEngine", EntryPoint = "CreateDefaultMaterial", CallingConvention = CallingConvention.Cdecl)]
    private static extern UInt64 _CreateDefaultMaterial();

    public static UInt64 CreateDefaultMaterial()
    {
        if (!_isLoaded) return InvalidContentId;
        try { return _CreateDefaultMaterial(); }
        catch (Exception ex) { Logger.Error("CreateDefaultMaterial failed", ex); return InvalidContentId; }
    }

    [DllImport("PrimalEngine", EntryPoint = "GetEntityTransform", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _GetEntityTransform(UInt32 entityId,
        [Out] float[] position, [Out] float[] rotation, [Out] float[] scale);

    public static (float x, float y, float z) GetEntityPosition(uint entityId)
    {
        if (!_isLoaded) return (0, 0, 0);
        try
        {
            float[] pos = new float[3], rot = new float[4], scl = new float[3];
            _GetEntityTransform(entityId, pos, rot, scl);
            return (pos[0], pos[1], pos[2]);
        }
        catch (Exception ex)
        {
            Logger.Error($"GetEntityPosition({entityId:X}) failed", ex);
            return (0, 0, 0);
        }
    }

    // Create a translated copy of an RHI-registered mesh, register it as a
    // legacy submesh + geometry, and return the legacy geometry_content_id
    // (use with AddRenderItem). Workaround for per-entity World matrix
    // propagation being broken on this system.
    [DllImport("PrimalEngine", EntryPoint = "CreateTranslatedProceduralMesh", CallingConvention = CallingConvention.Cdecl)]
    private static extern UInt64 _CreateTranslatedProceduralMesh(UInt64 rhiGeometryContentId, float offsetX, float offsetY, float offsetZ);

    public static UInt64 CreateTranslatedProceduralMesh(UInt64 rhiGeometryContentId, float offsetX, float offsetY, float offsetZ)
    {
        if (!_isLoaded) return InvalidContentId;
        try { return _CreateTranslatedProceduralMesh(rhiGeometryContentId, offsetX, offsetY, offsetZ); }
        catch (Exception ex) { Logger.Error("CreateTranslatedProceduralMesh failed", ex); return InvalidContentId; }
    }

    // Closes the geometry-vs-submesh API gap: ProceduralMeshCreate registers
    // the geometry with content::register_mesh_asset but not the submesh
    // entry in MetalContent::submesh_views / submesh_buffers that the
    // legacy AddRenderItem path requires. The implementation reconstructs
    // the legacy submesh::add binary blob from the already-registered
    // RHIMeshAsset and calls the internal submesh::add. Returns the
    // submesh id (or InvalidContentId on failure).
    [DllImport("PrimalEngine", EntryPoint = "RegisterProceduralMeshSubmesh", CallingConvention = CallingConvention.Cdecl)]
    private static extern UInt64 _RegisterProceduralMeshSubmesh(UInt64 geometryContentId);

    public static UInt64 RegisterProceduralMeshSubmesh(UInt64 geometryContentId)
    {
        if (!_isLoaded) return InvalidContentId;
        try { return _RegisterProceduralMeshSubmesh(geometryContentId); }
        catch (Exception ex) { Logger.Error("RegisterProceduralMeshSubmesh failed", ex); return InvalidContentId; }
    }

    [DllImport("PrimalEngine", EntryPoint = "RemoveRenderItem", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _RemoveRenderItem(UInt64 renderItemId);
    public static void RemoveRenderItem(UInt64 renderItemId)
    {
        if (!_isLoaded) return;
        try { _RemoveRenderItem(renderItemId); } catch (Exception ex) { Logger.Error("RemoveRenderItem failed", ex); }
    }

    // ====== Procedural Mesh (ProceduralMeshAPI.cpp) ======

    [DllImport("PrimalEngine", EntryPoint = "ProceduralMeshCreate", CallingConvention = CallingConvention.Cdecl)]
    private static extern UInt64 _ProceduralMeshCreate(uint meshType, float[] parameters, uint parameterCount);

    public static UInt64 ProceduralMeshCreate(uint meshType, float[] parameters)
    {
        if (!_isLoaded || parameters == null) return InvalidContentId;
        try { return _ProceduralMeshCreate(meshType, parameters, (uint)parameters.Length); }
        catch (Exception ex) { Logger.Error("ProceduralMeshCreate failed", ex); return InvalidContentId; }
    }

    // Native id::invalid_id is u32(-1) = 0xFFFFFFFF (see Engine/Common/Id.h).
    // Cast to u64 it becomes 0x00000000FFFFFFFF — NOT UInt64.MaxValue. Native
    // APIs (ProceduralMeshCreate, AddRenderItem, CreateEntity, CreateCamera,
    // CreateRenderSurface, etc.) all return this exact value on failure.
    //
    // IMPORTANT: 0 is a LEGAL handle value — every free_list in the engine
    // (rhi_mesh_assets, geometry_hierarchies, render_item_ids) hands out id 0
    // to its first allocation. Earlier we tried to be defensive and reject 0
    // too, but that broke the editor: ProceduralMeshCreate(Sphere) returned
    // the legitimate slot-0 id and our guard treated it as invalid, blanking
    // the viewport. Only the invalid_id sentinel marks failure.
    public const UInt64 InvalidContentId = 0x00000000FFFFFFFFUL;
    public const UInt32 InvalidEntityId = 0xFFFFFFFFu;

    // True iff the handle is not the native failure sentinel. 0 is valid.
    public static bool IsValidHandle(UInt64 handle) => handle != InvalidContentId;
    public static bool IsValidEntity(UInt32 entityId) => entityId != InvalidEntityId;

    [DllImport("PrimalEngine", EntryPoint = "ProceduralMeshDestroy", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ProceduralMeshDestroy(UInt64 contentId);
    public static void ProceduralMeshDestroy(UInt64 contentId)
    {
        if (!_isLoaded) return;
        try { _ProceduralMeshDestroy(contentId); } catch (Exception ex) { Logger.Error("ProceduralMeshDestroy failed", ex); }
    }

    // ====== Light (LightAPI.cpp) ======

    // light_type: 0=point, 1=spot (enum graphics::light::type)
    [DllImport("PrimalEngine", EntryPoint = "AddEntityLight", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _AddEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, uint isEnabled);
    public static uint AddEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled)
    {
        if (!_isLoaded || colorRgb == null) return 0;
        try { return _AddEntityLight(entityId, lightType, intensity, colorRgb, range, umbraRad, penumbraRad, isEnabled ? 1u : 0u); }
        catch (Exception ex) { Logger.Error("AddEntityLight failed", ex); return 0; }
    }

    [DllImport("PrimalEngine", EntryPoint = "RemoveEntityLight", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _RemoveEntityLight(uint entityId);
    public static void RemoveEntityLight(uint entityId)
    {
        if (!_isLoaded) return;
        try { _RemoveEntityLight(entityId); } catch (Exception ex) { Logger.Error("RemoveEntityLight failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "CreateEmbeddedRenderView", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr _CreateEmbeddedRenderView(IntPtr parent, int width, int height);

    [DllImport("PrimalEngine", EntryPoint = "DestroyEmbeddedRenderView", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DestroyEmbeddedRenderView(IntPtr view);

    [DllImport("PrimalEngine", EntryPoint = "ResizeEmbeddedRenderView", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ResizeEmbeddedRenderView(IntPtr view, int width, int height);

    // ====== Editor Render Bridge (EngineAPI_mac.mm) =====
    [DllImport("PrimalEngine", EntryPoint = "InitializeEditorRenderer", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr _InitializeEditorRenderer(IntPtr host_view, double screen_x, double screen_y, double screen_w, double screen_h);

    [DllImport("PrimalEngine", EntryPoint = "MoveEditorRenderer", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _MoveEditorRenderer(double screen_x, double screen_y, double screen_w, double screen_h);

    [DllImport("PrimalEngine", EntryPoint = "RunEditorFrame", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _RunEditorFrame(float delta_time);

    [DllImport("PrimalEngine", EntryPoint = "ResizeEditorSurface", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ResizeEditorSurface(int width, int height);

    [DllImport("PrimalEngine", EntryPoint = "ShutdownEditorRenderer", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ShutdownEditorRenderer();

    [DllImport("PrimalEngine", EntryPoint = "GetEditorWindowHandle", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr _GetEditorWindowHandle();

    public static IntPtr GetEditorWindowHandle()
    {
        if (!_isLoaded) return IntPtr.Zero;
        try { return _GetEditorWindowHandle(); }
        catch (Exception ex) { Logger.Error("GetEditorWindowHandle failed", ex); return IntPtr.Zero; }
    }

    public static IntPtr InitializeEditorRenderer(IntPtr hostView, double screenX, double screenY, double screenW, double screenH)
    {
        if (!_isLoaded) return IntPtr.Zero;
        try { return _InitializeEditorRenderer(hostView, screenX, screenY, screenW, screenH); }
        catch (Exception ex) { Logger.Error("InitializeEditorRenderer failed", ex); return IntPtr.Zero; }
    }

    public static void MoveEditorRenderer(double screenX, double screenY, double screenW, double screenH)
    {
        if (!_isLoaded) return;
        try { _MoveEditorRenderer(screenX, screenY, screenW, screenH); }
        catch (Exception ex) { Logger.Error("MoveEditorRenderer failed", ex); }
    }

    public static void RunEditorFrame(float deltaTime)
    {
        if (!_isLoaded) return;
        try { _RunEditorFrame(deltaTime); }
        catch (Exception ex) { Logger.Error("RunEditorFrame failed", ex); }
    }

    public static void ResizeEditorSurface(int width, int height)
    {
        if (!_isLoaded) return;
        try { _ResizeEditorSurface(width, height); }
        catch (Exception ex) { Logger.Error("ResizeEditorSurface failed", ex); }
    }

    public static void ShutdownEditorRenderer()
    {
        if (!_isLoaded) return;
        try { _ShutdownEditorRenderer(); }
        catch (Exception ex) { Logger.Error("ShutdownEditorRenderer failed", ex); }
    }

    public static uint LoadGameCodeDll(string dllPath)
    {
        if (!_isLoaded) { Logger.Warning("Engine not loaded"); return 0; }
        try
        {
            var p = MarshalUtf8(dllPath);
            return _LoadGameCodeDll(p);
        }
        catch (Exception ex)
        {
            Logger.Error("LoadGameCodeDll failed", ex);
            return 0;
        }
    }

    public static uint UnloadGameCodeDll(string dllPath)
    {
        if (!_isLoaded) return 0;
        try
        {
            var p = MarshalUtf8(dllPath);
            return _UnloadGameCodeDll(p);
        }
        catch (Exception ex)
        {
            Logger.Error("UnloadGameCodeDll failed", ex);
            return 0;
        }
    }

    public static uint CreateRenderSurface(IntPtr hostWindow, int width, int height)
    {
        if (!_isLoaded) { Logger.Warning("Engine not loaded"); return uint.MaxValue; }
        try
        {
            var result = _CreateRenderSurface(hostWindow, width, height);
            if (result == uint.MaxValue)
            {
                Logger.Warning("CreateRenderSurface: engine returned -1 (window creation failed)");
            }
            return result;
        }
        catch (Exception ex)
        {
            Logger.Error("CreateRenderSurface failed", ex);
            return uint.MaxValue;
        }
    }

    // Embeds the engine's forward pipeline into a host NSView (e.g. Avalonia
    // contentView) — the MTKView is attached as a subview so render output
    // appears inside the host's content area rather than in a separate window.
    public static uint CreateEmbeddedRenderSurface(IntPtr hostView, int width, int height)
    {
        if (!_isLoaded) { Logger.Warning("Engine not loaded"); return uint.MaxValue; }
        try
        {
            var result = _CreateEmbeddedRenderSurface(hostView, width, height);
            if (result == uint.MaxValue)
            {
                Logger.Warning("CreateEmbeddedRenderSurface: engine returned -1");
            }
            return result;
        }
        catch (Exception ex)
        {
            Logger.Error("CreateEmbeddedRenderSurface failed", ex);
            return uint.MaxValue;
        }
    }

    public static void RemoveRenderSurface(uint id)
    {
        if (!_isLoaded) return;
        try { _RemoveRenderSurface(id); }
        catch (Exception ex) { Logger.Error("RemoveRenderSurface failed", ex); }
    }

    public static IntPtr GetWindowHandle(uint id)
    {
        if (!_isLoaded) return IntPtr.Zero;
        try { return _GetWindowHandle(id); }
        catch (Exception ex) { Logger.Error("GetWindowHandle failed", ex); return IntPtr.Zero; }
    }

    public static IntPtr GetSurfaceView(uint id)
    {
        if (!_isLoaded) return IntPtr.Zero;
        try { return _GetSurfaceView(id); }
        catch (Exception ex) { Logger.Error("GetSurfaceView failed", ex); return IntPtr.Zero; }
    }

    public static IntPtr GetAvaloniaContentView()
    {
        if (!_isLoaded) return IntPtr.Zero;
        try { return _GetAvaloniaContentView(); }
        catch (Exception ex) { Logger.Error("GetAvaloniaContentView failed", ex); return IntPtr.Zero; }
    }

    public static void SetEmbeddedSurfaceFrame(uint id, int x, int y, uint w, uint h)
    {
        if (!_isLoaded) return;
        try { _SetEmbeddedSurfaceFrame(id, x, y, w, h); }
        catch (Exception ex) { Logger.Error("SetEmbeddedSurfaceFrame failed", ex); }
    }

    public static void ResizeRenderSurface(uint id, uint width = 0, uint height = 0)
    {
        if (!_isLoaded) return;
        try { _ResizeRenderSurface(id, width, height); }
        catch (Exception ex) { Logger.Error("ResizeRenderSurface failed", ex); }
    }

    // RHIPlatform values must match EngineAPI.cpp InitializeEngine.
    // Metal=3, Vulkan=2, D3D12=1 (see RHITypes.h RHIPlatform enum).
    public const uint RHIPlatformMetal = 3;

    public static bool InitializeEngine(uint rhiPlatform = RHIPlatformMetal, bool enableDebug = false)
    {
        if (!_isLoaded) return false;
        try
        {
            var result = _InitializeEngine(rhiPlatform, enableDebug ? 1u : 0u);
            return result != 0;
        }
        catch (Exception ex)
        {
            Logger.Error("InitializeEngine failed", ex);
            return false;
        }
    }

    public static void ShutdownEngine()
    {
        if (!_isLoaded) return;
        try { _ShutdownEngine(); }
        catch (Exception ex) { Logger.Error("ShutdownEngine failed", ex); }
    }

    public static bool IsEngineInitialized()
    {
        if (!_isLoaded) return false;
        try { return _IsEngineInitialized() != 0; }
        catch { return false; }
    }

    public static UInt64 GetEngineDeviceHandle()
    {
        if (!_isLoaded) return 0;
        try { return _GetEngineDeviceHandle(); }
        catch { return 0; }
    }

    public static bool RenderFrame(RenderFrameParams p)
    {
        if (!_isLoaded) return false;
        try
        {
            var size = Marshal.SizeOf<RenderFrameParams>();
            var ptr = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(p, ptr, false);
                return _RenderFrame(ptr) != 0;
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
        }
        catch (Exception ex)
        {
            Logger.Error("RenderFrame failed", ex);
            return false;
        }
    }

    public static IntPtr CreateEmbeddedRenderView(IntPtr parent, int width, int height)
    {
        if (!_isLoaded) { Logger.Warning("Engine not loaded"); return IntPtr.Zero; }
        try
        {
            var view = _CreateEmbeddedRenderView(parent, width, height);
            if (view == IntPtr.Zero)
            {
                Logger.Warning("CreateEmbeddedRenderView returned null");
            }
            return view;
        }
        catch (Exception ex)
        {
            Logger.Error("CreateEmbeddedRenderView failed", ex);
            return IntPtr.Zero;
        }
    }

    public static void DestroyEmbeddedRenderView(IntPtr view)
    {
        if (!_isLoaded || view == IntPtr.Zero) return;
        try { _DestroyEmbeddedRenderView(view); }
        catch (Exception ex) { Logger.Error("DestroyEmbeddedRenderView failed", ex); }
    }

    public static void ResizeEmbeddedRenderView(IntPtr view, int width, int height)
    {
        if (!_isLoaded || view == IntPtr.Zero) return;
        try { _ResizeEmbeddedRenderView(view, width, height); }
        catch (Exception ex) { Logger.Error("ResizeEmbeddedRenderView failed", ex); }
    }

    // ====== Entity API (EntityAPI.cpp) ======

    [StructLayout(LayoutKind.Sequential)]
    public struct TransformComponent
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
        public float[] Position;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
        public float[] Rotation;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
        public float[] Scale;

        public TransformComponent()
        {
            Position = new float[3];
            Rotation = new float[3];
            Scale = new float[3] { 1, 1, 1 };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct GameEntityDescriptor
    {
        public TransformComponent Transform;
    }

    [DllImport("PrimalEngine", EntryPoint = "CreateGameEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong _CreateGameEntity(ref GameEntityDescriptor desc);

    [DllImport("PrimalEngine", EntryPoint = "RemoveGameEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _RemoveGameEntity(ulong id);

    public static ulong CreateGameEntity(float px, float py, float pz, float rx, float ry, float rz, float sx, float sy, float sz)
    {
        if (!_isLoaded) return 0;
        try
        {
            var desc = new GameEntityDescriptor
            {
                Transform = new TransformComponent
                {
                    Position = new[] { px, py, pz },
                    Rotation = new[] { rx, ry, rz },
                    Scale = new[] { sx, sy, sz },
                }
            };
            return _CreateGameEntity(ref desc);
        }
        catch (Exception ex)
        {
            Logger.Error("CreateGameEntity failed", ex);
            return 0;
        }
    }

    public static void RemoveGameEntity(ulong id)
    {
        if (!_isLoaded) return;
        try { _RemoveGameEntity(id); }
        catch (Exception ex) { Logger.Error("RemoveGameEntity failed", ex); }
    }

    // ====== Asset API (AssetAPI.cpp) ======

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct TextureImportResult
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)]
        public string Path;
        public ulong Handle;
        public uint Width;
        public uint Height;
        public uint Success;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string ErrorMessage;
    }

    [DllImport("PrimalEngine", EntryPoint = "InitializeAssetSystem", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _InitializeAssetSystem();

    [DllImport("PrimalEngine", EntryPoint = "ShutdownAssetSystem", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ShutdownAssetSystem();

    [DllImport("PrimalEngine", EntryPoint = "ImportTexture", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern uint _ImportTexture(IntPtr path, out TextureImportResult result);

    [DllImport("PrimalEngine", EntryPoint = "ImportMeshAsync", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern ulong _ImportMeshAsync(IntPtr path);

    [DllImport("libContentTools", EntryPoint = "CookMesh", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern uint _CookMesh(IntPtr input, IntPtr output);

    [DllImport("PrimalEngine", EntryPoint = "TickAssetSystem", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _TickAssetSystem();

    [DllImport("PrimalEngine", EntryPoint = "GetPendingUploadCount", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GetPendingUploadCount();

    public static bool InitializeAssetSystem()
    {
        if (!_isLoaded) return false;
        try { return _InitializeAssetSystem() != 0; }
        catch (Exception ex) { Logger.Error("InitializeAssetSystem failed", ex); return false; }
    }

    public static void ShutdownAssetSystem()
    {
        if (!_isLoaded) return;
        try { _ShutdownAssetSystem(); }
        catch (Exception ex) { Logger.Error("ShutdownAssetSystem failed", ex); }
    }

    public static TextureImportResult? ImportTexture(string path)
    {
        if (!_isLoaded) return null;
        try
        {
            var p = MarshalUtf8(path);
            var result = new TextureImportResult();
            _ImportTexture(p, out result);
            return result;
        }
        catch (Exception ex) { Logger.Error("ImportTexture failed", ex); return null; }
    }

    public static ulong ImportMesh(string path)
    {
        if (!_isLoaded) return 0;
        try
        {
            var p = MarshalUtf8(path);
            return _ImportMeshAsync(p);
        }
        catch (Exception ex) { Logger.Error("ImportMesh failed", ex); return 0; }
    }

    public static bool CookMesh(string input, string output)
    {
        if (!_isLoaded) return false;
        try
        {
            var i = MarshalUtf8(input);
            var o = MarshalUtf8(output);
            return _CookMesh(i, o) != 0;
        }
        catch (Exception ex) { Logger.Error("CookMesh failed", ex); return false; }
    }

    public static void TickAssetSystem()
    {
        if (!_isLoaded) return;
        try { _TickAssetSystem(); }
        catch (Exception ex) { Logger.Error("TickAssetSystem failed", ex); }
    }

    public static uint GetPendingUploadCount()
    {
        if (!_isLoaded) return 0;
        try { return _GetPendingUploadCount(); }
        catch { return 0; }
    }

    // ====== Helpers ======

    private static IntPtr MarshalUtf8(string s)
    {
        if (s == null) return IntPtr.Zero;
        var bytes = System.Text.Encoding.UTF8.GetBytes(s + '\0');
        var ptr = Marshal.AllocHGlobal(bytes.Length);
        Marshal.Copy(bytes, 0, ptr, bytes.Length);
        return ptr;
    }
}