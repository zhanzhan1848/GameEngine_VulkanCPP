using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Editors.Geometry;

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

    static NativeEngineBridge()
    {
        // Fail fast on ABI drift. If the C# POD mirror doesn't match the
        // C++ struct's size, every CookMeshWithPipeline call would silently
        // mis-parse the parameters — surfaces as garbage output that takes
        // an afternoon to diagnose. Better to crash here.
        AIAssetPipelineSettings.AssertSize();
    }

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

    // ====== Render Pipeline (RenderPipelineAPI.cpp) ======
    //
    // Mirrors EngineDLL/RenderPipelineAPI.cpp. Exposes the
    // StandardRenderPipeline abstraction: pass toggles, editor mode,
    // Lumen GI quality preset, mesh / streaming-mesh entity registration,
    // shader hot-reload, viewport size, and the parameter descriptors
    // used by the editor UI to auto-generate controls.
    //
    // Settings struct (RenderPipelineSettings = 380 bytes on macOS) is
    // intentionally NOT mirrored as a managed struct — we marshal it as
    // a raw byte buffer via IntPtr. The full struct layout in
    // Engine/Graphics/RenderPipeline/PipelineQualityConfig.h and friends
    // drifts over time; keeping it opaque avoids binary-compat breakage.

    public enum RenderPassId : uint
    {
        Shadow = 0,
        DeferredLighting = 1,
        SSAO = 2,
        DDGI = 3,
        SurfaceCache = 4,
        SCDDGIIntegration = 5,
        SSGI = 6,
        ScreenProbes = 7,
        GIGather = 8,
        VolumePass = 9,
        VolumeRenderer = 10,
        FroxelFog = 11,
        FluidRender = 12,
        FusionComposite = 13,
        FinalBlit = 14,
        Count = 15,
    }

    public enum LumenQualityPresetId : uint
    {
        Off = 0,
        Low = 1,
        Medium = 2,
        High = 3,
        Ultra = 4,
        UltraRT = 5,
    }

    // Pass toggles
    [DllImport("PrimalEngine", EntryPoint = "SetRenderPassEnabled", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetRenderPassEnabled(uint passId, uint enabled);
    public static void SetRenderPassEnabled(RenderPassId passId, bool enabled)
    {
        if (!_isLoaded) return;
        try { _SetRenderPassEnabled((uint)passId, enabled ? 1u : 0u); }
        catch (Exception ex) { Logger.Error("SetRenderPassEnabled failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "IsRenderPassEnabled", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _IsRenderPassEnabled(uint passId);
    public static bool IsRenderPassEnabled(RenderPassId passId)
    {
        if (!_isLoaded) return false;
        try { return _IsRenderPassEnabled((uint)passId) != 0; }
        catch (Exception ex) { Logger.Error("IsRenderPassEnabled failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "IsRenderPassActive", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _IsRenderPassActive(uint passId);
    public static bool IsRenderPassActive(RenderPassId passId)
    {
        if (!_isLoaded) return false;
        try { return _IsRenderPassActive((uint)passId) != 0; }
        catch (Exception ex) { Logger.Error("IsRenderPassActive failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "GetRenderPassCount", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GetRenderPassCount();
    public static uint GetRenderPassCount()
    {
        if (!_isLoaded) return 0;
        try { return _GetRenderPassCount(); }
        catch (Exception ex) { Logger.Error("GetRenderPassCount failed", ex); return 0; }
    }

    // Pass info — three out-pointers (name, description, default_enabled).
    [DllImport("PrimalEngine", EntryPoint = "GetPipelinePassInfo", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _GetPipelinePassInfo(uint passId, out IntPtr name, out IntPtr description, out uint defaultEnabled);
    public static (string name, string description, bool defaultEnabled) GetPipelinePassInfo(RenderPassId passId)
    {
        if (!_isLoaded) return (string.Empty, string.Empty, false);
        try
        {
            _GetPipelinePassInfo((uint)passId, out var namePtr, out var descPtr, out var defEnabled);
            return (
                Marshal.PtrToStringAnsi(namePtr) ?? string.Empty,
                Marshal.PtrToStringAnsi(descPtr) ?? string.Empty,
                defEnabled != 0);
        }
        catch (Exception ex) { Logger.Error("GetPipelinePassInfo failed", ex); return (string.Empty, string.Empty, false); }
    }

    // Render pipeline settings — opaque byte buffer; size known at runtime
    // via sizeof(RenderPipelineSettings) on the C++ side. The editor does
    // not currently need to round-trip the full struct, so the bytes
    // marshaling path is provided but not used.
    public static uint GetRenderPipelineSettingsSize()
    {
        // sizeof(RenderPipelineSettings) on macOS is 380 bytes; if that
        // changes upstream, recompile this constant. The native side
        // trusts the caller to pass a buffer of the right size.
        return 380;
    }

    [DllImport("PrimalEngine", EntryPoint = "GetRenderPipelineSettings", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _GetRenderPipelineSettings(IntPtr outBuffer);
    public static byte[] GetRenderPipelineSettings()
    {
        if (!_isLoaded) return null;
        try
        {
            var buf = new byte[GetRenderPipelineSettingsSize()];
            var handle = GCHandle.Alloc(buf, GCHandleType.Pinned);
            try
            {
                _GetRenderPipelineSettings(handle.AddrOfPinnedObject());
            }
            finally { handle.Free(); }
            return buf;
        }
        catch (Exception ex) { Logger.Error("GetRenderPipelineSettings failed", ex); return null; }
    }

    [DllImport("PrimalEngine", EntryPoint = "UpdateRenderPipelineSettings", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _UpdateRenderPipelineSettings(IntPtr settings);
    public static void UpdateRenderPipelineSettings(byte[] settings)
    {
        if (!_isLoaded || settings == null) return;
        try
        {
            var handle = GCHandle.Alloc(settings, GCHandleType.Pinned);
            try { _UpdateRenderPipelineSettings(handle.AddrOfPinnedObject()); }
            finally { handle.Free(); }
        }
        catch (Exception ex) { Logger.Error("UpdateRenderPipelineSettings failed", ex); }
    }

    // Param descriptors for UI auto-generation.
    [DllImport("PrimalEngine", EntryPoint = "GetPipelineParamDescriptors", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr _GetPipelineParamDescriptors(out uint outCount);
    public static uint GetPipelineParamDescriptorsCount()
    {
        if (!_isLoaded) return 0;
        try { _GetPipelineParamDescriptors(out var count); return count; }
        catch (Exception ex) { Logger.Error("GetPipelineParamDescriptors (count) failed", ex); return 0; }
    }

    // Pipeline viewport size — must match the surface size for the
    // StandardRenderPipeline to scale its internal buffers correctly.
    [DllImport("PrimalEngine", EntryPoint = "SetPipelineViewportSize", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetPipelineViewportSize(uint width, uint height);
    public static void SetPipelineViewportSize(int width, int height)
    {
        if (!_isLoaded) return;
        try { _SetPipelineViewportSize((uint)width, (uint)height); }
        catch (Exception ex) { Logger.Error("SetPipelineViewportSize failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "ReloadPipelineShader", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _ReloadPipelineShader(UInt64 shaderHandle, IntPtr data, uint dataSize);
    public static bool ReloadPipelineShader(UInt64 shaderHandle, byte[] data)
    {
        if (!_isLoaded || data == null || data.Length == 0) return false;
        try
        {
            var handle = GCHandle.Alloc(data, GCHandleType.Pinned);
            try { return _ReloadPipelineShader(shaderHandle, handle.AddrOfPinnedObject(), (uint)data.Length) != 0; }
            finally { handle.Free(); }
        }
        catch (Exception ex) { Logger.Error("ReloadPipelineShader failed", ex); return false; }
    }

    // Mesh entity registration. texture_content_ids is u64 array of
    // content::add_texture handles; pass null/0 for untextured mesh.
    [DllImport("PrimalEngine", EntryPoint = "PipelineRegisterMeshEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern UInt64 _PipelineRegisterMeshEntity(UInt64 geometryContentId, IntPtr textureContentIds, uint textureCount);
    public static UInt64 PipelineRegisterMeshEntity(UInt64 geometryContentId, UInt64[] textureContentIds)
    {
        if (!_isLoaded) return 0;
        try
        {
            if (textureContentIds == null || textureContentIds.Length == 0)
                return _PipelineRegisterMeshEntity(geometryContentId, IntPtr.Zero, 0);
            var handle = GCHandle.Alloc(textureContentIds, GCHandleType.Pinned);
            try { return _PipelineRegisterMeshEntity(geometryContentId, handle.AddrOfPinnedObject(), (uint)textureContentIds.Length); }
            finally { handle.Free(); }
        }
        catch (Exception ex) { Logger.Error("PipelineRegisterMeshEntity failed", ex); return 0; }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineUnregisterMeshEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineUnregisterMeshEntity(UInt64 entityId);
    public static void PipelineUnregisterMeshEntity(UInt64 entityId)
    {
        if (!_isLoaded) return;
        try { _PipelineUnregisterMeshEntity(entityId); }
        catch (Exception ex) { Logger.Error("PipelineUnregisterMeshEntity failed", ex); }
    }

    // ECS Camera component attach. Path B renderer wires
    // StandardRenderPipeline::UpdatePerFrame → scene_sync::SyncCamerasFromECS,
    // which reads the first entity holding a Camera component + Transform and
    // overrides RenderView's view/projection matrices. Returns 1 on success.
    //
    // projectionType: 0 = perspective, 1 = orthographic.
    // fieldOfView is stored as a fraction of π (MetalCamera multiplies by π at
    // projection time), so 1f/3f → 60° vertical FOV.
    [DllImport("PrimalEngine", EntryPoint = "AddEntityCamera", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, IntPtr upVector);

    public static uint AddEntityCamera(uint entityId, uint projectionType, float fieldOfView, float aspectRatio, float nearZ, float farZ, float[] upVector)
    {
        if (!_isLoaded) return 0;
        if (upVector == null || upVector.Length != 3)
        {
            // up_vector is optional per the C ABI; pass IntPtr.Zero for the default (0,1,0).
            try { return _AddEntityCamera(entityId, projectionType, fieldOfView, aspectRatio, nearZ, farZ, IntPtr.Zero); }
            catch (Exception ex) { Logger.Error("AddEntityCamera failed", ex); return 0; }
        }
        var pin = GCHandle.Alloc(upVector, GCHandleType.Pinned);
        try { return _AddEntityCamera(entityId, projectionType, fieldOfView, aspectRatio, nearZ, farZ, pin.AddrOfPinnedObject()); }
        catch (Exception ex) { Logger.Error("AddEntityCamera failed", ex); return 0; }
        finally { pin.Free(); }
    }

    // Streaming-mesh entity registration (Phase 9.3b). streamingMesh is
    // an opaque pointer owned by the C# caller (the pipeline holds a
    // non-owning reference).
    [DllImport("PrimalEngine", EntryPoint = "PipelineRegisterStreamingMeshEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern UInt64 _PipelineRegisterStreamingMeshEntity(IntPtr streamingMesh, IntPtr boundsMin, IntPtr boundsMax);
    public static UInt64 PipelineRegisterStreamingMeshEntity(IntPtr streamingMesh, float[] boundsMin, float[] boundsMax)
    {
        if (!_isLoaded || streamingMesh == IntPtr.Zero) return 0;
        if (boundsMin == null || boundsMin.Length != 3) return 0;
        if (boundsMax == null || boundsMax.Length != 3) return 0;
        try
        {
            var minHandle = GCHandle.Alloc(boundsMin, GCHandleType.Pinned);
            var maxHandle = GCHandle.Alloc(boundsMax, GCHandleType.Pinned);
            try { return _PipelineRegisterStreamingMeshEntity(streamingMesh, minHandle.AddrOfPinnedObject(), maxHandle.AddrOfPinnedObject()); }
            finally { minHandle.Free(); maxHandle.Free(); }
        }
        catch (Exception ex) { Logger.Error("PipelineRegisterStreamingMeshEntity failed", ex); return 0; }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineUpdateStreamingMeshEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineUpdateStreamingMeshEntity(UInt64 entityId, UInt64 generation, IntPtr boundsMin, IntPtr boundsMax);
    public static void PipelineUpdateStreamingMeshEntity(UInt64 entityId, UInt64 generation, float[] boundsMin, float[] boundsMax)
    {
        if (!_isLoaded) return;
        if (boundsMin == null || boundsMin.Length != 3) return;
        if (boundsMax == null || boundsMax.Length != 3) return;
        try
        {
            var minHandle = GCHandle.Alloc(boundsMin, GCHandleType.Pinned);
            var maxHandle = GCHandle.Alloc(boundsMax, GCHandleType.Pinned);
            try { _PipelineUpdateStreamingMeshEntity(entityId, generation, minHandle.AddrOfPinnedObject(), maxHandle.AddrOfPinnedObject()); }
            finally { minHandle.Free(); maxHandle.Free(); }
        }
        catch (Exception ex) { Logger.Error("PipelineUpdateStreamingMeshEntity failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineUnregisterStreamingMeshEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineUnregisterStreamingMeshEntity(UInt64 entityId);
    public static void PipelineUnregisterStreamingMeshEntity(UInt64 entityId)
    {
        if (!_isLoaded) return;
        try { _PipelineUnregisterStreamingMeshEntity(entityId); }
        catch (Exception ex) { Logger.Error("PipelineUnregisterStreamingMeshEntity failed", ex); }
    }

    // Editor mode toggle (lightweight forward rendering path used by the
    // editor preview; the engine's gameplay path bypasses this).
    [DllImport("PrimalEngine", EntryPoint = "PipelineSetEditorMode", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineSetEditorMode(uint enable);
    public static void PipelineSetEditorMode(bool enable)
    {
        if (!_isLoaded) return;
        try { _PipelineSetEditorMode(enable ? 1u : 0u); }
        catch (Exception ex) { Logger.Error("PipelineSetEditorMode failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineIsEditorMode", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _PipelineIsEditorMode();
    public static bool PipelineIsEditorMode()
    {
        if (!_isLoaded) return false;
        try { return _PipelineIsEditorMode() != 0; }
        catch (Exception ex) { Logger.Error("PipelineIsEditorMode failed", ex); return false; }
    }

    // Lumen GI quality preset (0=Off .. 5=UltraRT). Triggers Lumen
    // subsystem re-init — do not call per-frame.
    [DllImport("PrimalEngine", EntryPoint = "PipelineSetLumenConfig", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineSetLumenConfig(uint qualityPreset);
    public static void PipelineSetLumenConfig(LumenQualityPresetId preset)
    {
        if (!_isLoaded) return;
        try { _PipelineSetLumenConfig((uint)preset); }
        catch (Exception ex) { Logger.Error("PipelineSetLumenConfig failed", ex); }
    }

    // ====== Scene Setup (ResourceAPI: CreateCamera/Entity/RenderItem) ======

[DllImport("PrimalEngine", EntryPoint = "CreateEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CreateEntity(float pos_x, float pos_y, float pos_z);

    public static uint CreateEntity(float x, float y, float z)
    {
        if (!_isLoaded) return InvalidEntityId;
        try { return _CreateEntity(x, y, z); }
        catch (Exception ex) { Logger.Error("CreateEntity failed", ex); return InvalidEntityId; }
    }

    [DllImport("PrimalEngine", EntryPoint = "CreateCamera", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CreateCamera(uint entity_id, float fov, float aspect, float near_z, float far_z);

    public static uint CreateCamera(uint entityId, float fov, float aspect, float nearZ, float farZ)
    {
        if (!_isLoaded) return InvalidCameraId;
        try { return _CreateCamera(entityId, fov, aspect, nearZ, farZ); }
        catch (Exception ex) { Logger.Error("CreateCamera failed", ex); return InvalidCameraId; }
    }

    [DllImport("PrimalEngine", EntryPoint = "SetEntityCameraAspectRatio", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SetEntityCameraAspectRatio(uint entityId, float aspect);

    public static bool SetEntityCameraAspectRatio(uint entityId, float aspect)
    {
        if (!_isLoaded) return false;
        try { return _SetEntityCameraAspectRatio(entityId, aspect) != 0; }
        catch (Exception ex) { Logger.Error("SetEntityCameraAspectRatio failed", ex); return false; }
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

    [DllImport("PrimalEngine", EntryPoint = "GetEntityOrientation", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GetEntityOrientation(UInt32 entityId, [Out] float[] xyz);

    [DllImport("PrimalEngine", EntryPoint = "SetEntityOrientation", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SetEntityOrientation(UInt32 entityId, float[] xyz);

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

    /// <summary>Read the entity's forward orientation vector (local +Z
    /// axis in world space). The engine normalises this to a unit
    /// length internally so callers may pass any non-zero vector and
    /// the result is well-defined.</summary>
    public static (float x, float y, float z) GetEntityOrientation(uint entityId)
    {
        if (!_isLoaded) return (0, 0, 0);
        try
        {
            float[] xyz = new float[3];
            _GetEntityOrientation(entityId, xyz);
            return (xyz[0], xyz[1], xyz[2]);
        }
        catch (Exception ex)
        {
            Logger.Error($"GetEntityOrientation({entityId:X}) failed", ex);
            return (0, 0, 0);
        }
    }

    /// <summary>Write the entity's forward orientation vector. The
    /// engine normalises to a unit length. Other axes (right, up)
    /// are derived from this and the world up.</summary>
    public static bool SetEntityOrientation(uint entityId, float[] xyz)
    {
        if (!_isLoaded || entityId == 0 || entityId == InvalidEntityId) return false;
        if (xyz == null || xyz.Length < 3) return false;
        try { return _SetEntityOrientation(entityId, xyz) != 0; }
        catch (Exception ex) { Logger.Error("SetEntityOrientation failed", ex); return false; }
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
    // CreateCamera / CreateRenderTarget return 0xFFFFFFFF on failure
    // (matches the engine's id::invalid_id). Used by callers that want
    // a clearer name than comparing to the magic constant directly.
    public const UInt32 InvalidCameraId = 0xFFFFFFFFu;

    // True iff the handle is not the native failure sentinel. 0 is valid.
    public static bool IsValidHandle(UInt64 handle) => handle != InvalidContentId;
    public static bool IsValidEntity(UInt32 entityId) => entityId != InvalidEntityId;
    public static bool IsValidCamera(UInt32 cameraId) => cameraId != InvalidCameraId;

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
    public const uint RHIPlatformVulkan = 2;
    public const uint RHIPlatformD3D12 = 1;

    /// <summary>
    /// Returns the RHI platform id for the current host OS. This is the
    /// single point at which the editor maps "running on macOS" to "use
    /// Metal"; callers (CLI headless init, the MacMetalViewportHost
    /// implementation) use this rather than hard-coding the constant.
    /// </summary>
    public static uint DefaultRHIPlatformForCurrentPlatform()
    {
        if (System.OperatingSystem.IsMacOS()) return RHIPlatformMetal;
        if (System.OperatingSystem.IsWindows()) return RHIPlatformD3D12;
        return RHIPlatformVulkan;
    }

    public static bool InitializeEngine(uint rhiPlatform = RHIPlatformMetal, bool enableDebug = false)
    {
        if (!_isLoaded) return false;
        // C++ initialize_with_device() returns false on a second call because
        // g_rhiDevice is already non-null. The MacMetalViewportHost path calls
        // InitializeEngine from inside TryInitialize, which itself is retried
        // every 50ms until the host NSView grows non-zero bounds. On the
        // retry after the first successful init, the native call "fails"
        // (returning 0) and the bridge prints a spurious warning even
        // though the engine is fully running. Treat "already initialized"
        // as success so retries are silent.
        if (IsEngineInitialized()) return true;
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

    [DllImport("PrimalEngine", EntryPoint = "CreateStandardRenderPipeline", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CreateStandardRenderPipeline(UInt64 deviceHandle);

    [DllImport("PrimalEngine", EntryPoint = "DestroyStandardRenderPipeline", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DestroyStandardRenderPipeline();

    public static bool CreateStandardRenderPipeline(UInt64 deviceHandle)
    {
        if (!_isLoaded || deviceHandle == 0) return false;
        try { return _CreateStandardRenderPipeline(deviceHandle) != 0; }
        catch { return false; }
    }

    public static void DestroyStandardRenderPipeline()
    {
        if (!_isLoaded) return;
        try { _DestroyStandardRenderPipeline(); } catch { }
    }

    public enum RenderTargetFormat : uint { RGBA8 = 0, RGBA16F = 1, R8 = 2, Depth24 = 3 }

    [DllImport("PrimalEngine", EntryPoint = "CreateRenderTarget", CallingConvention = CallingConvention.Cdecl)]
    private static extern UInt64 _CreateRenderTarget(uint width, uint height, uint format);

    [DllImport("PrimalEngine", EntryPoint = "DestroyRenderTarget", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DestroyRenderTarget(UInt64 handle);

    [DllImport("PrimalEngine", EntryPoint = "DebugReadRenderTarget", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _DebugReadRenderTarget(UInt64 targetHandle, IntPtr outBuffer, UInt64 bufferSize);

    public static UInt64 CreateRenderTarget(int width, int height, RenderTargetFormat format)
    {
        if (!_isLoaded || width <= 0 || height <= 0) return 0;
        try { return _CreateRenderTarget((uint)width, (uint)height, (uint)format); }
        catch (Exception ex) { Logger.Error("CreateRenderTarget failed", ex); return 0; }
    }

    public static void DestroyRenderTarget(UInt64 handle)
    {
        if (!_isLoaded || handle == 0) return;
        try { _DestroyRenderTarget(handle); } catch (Exception ex) { Logger.Error("DestroyRenderTarget failed", ex); }
    }

    public static bool DebugReadRenderTarget(UInt64 handle, byte[] buffer)
    {
        if (!_isLoaded || handle == 0 || buffer == null) return false;
        var pin = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        try { return _DebugReadRenderTarget(handle, pin.AddrOfPinnedObject(), (UInt64)buffer.Length) != 0; }
        catch (Exception ex) { Logger.Error("DebugReadRenderTarget failed", ex); return false; }
        finally { pin.Free(); }
    }

    // Headless PNG export. RGBA16F target → RGBA8 PNG file. Path defaults
    // to "headless_render.png" in BaseDirectory if null/empty.
    [DllImport("PrimalEngine", EntryPoint = "SaveRenderTargetAsPng", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SaveRenderTargetAsPng(UInt64 targetHandle, [MarshalAs(UnmanagedType.LPStr)] string path);

    [DllImport("PrimalEngine", EntryPoint = "FlushGPU", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _FlushGPU();

    public static void FlushGPU()
    {
        if (!_isLoaded) return;
        try { _FlushGPU(); } catch (Exception ex) { Logger.Error("FlushGPU failed", ex); }
    }

    // --- Debug-draw helpers (editor gizmos) ---

    [DllImport("PrimalEngine", EntryPoint = "DrawDebugLine", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DrawDebugLine(
        float x0, float y0, float z0, float x1, float y1, float z1, uint rgb);

    public static void DrawDebugLine(
        float x0, float y0, float z0,
        float x1, float y1, float z1,
        byte r, byte g, byte b)
    {
        if (!_isLoaded) return;
        uint packed = ((uint)r << 16) | ((uint)g << 8) | b;
        try { _DrawDebugLine(x0, y0, z0, x1, y1, z1, packed); }
        catch (Exception ex) { Logger.Error("DrawDebugLine failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "DrawDebugBoxWireframe", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DrawDebugBoxWireframe(
        float cx, float cy, float cz, float ex, float ey, float ez, uint rgb);

    public static void DrawDebugBoxWireframe(
        float cx, float cy, float cz, float ex, float ey, float ez,
        byte r, byte g, byte b)
    {
        if (!_isLoaded) return;
        uint packed = ((uint)r << 16) | ((uint)g << 8) | b;
        try { _DrawDebugBoxWireframe(cx, cy, cz, ex, ey, ez, packed); }
        catch (Exception exc) { Logger.Error("DrawDebugBoxWireframe failed", exc); }
    }

    [DllImport("PrimalEngine", EntryPoint = "DrawDebugWorldQuad", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DrawDebugWorldQuad(
        float x0, float y0, float z0,
        float x1, float y1, float z1,
        float x2, float y2, float z2,
        float x3, float y3, float z3,
        uint rgb);

    public static void DrawDebugWorldQuad(
        float x0, float y0, float z0,
        float x1, float y1, float z1,
        float x2, float y2, float z2,
        float x3, float y3, float z3,
        byte r, byte g, byte b)
    {
        if (!_isLoaded) return;
        uint packed = ((uint)r << 16) | ((uint)g << 8) | b;
        try { _DrawDebugWorldQuad(x0, y0, z0, x1, y1, z1, x2, y2, z2, x3, y3, z3, packed); }
        catch (Exception ex) { Logger.Error("DrawDebugWorldQuad failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "DrawDebugLightBulb", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DrawDebugLightBulb(
        float cx, float cy, float cz, float radius, float rayLength, uint rgb);

    public static void DrawDebugLightBulb(
        float cx, float cy, float cz, float radius, float rayLength,
        byte r, byte g, byte b)
    {
        if (!_isLoaded) return;
        uint packed = ((uint)r << 16) | ((uint)g << 8) | b;
        try { _DrawDebugLightBulb(cx, cy, cz, radius, rayLength, packed); }
        catch (Exception ex) { Logger.Error("DrawDebugLightBulb failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "ClearDebugLines", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ClearDebugLines();

    public static void ClearDebugLines()
    {
        if (!_isLoaded) return;
        try { _ClearDebugLines(); } catch (Exception ex) { Logger.Error("ClearDebugLines failed", ex); }
    }

    // --- Scene environment push ---

    [DllImport("PrimalEngine", EntryPoint = "UpdateSceneEnvironment", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _UpdateSceneEnvironment(float[] params24);

    public static bool UpdateSceneEnvironment(float[] p)
    {
        if (!_isLoaded) return false;
        if (p == null || p.Length < 24) return false;
        try { return _UpdateSceneEnvironment(p) != 0; }
        catch (Exception ex) { Logger.Error("UpdateSceneEnvironment failed", ex); return false; }
    }

    public static bool SaveRenderTargetAsPng(UInt64 handle, string path)
    {
        if (!_isLoaded || handle == 0) return false;
        FlushGPU();
        try { return _SaveRenderTargetAsPng(handle, string.IsNullOrEmpty(path) ? null : path) != 0; }
        catch (Exception ex) { Logger.Error("SaveRenderTargetAsPng failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineRenderFrame", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _PipelineRenderFrame(UInt64 targetHandle, uint cameraId, uint surfaceId);

    [DllImport("PrimalEngine", EntryPoint = "PipelineRenderFrameForEditorCamera", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _PipelineRenderFrameForEditorCamera(UInt64 targetHandle, uint cameraId, uint surfaceId);

    [DllImport("PrimalEngine", EntryPoint = "PipelineRenderFrameForSceneCamera", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _PipelineRenderFrameForSceneCamera(UInt64 targetHandle, uint cameraId, uint surfaceId, uint sceneCameraEntityId);

    [DllImport("PrimalEngine", EntryPoint = "BlitRenderTargetToSurface", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _BlitRenderTargetToSurface(UInt64 targetHandle, uint surfaceId);

    // surface_id ~0u means headless (no blit, no drawable).
    public const uint PipelineRenderFrameHeadlessSurface = 0xFFFFFFFFu;

    public static bool PipelineRenderFrame(UInt64 targetHandle, uint cameraId, uint surfaceId)
    {
        if (!_isLoaded || targetHandle == 0 || cameraId == 0xFFFFFFFFu) return false;
        try { return _PipelineRenderFrame(targetHandle, cameraId, surfaceId) != 0; }
        catch (Exception ex) { Logger.Error("PipelineRenderFrame failed", ex); return false; }
    }

    /// <summary>Same as <see cref="PipelineRenderFrame"/> but tags the resulting
    /// RenderView as the editor camera so the ECS Camera sync layer does not
    /// overwrite the editor's orbit/pan/zoom matrices with a scene Camera
    /// entity. Call this from the 3D viewport's render tick (which uses the
    /// editor's per-frame camera), NOT for any in-game / scene render.</summary>
    public static bool PipelineRenderFrameForEditorCamera(UInt64 targetHandle, uint cameraId, uint surfaceId)
    {
        if (!_isLoaded || targetHandle == 0 || cameraId == 0xFFFFFFFFu) return false;
        try { return _PipelineRenderFrameForEditorCamera(targetHandle, cameraId, surfaceId) != 0; }
        catch (Exception ex) { Logger.Error("PipelineRenderFrameForEditorCamera failed", ex); return false; }
    }

    public static bool PipelineRenderFrameForSceneCamera(UInt64 targetHandle, uint cameraId, uint surfaceId, uint sceneCameraEntityId)
    {
        if (!_isLoaded || targetHandle == 0 || cameraId == InvalidCameraId ||
            sceneCameraEntityId == 0 || sceneCameraEntityId == InvalidEntityId) return false;
        try { return _PipelineRenderFrameForSceneCamera(targetHandle, cameraId, surfaceId, sceneCameraEntityId) != 0; }
        catch (Exception ex) { Logger.Error("PipelineRenderFrameForSceneCamera failed", ex); return false; }
    }

    public static bool BlitRenderTargetToSurface(UInt64 targetHandle, uint surfaceId)
    {
        if (!_isLoaded || targetHandle == 0) return false;
        try { return _BlitRenderTargetToSurface(targetHandle, surfaceId) != 0; }
        catch (Exception ex) { Logger.Error("BlitRenderTargetToSurface failed", ex); return false; }
    }

    // PipelineLightDesc is fed through unmanaged memory as opaque bytes so
    // we don't have to recreate the layout in C#. Size is verified at call.
    public const int PipelineLightDescSize = 64;

    [DllImport("PrimalEngine", EntryPoint = "PipelineRegisterLightEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern UInt64 _PipelineRegisterLightEntity(IntPtr desc);

    [DllImport("PrimalEngine", EntryPoint = "PipelineUpdateLightEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _PipelineUpdateLightEntity(UInt64 entityId, IntPtr desc);

    [DllImport("PrimalEngine", EntryPoint = "PipelineUnregisterLightEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineUnregisterLightEntity(UInt64 entityId);

    public static UInt64 PipelineRegisterLightEntity(byte[] desc)
    {
        if (!_isLoaded || desc == null || desc.Length < PipelineLightDescSize) return 0;
        var pin = GCHandle.Alloc(desc, GCHandleType.Pinned);
        try { return _PipelineRegisterLightEntity(pin.AddrOfPinnedObject()); }
        catch (Exception ex) { Logger.Error("PipelineRegisterLightEntity failed", ex); return 0; }
        finally { pin.Free(); }
    }

    public static bool PipelineUpdateLightEntity(UInt64 entityId, byte[] desc)
    {
        if (!_isLoaded || entityId == 0 || desc == null || desc.Length < PipelineLightDescSize) return false;
        var pin = GCHandle.Alloc(desc, GCHandleType.Pinned);
        try { return _PipelineUpdateLightEntity(entityId, pin.AddrOfPinnedObject()) != 0; }
        catch (Exception ex) { Logger.Error("PipelineUpdateLightEntity failed", ex); return false; }
        finally { pin.Free(); }
    }

    public static void PipelineUnregisterLightEntity(UInt64 entityId)
    {
        if (!_isLoaded || entityId == 0) return;
        try { _PipelineUnregisterLightEntity(entityId); } catch { }
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

    /// <summary>
    /// Cook result for a single source image. Created by
    /// <see cref="NativeEngineBridge.CookTexture"/>; the slices/icon byte
    /// arrays are caller-owned and may be passed to a TEXR writer on the
    /// managed side.
    /// </summary>
    public sealed class CookedTexturePayload
    {
        public string SourcePath { get; }
        public byte[] Slices { get; }
        public byte[] Icon { get; }
        public uint Width { get; }
        public uint Height { get; }
        public uint ArraySize { get; }
        public uint MipLevels { get; }
        public uint Format { get; }
        public uint Flags { get; }
        public uint SourcesLen { get; }
        public uint SourceCount { get; }
        public uint SettingsDimension { get; }
        public uint SettingsMipLevels { get; }
        public float SettingsAlphaThreshold { get; }
        public uint SettingsPreferBc7 { get; }
        public uint SettingsOutputFormat { get; }
        public uint SettingsCompress { get; }

        public CookedTexturePayload(
            string sourcePath, byte[] slices, byte[] icon,
            uint width, uint height, uint arraySize, uint mipLevels,
            uint format, uint flags,
            uint sourcesLen, uint sourceCount,
            uint settingsDimension, uint settingsMipLevels, float settingsAlphaThreshold,
            uint settingsPreferBc7, uint settingsOutputFormat, uint settingsCompress)
        {
            SourcePath = sourcePath;
            Slices = slices;
            Icon = icon;
            Width = width;
            Height = height;
            ArraySize = arraySize;
            MipLevels = mipLevels;
            Format = format;
            Flags = flags;
            SourcesLen = sourcesLen;
            SourceCount = sourceCount;
            SettingsDimension = settingsDimension;
            SettingsMipLevels = settingsMipLevels;
            SettingsAlphaThreshold = settingsAlphaThreshold;
            SettingsPreferBc7 = settingsPreferBc7;
            SettingsOutputFormat = settingsOutputFormat;
            SettingsCompress = settingsCompress;
        }
    }

    [DllImport("PrimalEngine", EntryPoint = "InitializeAssetSystem", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _InitializeAssetSystem();

    [DllImport("PrimalEngine", EntryPoint = "ShutdownAssetSystem", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ShutdownAssetSystem();

    [DllImport("PrimalEngine", EntryPoint = "ImportTexture", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern uint _ImportTexture(IntPtr path, out TextureImportResult result);

    [DllImport("PrimalEngine", EntryPoint = "ImportMeshAsync", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern ulong _ImportMeshAsync(IntPtr path);

    [DllImport("PrimalEngine", EntryPoint = "TryGetMeshImportResult", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _TryGetMeshImportResult(ulong requestId, out ulong meshContentId, out uint status);

    [DllImport("libContentTools", EntryPoint = "CookMesh", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern uint _CookMesh(IntPtr input, IntPtr output);

    [DllImport("libContentTools", EntryPoint = "CookMeshWithSettings", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern uint _CookMeshWithSettings(
        IntPtr input, IntPtr output,
        float smoothing_angle,
        [MarshalAs(UnmanagedType.U1)] bool calculate_normals,
        [MarshalAs(UnmanagedType.U1)] bool calculate_tangents,
        [MarshalAs(UnmanagedType.U1)] bool reverse_handedness,
        [MarshalAs(UnmanagedType.U1)] bool import_embedded_textures,
        [MarshalAs(UnmanagedType.U1)] bool import_animations,
        [MarshalAs(UnmanagedType.U1)] bool coalesce_meshes);

    // CookMeshWithPipeline: Phase 2 M13. Takes a fully-populated
    // ai_asset_pipeline_params by value (Sequential layout, 208 bytes).
    // C# caller builds the struct from GeometryImportSettings; the native
    // side runs ProcessAIAsset then writes .mesh + .model.json sidecar.
    [DllImport("libContentTools", EntryPoint = "CookMeshWithPipeline", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern uint _CookMeshWithPipeline(
        IntPtr input,
        IntPtr output,
        AIAssetPipelineSettings settings);

    [StructLayout(LayoutKind.Sequential)]
    public struct TextureCookResult
    {
        public uint InfoWidth;
        public uint InfoHeight;
        public uint InfoArraySize;
        public uint InfoMipLevels;
        public uint InfoFormat;
        public uint InfoFlags;
        public uint ImportError;
        public uint SourcesLen;
        public uint SourceCount;
        public uint SettingsDimension;
        public uint SettingsMipLevels;
        public float SettingsAlphaThreshold;
        public uint SettingsPreferBc7;
        public uint SettingsOutputFormat;
        public uint SettingsCompress;
        public uint SlicesSize;
        public uint IconSize;
        public uint ImportSucceeded;
    }

    [DllImport("libContentTools", EntryPoint = "CookTexture", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern uint _CookTexture(
        IntPtr input,
        uint dimension, float alpha_threshold,
        uint mip_levels_setting, uint prefer_bc7,
        uint output_format, uint compress,
        IntPtr slices_buffer, uint slices_capacity, out uint out_slices_size,
        IntPtr icon_buffer,   uint icon_capacity,   out uint out_icon_size,
        out TextureCookResult out_info);

    [DllImport("libContentTools", EntryPoint = "CookTextureQuery", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern uint _CookTextureQuery(
        IntPtr input,
        uint dimension, float alpha_threshold,
        uint mip_levels_setting, uint prefer_bc7,
        uint output_format, uint compress,
        out TextureCookResult out_info);

    [DllImport("PrimalEngine", EntryPoint = "TickAssetSystem", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _TickAssetSystem();

    [DllImport("PrimalEngine", EntryPoint = "TickMainThreadJobs", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _TickMainThreadJobs();

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

    public enum MeshImportResultStatus : uint
    {
        Pending = 0,
        Success = 1,
        Failed = 2,
        NotFound = 3,
    }

    public static (ulong meshContentId, MeshImportResultStatus status) TryGetMeshImportResult(ulong requestId)
    {
        if (!_isLoaded || requestId == 0)
            return (0, MeshImportResultStatus.NotFound);
        try
        {
            _TryGetMeshImportResult(requestId, out var meshId, out var status);
            return (meshId, (MeshImportResultStatus)status);
        }
        catch (Exception ex) { Logger.Error("TryGetMeshImportResult failed", ex); return (0, MeshImportResultStatus.Failed); }
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

    public static bool CookMesh(string input, string output, GeometryImportSettings settings)
    {
        if (!_isLoaded) return false;
        try
        {
            var i = MarshalUtf8(input);
            var o = MarshalUtf8(output);
            return _CookMeshWithSettings(
                i, o,
                settings.SmoothingAngle,
                settings.CalculateNormals,
                settings.CalculateTangents,
                settings.ReverseHandedness,
                settings.ImportEmbeddedTextures,
                settings.ImportAnimations,
                settings.CoalesceMeshes) != 0;
        }
        catch (Exception ex) { Logger.Error("CookMesh failed", ex); return false; }
    }

    /// <summary>
    /// Cook a mesh through the AssetPipeline (Phase 2 M13). The supplied
    /// settings carry every ContentTools parameter — legacy FBX/OBJ parse
    /// flags AND every pipeline module enable flag + tuning param. Returns
    /// true on success, false on cook failure or native bridge error.
    /// </summary>
    public static bool CookMeshWithPipeline(string input, string output, AIAssetPipelineSettings settings)
    {
        if (!_isLoaded) return false;
        try
        {
            var i = MarshalUtf8(input);
            var o = MarshalUtf8(output);
            return _CookMeshWithPipeline(i, o, settings) != 0;
        }
        catch (Exception ex) { Logger.Error("CookMeshWithPipeline failed", ex); return false; }
    }

    public enum CookTextureStatus
    {
        NativeUnavailable = -1,
        InvalidArgument = -2,
        ProbeFailed = -3,
        CookFailed = -4,
        CapacityExceeded = -5,
        ProbeSucceeded = 0,
    }

    /// <summary>
    /// Cook a texture via ContentTools. Two-phase: first probe the
    /// required buffer sizes (<c>CookTextureQuery</c>), then run
    /// <c>CookTexture</c> with pre-sized buffers. On failure, status
    /// carries the C++ <c>info.import_error</c> code from the cook probe
    /// (1=max_size_exceeded, 2=compress, 4=load, 8=size_mismatch,
    /// 9=format_mismatch, 9=file_not_found etc. — see
    /// ContentTools/TextureImporter.cpp::import_error).
    /// </summary>
    public static (CookedTexturePayload? payload, CookTextureStatus status, uint nativeErrorCode) CookTexture(
        string input, Editors.Texture.TextureImportSettings settings)
    {
        if (!_isLoaded) return (null, CookTextureStatus.NativeUnavailable, 0);
        if (string.IsNullOrEmpty(input)) return (null, CookTextureStatus.InvalidArgument, 0);

        IntPtr pInput = IntPtr.Zero;
        try
        {
            pInput = MarshalUtf8(input);

            var managed = new Editors.Texture.TextureImportSettings();
            PrimalEditor_Avalonia.Content.AssetImportSettingsExtensions.CopyImportSettings(settings, managed);

            var probeInfo = new TextureCookResult();
            var probeOk = _CookTextureQuery(
                pInput,
                (uint)managed.Dimension, managed.AlphaThreshold,
                (uint)managed.MipLevels, managed.PreferBC7 ? 1u : 0u,
                (uint)managed.OutputFormat, managed.Compress ? 1u : 0u,
                out probeInfo);

            if (probeOk == 0 || probeInfo.ImportSucceeded == 0)
            {
                return (null, CookTextureStatus.ProbeFailed, probeInfo.ImportError);
            }

            uint neededSlices = probeInfo.SlicesSize;
            uint neededIcon   = probeInfo.IconSize;
            IntPtr pSlices = neededSlices > 0 ? Marshal.AllocHGlobal((int)neededSlices) : IntPtr.Zero;
            IntPtr pIcon   = neededIcon   > 0 ? Marshal.AllocHGlobal((int)neededIcon)   : IntPtr.Zero;
            try
            {
                var info = new TextureCookResult();
                var ok = _CookTexture(
                    pInput,
                    (uint)managed.Dimension, managed.AlphaThreshold,
                    (uint)managed.MipLevels, managed.PreferBC7 ? 1u : 0u,
                    (uint)managed.OutputFormat, managed.Compress ? 1u : 0u,
                    pSlices, neededSlices, out uint slicesSize,
                    pIcon,   neededIcon,   out uint iconSize,
                    out info);

                if (ok == 0 || info.ImportSucceeded == 0)
                {
                    Logger.Error($"CookTexture cook phase failed. probe slices/icon={probeInfo.SlicesSize}/{probeInfo.IconSize}, cook slices/icon={slicesSize}/{iconSize}, nativeError={info.ImportError}");
                    return (null, CookTextureStatus.CookFailed, info.ImportError);
                }

                var slices = new byte[slicesSize];
                var icon   = new byte[iconSize];
                if (slicesSize > 0) Marshal.Copy(pSlices, slices, 0, (int)slicesSize);
                if (iconSize   > 0) Marshal.Copy(pIcon,   icon,   0, (int)iconSize);

                return (new CookedTexturePayload(
                    input,
                    slices, icon,
                    info.InfoWidth, info.InfoHeight, info.InfoArraySize, info.InfoMipLevels,
                    info.InfoFormat, info.InfoFlags,
                    info.SourcesLen, info.SourceCount,
                    info.SettingsDimension, info.SettingsMipLevels, info.SettingsAlphaThreshold,
                    info.SettingsPreferBc7, info.SettingsOutputFormat, info.SettingsCompress),
                    CookTextureStatus.ProbeSucceeded, info.ImportError);
            }
            finally
            {
                if (pSlices != IntPtr.Zero) Marshal.FreeHGlobal(pSlices);
                if (pIcon   != IntPtr.Zero) Marshal.FreeHGlobal(pIcon);
            }
        }
        catch (Exception ex)
        {
            Logger.Error("CookTexture failed", ex);
            return (null, CookTextureStatus.CookFailed, 0);
        }
        finally
        {
            if (pInput != IntPtr.Zero) Marshal.FreeHGlobal(pInput);
        }
    }

    public static void TickAssetSystem()
    {
        if (!_isLoaded) return;
        try { _TickAssetSystem(); }
        catch (Exception ex) { Logger.Error("TickAssetSystem failed", ex); }
    }

    public static void TickMainThreadJobs()
    {
        if (!_isLoaded) return;
        try { _TickMainThreadJobs(); }
        catch (Exception ex) { Logger.Error("TickMainThreadJobs failed", ex); }
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

    // =========================================================================
    // Editor render loop — native CFRunLoopTimer-driven frame callback.
    // macOS only. The native side calls back into C# via a function pointer
    // registered once at startup, so Avalonia's DispatcherTimer is not
    // involved in frame submission.
    // =========================================================================

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void NativeFrameCb(uint loopId, float dt);

    [DllImport("PrimalEngine", EntryPoint = "EditorSetFrameCallback", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _EditorSetFrameCallback(NativeFrameCb cb);

    [DllImport("PrimalEngine", EntryPoint = "EditorCreateRenderLoop", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _EditorCreateRenderLoop(int fps);

    [DllImport("PrimalEngine", EntryPoint = "EditorStartRenderLoop", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _EditorStartRenderLoop(uint loopId);

    // Returns the loop id on success, 0 on failure. The native side returns
    // 0 when the loop is already destroyed (slot freed) or CFRunLoopTimerCreate
    // failed. Treating it as void previously hid both failure modes from C#.
    public static uint StartRenderLoopRaw(uint loopId)
    {
        if (!_isLoaded || loopId == 0) return 0;
        try { return _EditorStartRenderLoop(loopId); }
        catch (Exception ex) { Logger.Error("EditorStartRenderLoop failed", ex); return 0; }
    }

    [DllImport("PrimalEngine", EntryPoint = "EditorStopRenderLoop", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _EditorStopRenderLoop(uint loopId);

    [DllImport("PrimalEngine", EntryPoint = "EditorDestroyRenderLoop", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _EditorDestroyRenderLoop(uint loopId);

    public static class EditorRenderLoop
    {
        // _callbacks is read on the GCD worker thread (from inside
        // fire_timer's @autoreleasepool block in EngineAPI_mac.mm) and
        // written on the UI thread (Start/Stop on the editor's render
        // loop start/stop). System.Collections.Generic.Dictionary is
        // not thread-safe — concurrent read/write can corrupt the
        // internal buckets array, producing the same tagged-pointer
        // crash signature as the autoreleased-reference bug. Use
        // ConcurrentDictionary so reads and writes are safe.
        private static readonly System.Collections.Concurrent.ConcurrentDictionary<uint, Action<float>> _callbacks = new();
        private static GCHandle _thunkHandle;
        private static NativeFrameCb? _thunk;
        private static bool _thunkRegistered;

        private static void EnsureThunkRegistered()
        {
            if (_thunkRegistered) return;
            _thunk = (loopId, dt) =>
            {
                if (_callbacks.TryGetValue(loopId, out var cb))
                {
                    try { cb(dt); }
                    catch (Exception ex) { Logger.Error("EditorRenderLoop callback threw", ex); }
                }
            };
            _thunkHandle = GCHandle.Alloc(_thunk);
            _EditorSetFrameCallback(_thunk);
            _thunkRegistered = true;
        }

        public static uint Start(int fps, Action<float> onFrame)
        {
            if (!_isLoaded) return 0;
            if (onFrame == null) return 0;

            EnsureThunkRegistered();
            uint loopId = _EditorCreateRenderLoop(fps);
            if (loopId == 0) return 0;

            _callbacks[loopId] = onFrame;
            // Propagate the native-side success/failure signal. A return of
            // 0 means the CFRunLoopTimer could not be created (e.g. OOM or
            // running off the AppKit main thread) — caller should Destroy
            // and report the failure rather than treat the loop as running.
            uint started = StartRenderLoopRaw(loopId);
            if (started == 0)
            {
                Destroy(loopId);
                return 0;
            }
            return loopId;
        }

        public static void Stop(uint loopId)
        {
            if (loopId == 0 || !_isLoaded) return;
            try { _EditorStopRenderLoop(loopId); } catch { }
            // TryRemove avoids the CollectionExtensions.Remove extension method
            // ambiguity (Dictionary has both, ConcurrentDictionary only has
            // TryRemove). ConcurrentDictionary.TryRemove is the thread-safe
            // removal API; the discarded out value is irrelevant here.
            _callbacks.TryRemove(loopId, out _);
        }

        public static void Destroy(uint loopId)
        {
            if (loopId == 0 || !_isLoaded) return;
            Stop(loopId);
            try { _EditorDestroyRenderLoop(loopId); } catch { }
        }
    }
}
