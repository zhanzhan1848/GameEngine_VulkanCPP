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

    // Viewport render mode preset (plan §6.1 B3): atomically apply one of
    // the toolbar's Forward / Deferred / +Shadow+AO / +Shadow+AO+GI modes.
    // mode: 0=Forward, 1=Deferred, 2=DeferredShadowAO, 3=DeferredShadowAOGI.
    [DllImport("PrimalEngine", EntryPoint = "PipelineApplyModePreset", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineApplyModePreset(uint mode);
    public static void PipelineApplyModePreset(uint mode)
    {
        if (!_isLoaded) return;
        try { _PipelineApplyModePreset(mode); }
        catch (Exception ex) { Logger.Error("PipelineApplyModePreset failed", ex); }
    }

    // Viewport view mode (plan §6.3 A7): 0=Shaded, 1=Wireframe, 2=Unlit
    // (reserved). Cheap per-frame value; orthogonal to the pass-combo
    // presets above.
    [DllImport("PrimalEngine", EntryPoint = "PipelineSetViewMode", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineSetViewMode(uint mode);
    public static void PipelineSetViewMode(uint mode)
    {
        if (!_isLoaded) return;
        try { _PipelineSetViewMode(mode); }
        catch (Exception ex) { Logger.Error("PipelineSetViewMode failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineSetProbeSphereDebug", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineSetProbeSphereDebug(uint enabled);
    public static void PipelineSetProbeSphereDebug(bool enabled)
    {
        if (!_isLoaded) return;
        try { _PipelineSetProbeSphereDebug(enabled ? 1u : 0u); }
        catch (Exception ex) { Logger.Error("PipelineSetProbeSphereDebug failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineGetProbeSphereDebug", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _PipelineGetProbeSphereDebug();
    public static bool PipelineGetProbeSphereDebug()
    {
        if (!_isLoaded) return false;
        try { return _PipelineGetProbeSphereDebug() != 0; }
        catch (Exception ex) { Logger.Error("PipelineGetProbeSphereDebug failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineGetViewMode", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _PipelineGetViewMode();
    public static uint PipelineGetViewMode()
    {
        if (!_isLoaded) return 0;
        try { return _PipelineGetViewMode(); }
        catch (Exception ex) { Logger.Error("PipelineGetViewMode failed", ex); return 0; }
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
    [DllImport("PrimalEngine", EntryPoint = "PipelineImportCookedTexture", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern UInt64 _PipelineImportCookedTexture(IntPtr path);
    public static UInt64 PipelineImportCookedTexture(string path)
    {
        if (!_isLoaded || string.IsNullOrWhiteSpace(path)) return InvalidContentId;
        try { return _PipelineImportCookedTexture(MarshalUtf8(path)); }
        catch (Exception ex) { Logger.Error("PipelineImportCookedTexture failed", ex); return InvalidContentId; }
    }

    // HDR environment map (equirect .hdr): skybox + baked IBL. Returns true
    // when the map was loaded and baked (or was already cached).
    [DllImport("PrimalEngine", EntryPoint = "PipelineSetEnvironmentMap", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern uint _PipelineSetEnvironmentMap(IntPtr path);
    public static bool PipelineSetEnvironmentMap(string path)
    {
        if (!_isLoaded || string.IsNullOrWhiteSpace(path)) return false;
        try { return _PipelineSetEnvironmentMap(MarshalUtf8(path)) != 0; }
        catch (Exception ex) { Logger.Error("PipelineSetEnvironmentMap failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineClearEnvironmentMap", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineClearEnvironmentMap();
    public static void PipelineClearEnvironmentMap()
    {
        if (!IsLoaded) return;
        try { _PipelineClearEnvironmentMap(); }
        catch (Exception ex) { Logger.Error("PipelineClearEnvironmentMap failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineSetSkyboxEnabled", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineSetSkyboxEnabled(uint enabled);
    public static void PipelineSetSkyboxEnabled(bool enabled)
    {
        if (!_isLoaded) return;
        try { _PipelineSetSkyboxEnabled(enabled ? 1u : 0u); } catch (Exception ex) { Logger.Error("PipelineSetSkyboxEnabled failed", ex); }
    }

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

    [DllImport("PrimalEngine", EntryPoint = "PipelineSetEntityMaterialGraph", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern UInt32 _PipelineSetEntityMaterialGraph(UInt32 entityId, IntPtr utf8Json);
    /// <summary>
    /// Compile the material graph JSON (absolute texture paths — the
    /// PreparePreviewJson expansion; the disk sidecar stays portable) and
    /// attach the generated-shader pipeline to the entity's world rendering.
    /// Returns false when compilation fails; the entity then keeps rendering
    /// through the fixed three-slot material path.
    /// </summary>
    public static bool PipelineSetEntityMaterialGraph(uint entityId, string graphJsonUtf8)
    {
        if (!_isLoaded || string.IsNullOrWhiteSpace(graphJsonUtf8)) return false;
        try { return _PipelineSetEntityMaterialGraph(entityId, MarshalUtf8(graphJsonUtf8)) != 0; }
        catch (Exception ex) { Logger.Error("PipelineSetEntityMaterialGraph failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "PipelineClearEntityMaterialGraph", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _PipelineClearEntityMaterialGraph(UInt32 entityId);
    public static void PipelineClearEntityMaterialGraph(uint entityId)
    {
        if (!_isLoaded) return;
        try { _PipelineClearEntityMaterialGraph(entityId); }
        catch (Exception ex) { Logger.Error("PipelineClearEntityMaterialGraph failed", ex); }
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

    // GI probe freeze (plan §2 A10): freeze DDGI probe updates so a static
    // scene skips the per-frame trace/update chain after convergence.
    [DllImport("PrimalEngine", EntryPoint = "GISetProbeFreeze", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _GISetProbeFreeze(uint enabled);
    public static void GISetProbeFreeze(bool enabled)
    {
        if (!_isLoaded) return;
        try { _GISetProbeFreeze(enabled ? 1u : 0u); }
        catch (Exception ex) { Logger.Error("GISetProbeFreeze failed", ex); }
    }

    // Probe debug visualization mode (plan §6.2 B1 L1): 0=Off, 1=IrradianceAtlas,
    // 2=DepthAtlas, 3=Confidence, 4=ProbePositions (data path: Vulkan GIGather).
    [DllImport("PrimalEngine", EntryPoint = "GISetProbeVisMode", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _GISetProbeVisMode(uint mode);
    public static void GISetProbeVisMode(uint mode)
    {
        if (!_isLoaded) return;
        try { _GISetProbeVisMode(mode); }
        catch (Exception ex) { Logger.Error("GISetProbeVisMode failed", ex); }
    }

    // -----------------------------------------------------------------------
    // GI volume CRUD (plan §6.2 B1 L1). LumenVolumeConfig
    // (Engine/Graphics/Lumen/LumenTypes.h:31-41) is marshaled as an opaque
    // 64-byte buffer with the fixed field offsets below; sizeof on the C++
    // side is 44 (one trailing bool + padding). If LumenTypes.h adds fields,
    // update the offsets AND GIVolumeConfigCapacity guard.
    // -----------------------------------------------------------------------
    public const int GIVolumeConfigCapacity = 64;
    public const int GIOffCountX = 0;
    public const int GIOffCountY = 4;
    public const int GIOffCountZ = 8;
    public const int GIOffSpacing = 12;
    public const int GIOffRaysPerProbe = 16;
    public const int GIOffMaxDistance = 20;
    public const int GIOffBudgetDivisor = 24;
    public const int GIOffOriginX = 28;
    public const int GIOffOriginY = 32;
    public const int GIOffOriginZ = 36;
    public const int GIOffUseOrigin = 40; // bool (1 byte)

    [DllImport("PrimalEngine", EntryPoint = "GIGetVolumeOverrideCount", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GIGetVolumeOverrideCount();

    [DllImport("PrimalEngine", EntryPoint = "GIGetVolumeOverride", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GIGetVolumeOverride(uint index, [Out] byte[] outConfig);

    [DllImport("PrimalEngine", EntryPoint = "GISetVolumeOverride", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GISetVolumeOverride(uint index, byte[] config);

    [DllImport("PrimalEngine", EntryPoint = "GIAppendVolumeOverride", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GIAppendVolumeOverride();

    [DllImport("PrimalEngine", EntryPoint = "GIRemoveVolumeOverride", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GIRemoveVolumeOverride(uint index);

    [DllImport("PrimalEngine", EntryPoint = "GIMaterializeDerivedVolumes", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GIMaterializeDerivedVolumes();

    // -----------------------------------------------------------------------
    // Particle emitter editing channel (plan §6.3 B4 P1).
    // -----------------------------------------------------------------------
    [DllImport("PrimalEngine", EntryPoint = "ParticleEnsureSystems", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _ParticleEnsureSystems();
    public static bool ParticleEnsureSystems()
    {
        if (!_isLoaded) return false;
        try { return _ParticleEnsureSystems() != 0; }
        catch (Exception ex) { Logger.Error("ParticleEnsureSystems failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "ParticleEmitterConfigSize", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _ParticleEmitterConfigSize();
    public static uint ParticleEmitterConfigSize()
    {
        if (!_isLoaded) return 0;
        try { return _ParticleEmitterConfigSize(); }
        catch (Exception ex) { Logger.Error("ParticleEmitterConfigSize failed", ex); return 0; }
    }

    [DllImport("PrimalEngine", EntryPoint = "EmitterCreateForEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _EmitterCreateForEntity(uint entityId, byte[] config, uint size);
    /// <summary>Attach a Particle component + create the emitter. Returns the
    /// emitter id (0 = failure). config must come from ParticlePresetApplyToConfig
    /// (engine fills defaults + preset scalars; curve pointers stay null).</summary>
    public static uint EmitterCreateForEntity(uint entityId, byte[] config)
    {
        if (!_isLoaded || config == null) return 0;
        try { return _EmitterCreateForEntity(entityId, config, (uint)config.Length); }
        catch (Exception ex) { Logger.Error($"EmitterCreateForEntity({entityId:X}) failed", ex); return 0; }
    }

    [DllImport("PrimalEngine", EntryPoint = "EmitterRemoveFromEntity", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _EmitterRemoveFromEntity(uint entityId);
    public static bool EmitterRemoveFromEntity(uint entityId)
    {
        if (!_isLoaded) return false;
        try { return _EmitterRemoveFromEntity(entityId) != 0; }
        catch (Exception ex) { Logger.Error($"EmitterRemoveFromEntity({entityId:X}) failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "EmitterSetSpawnRate", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _EmitterSetSpawnRate(uint entityId, float rate);
    public static bool EmitterSetSpawnRate(uint entityId, float rate)
    {
        if (!_isLoaded) return false;
        try { return _EmitterSetSpawnRate(entityId, rate) != 0; }
        catch (Exception ex) { Logger.Error("EmitterSetSpawnRate failed", ex); return false; }
    }

    // B4 P3: per-slot curve editing. Wire format (mirrors
    // EngineDLL/ParticleEmitterAPI.cpp): u32 interpolation + u32 keyCount +
    // keys — float slots (0 scale / 1 alpha / 3 velocity / 5 rotation) carry
    // 16B keys {t, v, inTan, outTan}; color slot (2) 20B keys {t, r,g,b,a};
    // vector slot (4) 16B keys {t, x,y,z}.

    [DllImport("PrimalEngine", EntryPoint = "EmitterGetCurveData", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _EmitterGetCurveData(uint entityId, uint slot, byte[] outBuffer, uint cap, out uint outLen);
    /// <summary>Wire bytes for the slot's curve, or null when the slot has
    /// no curve enabled.</summary>
    public static byte[]? EmitterGetCurveData(uint entityId, uint slot)
    {
        if (!_isLoaded) return null;
        try
        {
            var buf = new byte[64 * 1024];
            if (_EmitterGetCurveData(entityId, slot, buf, (uint)buf.Length, out var len) == 0)
                return null;
            if (len == 0) return null;
            var result = new byte[len];
            Array.Copy(buf, result, len);
            return result;
        }
        catch (Exception ex) { Logger.Error("EmitterGetCurveData failed", ex); return null; }
    }

    [DllImport("PrimalEngine", EntryPoint = "EmitterSetCurveData", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _EmitterSetCurveData(uint entityId, uint slot, byte[] data, uint size);
    public static bool EmitterSetCurveData(uint entityId, uint slot, byte[] data)
    {
        if (!_isLoaded || data == null) return false;
        try { return _EmitterSetCurveData(entityId, slot, data, (uint)data.Length) != 0; }
        catch (Exception ex) { Logger.Error("EmitterSetCurveData failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "EmitterDisableCurve", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _EmitterDisableCurve(uint entityId, uint slot);
    public static bool EmitterDisableCurve(uint entityId, uint slot)
    {
        if (!_isLoaded) return false;
        try { return _EmitterDisableCurve(entityId, slot) != 0; }
        catch (Exception ex) { Logger.Error("EmitterDisableCurve failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "ParticlePresetList", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _ParticlePresetList(byte[] outBuffer, uint cap, out uint outLen);
    /// <summary>Preset names joined by '\n' (includes the 8 built-ins once
    /// systems are ensured). Returns null when the buffer needs to grow.</summary>
    public static string? ParticlePresetList()
    {
        if (!_isLoaded) return null;
        try
        {
            var buf = new byte[4096];
            if (_ParticlePresetList(buf, (uint)buf.Length, out var len) == 0) return null;
            return System.Text.Encoding.UTF8.GetString(buf, 0, (int)len - 1);
        }
        catch (Exception ex) { Logger.Error("ParticlePresetList failed", ex); return null; }
    }

    [DllImport("PrimalEngine", EntryPoint = "ParticlePresetSaveToFile", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _ParticlePresetSaveToFile(
        [MarshalAs(UnmanagedType.LPStr)] string name, [MarshalAs(UnmanagedType.LPStr)] string path);
    public static bool ParticlePresetSaveToFile(string name, string path)
    {
        if (!_isLoaded) return false;
        try { return _ParticlePresetSaveToFile(name, path) != 0; }
        catch (Exception ex) { Logger.Error($"ParticlePresetSaveToFile({name}) failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "ParticlePresetLoadFromFile", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _ParticlePresetLoadFromFile([MarshalAs(UnmanagedType.LPStr)] string path);
    /// <summary>Load a .particle.json into the preset system so its name
    /// becomes resolvable by PresetList / PresetApplyToConfig.</summary>
    public static bool ParticlePresetLoadFromFile(string path)
    {
        if (!_isLoaded) return false;
        try { return _ParticlePresetLoadFromFile(path) != 0; }
        catch (Exception ex) { Logger.Error($"ParticlePresetLoadFromFile({path}) failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "ParticlePresetApplyToConfig", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _ParticlePresetApplyToConfig(
        [MarshalAs(UnmanagedType.LPStr)] string name, byte[] config, uint size);
    /// <summary>Fill a fresh emitter-config blob with the named preset's
    /// scalar fields (defaults + preset). Returns false for unknown names.</summary>
    public static bool ParticlePresetApplyToConfig(string name, byte[] config)
    {
        if (!_isLoaded || config == null) return false;
        try { return _ParticlePresetApplyToConfig(name, config, (uint)config.Length) != 0; }
        catch (Exception ex) { Logger.Error($"ParticlePresetApplyToConfig({name}) failed", ex); return false; }
    }

    public static uint GIVolumeOverrideCount()
    {
        if (!_isLoaded) return 0;
        try { return _GIGetVolumeOverrideCount(); }
        catch (Exception ex) { Logger.Error("GIGetVolumeOverrideCount failed", ex); return 0; }
    }

    public static bool GIGetVolumeOverride(uint index, byte[] buffer)
    {
        if (!_isLoaded || buffer == null || buffer.Length < GIVolumeConfigCapacity) return false;
        try { return _GIGetVolumeOverride(index, buffer) != 0; }
        catch (Exception ex) { Logger.Error($"GIGetVolumeOverride({index}) failed", ex); return false; }
    }

    public static bool GISetVolumeOverride(uint index, byte[] buffer)
    {
        if (!_isLoaded || buffer == null || buffer.Length < GIVolumeConfigCapacity) return false;
        try { return _GISetVolumeOverride(index, buffer) != 0; }
        catch (Exception ex) { Logger.Error($"GISetVolumeOverride({index}) failed", ex); return false; }
    }

    public static bool GIAppendVolumeOverride()
    {
        if (!_isLoaded) return false;
        try { return _GIAppendVolumeOverride() != 0; }
        catch (Exception ex) { Logger.Error("GIAppendVolumeOverride failed", ex); return false; }
    }

    public static bool GIRemoveVolumeOverride(uint index)
    {
        if (!_isLoaded) return false;
        try { return _GIRemoveVolumeOverride(index) != 0; }
        catch (Exception ex) { Logger.Error($"GIRemoveVolumeOverride({index}) failed", ex); return false; }
    }

    public static bool GIMaterializeDerivedVolumes()
    {
        if (!_isLoaded) return false;
        try { return _GIMaterializeDerivedVolumes() != 0; }
        catch (Exception ex) { Logger.Error("GIMaterializeDerivedVolumes failed", ex); return false; }
    }

    // GI gizmo (volume select/translate/scale). Modes: 1=Select, 2=Translate,
    // 3=Scale. Translate writes origin_override (+use flag); Scale writes
    // probe_count[axis] clamped to 4..128 — both flip settings_dirty_ and the
    // pipeline rebuilds Lumen passes next frame.
    [DllImport("PrimalEngine", EntryPoint = "GIGizmoSetMode", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _GIGizmoSetMode(uint mode);
    public static void GIGizmoSetMode(uint mode)
    {
        if (!_isLoaded) return;
        try { _GIGizmoSetMode(mode); }
        catch (Exception ex) { Logger.Error("GIGizmoSetMode failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "GIGizmoGetSelectedVolume", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GIGizmoGetSelectedVolume();
    public static uint GIGizmoGetSelectedVolume()
    {
        if (!_isLoaded) return 0xFFFFFFFFu;
        try { return _GIGizmoGetSelectedVolume(); }
        catch (Exception ex) { Logger.Error("GIGizmoGetSelectedVolume failed", ex); return 0xFFFFFFFFu; }
    }

    [DllImport("PrimalEngine", EntryPoint = "GIGizmoTranslate", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GIGizmoTranslate(uint index, float[] delta);
    public static bool GIGizmoTranslate(uint index, float dx, float dy, float dz)
    {
        if (!_isLoaded) return false;
        try { return _GIGizmoTranslate(index, new[] { dx, dy, dz }) != 0; }
        catch (Exception ex) { Logger.Error("GIGizmoTranslate failed", ex); return false; }
    }

    [DllImport("PrimalEngine", EntryPoint = "GIGizmoScale", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GIGizmoScale(uint index, float factor, uint axis);
    public static bool GIGizmoScale(uint index, float factor, uint axis)
    {
        if (!_isLoaded) return false;
        try { return _GIGizmoScale(index, factor, axis) != 0; }
        catch (Exception ex) { Logger.Error("GIGizmoScale failed", ex); return false; }
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

    [DllImport("PrimalEngine", EntryPoint = "SetMaterialTechnique", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialTechnique(UInt32 entityId, UInt32 technique);

    [DllImport("PrimalEngine", EntryPoint = "SetMaterialRoughness", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialRoughness(UInt32 entityId, float value);

    [DllImport("PrimalEngine", EntryPoint = "SetMaterialMetallic", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialMetallic(UInt32 entityId, float value);

    [DllImport("PrimalEngine", EntryPoint = "SetMaterialAlphaCutoff", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialAlphaCutoff(UInt32 entityId, float value);

    [DllImport("PrimalEngine", EntryPoint = "SetMaterialBaseColor", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialBaseColor(UInt32 entityId, float[] rgba);

    [DllImport("PrimalEngine", EntryPoint = "SetMaterialAlbedoTexture", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialAlbedoTexture(UInt32 entityId, UInt64 textureId);

    [DllImport("PrimalEngine", EntryPoint = "SetMaterialNormalTexture", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialNormalTexture(UInt32 entityId, UInt64 textureId);

    [DllImport("PrimalEngine", EntryPoint = "SetMaterialOrmTexture", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetMaterialOrmTexture(UInt32 entityId, UInt64 textureId);

    public static void SetMaterialTechnique(uint entityId, uint technique)
    {
        if (!_isLoaded) return;
        try { _SetMaterialTechnique(entityId, technique); }
        catch (Exception ex) { Logger.Error($"SetMaterialTechnique({entityId}) failed", ex); }
    }

    public static void SetMaterialRoughness(uint entityId, float value)
    {
        if (!_isLoaded) return;
        try { _SetMaterialRoughness(entityId, value); }
        catch (Exception ex) { Logger.Error($"SetMaterialRoughness({entityId}) failed", ex); }
    }

    public static void SetMaterialMetallic(uint entityId, float value)
    {
        if (!_isLoaded) return;
        try { _SetMaterialMetallic(entityId, value); }
        catch (Exception ex) { Logger.Error($"SetMaterialMetallic({entityId}) failed", ex); }
    }

    public static void SetMaterialAlphaCutoff(uint entityId, float value)
    {
        if (!_isLoaded) return;
        try { _SetMaterialAlphaCutoff(entityId, value); }
        catch (Exception ex) { Logger.Error($"SetMaterialAlphaCutoff({entityId}) failed", ex); }
    }

    public static void SetMaterialBaseColor(uint entityId, float r, float g, float b, float a)
    {
        if (!_isLoaded) return;
        try { _SetMaterialBaseColor(entityId, new[] { r, g, b, a }); }
        catch (Exception ex) { Logger.Error($"SetMaterialBaseColor({entityId}) failed", ex); }
    }

    public static void SetMaterialAlbedoTexture(uint entityId, ulong textureId)
    {
        if (!_isLoaded) return;
        try { _SetMaterialAlbedoTexture(entityId, textureId); }
        catch (Exception ex) { Logger.Error($"SetMaterialAlbedoTexture({entityId}) failed", ex); }
    }

    public static void SetMaterialNormalTexture(uint entityId, ulong textureId)
    {
        if (!_isLoaded) return;
        try { _SetMaterialNormalTexture(entityId, textureId); }
        catch (Exception ex) { Logger.Error($"SetMaterialNormalTexture({entityId}) failed", ex); }
    }

    public static void SetMaterialOrmTexture(uint entityId, ulong textureId)
    {
        if (!_isLoaded) return;
        try { _SetMaterialOrmTexture(entityId, textureId); }
        catch (Exception ex) { Logger.Error($"SetMaterialOrmTexture({entityId}) failed", ex); }
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

    /// <summary>Per-entity world-space AABB (plan §6.1 A1). Mesh-bearing
    /// entities return the render proxy's worldAABB; entities without a
    /// render proxy (lights, empties) get a half-meter cube around their
    /// position so they stay clickable.</summary>
    [DllImport("PrimalEngine", EntryPoint = "EntityGetWorldAABB", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _EntityGetWorldAABB(uint entityId, [Out] float[] outMin, [Out] float[] outMax);
    public static bool TryGetEntityWorldAABB(uint entityId,
        out (float minX, float minY, float minZ, float maxX, float maxY, float maxZ) bounds)
    {
        bounds = default;
        if (!_isLoaded || entityId == 0 || entityId == InvalidEntityId) return false;
        try
        {
            float[] min = new float[3], max = new float[3];
            if (_EntityGetWorldAABB(entityId, min, max) == 0) return false;
            bounds = (min[0], min[1], min[2], max[0], max[1], max[2]);
            return true;
        }
        catch (Exception ex) { Logger.Error($"EntityGetWorldAABB({entityId:X}) failed", ex); return false; }
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

    // Live light-edit setters (LightAPI.cpp) — let Inspector changes reach
    // the renderer without a full scene resync. light_type: 0=directional,
    // 1=point, 2=spot (mirrors the C# LightType enum).
    [DllImport("PrimalEngine", EntryPoint = "SetEntityLightType", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SetEntityLightType(uint entityId, uint lightType);
    public static void SetEntityLightType(uint entityId, uint lightType)
    {
        if (!_isLoaded) return;
        try { _SetEntityLightType(entityId, lightType); } catch (Exception ex) { Logger.Error("SetEntityLightType failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "SetEntityLightIntensity", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SetEntityLightIntensity(uint entityId, float intensity);
    public static void SetEntityLightIntensity(uint entityId, float intensity)
    {
        if (!_isLoaded) return;
        try { _SetEntityLightIntensity(entityId, intensity); } catch (Exception ex) { Logger.Error("SetEntityLightIntensity failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "SetEntityLightColor", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SetEntityLightColor(uint entityId, float[] colorRgb);
    public static void SetEntityLightColor(uint entityId, float[] colorRgb)
    {
        if (!_isLoaded || colorRgb == null) return;
        try { _SetEntityLightColor(entityId, colorRgb); } catch (Exception ex) { Logger.Error("SetEntityLightColor failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "SetEntityLightRange", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SetEntityLightRange(uint entityId, float range);
    public static void SetEntityLightRange(uint entityId, float range)
    {
        if (!_isLoaded) return;
        try { _SetEntityLightRange(entityId, range); } catch (Exception ex) { Logger.Error("SetEntityLightRange failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "SetEntityLightConeAngles", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SetEntityLightConeAngles(uint entityId, float umbraRad, float penumbraRad);
    public static void SetEntityLightConeAngles(uint entityId, float umbraRad, float penumbraRad)
    {
        if (!_isLoaded) return;
        try { _SetEntityLightConeAngles(entityId, umbraRad, penumbraRad); } catch (Exception ex) { Logger.Error("SetEntityLightConeAngles failed", ex); }
    }

    [DllImport("PrimalEngine", EntryPoint = "SetEntityLightEnabled", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SetEntityLightEnabled(uint entityId, uint isEnabled);
    public static void SetEntityLightEnabled(uint entityId, bool isEnabled)
    {
        if (!_isLoaded) return;
        try { _SetEntityLightEnabled(entityId, isEnabled ? 1u : 0u); } catch (Exception ex) { Logger.Error("SetEntityLightEnabled failed", ex); }
    }

    // Full-state convenience push used by live Inspector edits — writes every
    // light parameter so callers don't need to track which field changed.
    // ===== 诊断（CLI 规划 §11/§12 L2/L3）=====

    [DllImport("PrimalEngine", EntryPoint = "SetLogFile", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetLogFile(string? path, uint maxBytes, uint maxFiles);

    /// <summary>启用引擎 FileSink（8MiB × maxFiles 轮转）。path=null 关闭。</summary>
    public static void SetLogFile(string? path, uint maxBytes = 0, uint maxFiles = 0)
    {
        if (!_isLoaded) return;
        try { _SetLogFile(path, maxBytes, maxFiles); } catch { /* 旧 dylib 无导出 */ }
    }

    [DllImport("PrimalEngine", EntryPoint = "CopyRecentLogs", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CopyRecentLogs(IntPtr buffer, uint bufferSize);

    /// <summary>读取引擎黑匣子（最近 2048 条，每行 "HH:MM:SS.mmm [LEVEL][category] message"）。
    /// 空返回 null（引擎未加载或缓冲为空）。</summary>
    public static string? CopyRecentLogs()
    {
        if (!_isLoaded) return null;
        try
        {
            uint needed = _CopyRecentLogs(IntPtr.Zero, 0);
            if (needed <= 1) return string.Empty;
            var buffer = System.Runtime.InteropServices.Marshal.AllocHGlobal((int)needed);
            try
            {
                if (_CopyRecentLogs(buffer, needed) == 0) return string.Empty;
                return System.Runtime.InteropServices.Marshal.PtrToStringUTF8(buffer);
            }
            finally { System.Runtime.InteropServices.Marshal.FreeHGlobal(buffer); }
        }
        catch (Exception ex)
        {
            // 桥接失败必须可见（旧 dylib 缺导出只是其中一种原因），不再静默吞掉。
            Console.Error.WriteLine($"[NativeEngineBridge] CopyRecentLogs failed: {ex.Message}");
            return null;
        }
    }

    [DllImport("PrimalEngine", EntryPoint = "FlushLogQueue", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _FlushLogQueue();

    [DllImport("PrimalEngine", EntryPoint = "SetCategoryFilter", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _SetCategoryFilter(string? commaSeparatedCategories);

    /// <summary>设置日志类别白名单（逗号分隔，如 "RHI,Asset"）；null/空 = 不过滤。
    /// 对全部 sink 生效（stderr/文件/黑匣子/事件表）。</summary>
    public static void SetCategoryFilter(string? commaSeparatedCategories)
    {
        if (!_isLoaded) return;
        try { _SetCategoryFilter(commaSeparatedCategories); } catch { }
    }

    /// <summary>同步排空后台日志队列（FileSink/回调/事件表送达保证）。</summary>
    public static void FlushLogQueue()
    {
        if (!_isLoaded) return;
        try { _FlushLogQueue(); } catch { }
    }

    public static void SetEntityLight(uint entityId, uint lightType, float intensity, float[] colorRgb, float range, float umbraRad, float penumbraRad, bool isEnabled)
    {
        if (!_isLoaded) return;
        SetEntityLightType(entityId, lightType);
        SetEntityLightIntensity(entityId, intensity);
        SetEntityLightColor(entityId, colorRgb);
        SetEntityLightRange(entityId, range);
        SetEntityLightConeAngles(entityId, umbraRad, penumbraRad);
        SetEntityLightEnabled(entityId, isEnabled);
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
            else
            {
                TrackSurface(result);
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
            else
            {
                TrackSurface(result);
            }
            return result;
        }
        catch (Exception ex)
        {
            Logger.Error("CreateEmbeddedRenderSurface failed", ex);
            return uint.MaxValue;
        }
    }

    // 引擎侧失败值约定 (uint.MaxValue)。见 CreateRenderSurface 等的返回契约。
    public const uint InvalidRenderSurfaceId = uint.MaxValue;
    private static readonly HashSet<uint> LiveRenderSurfaces = new();

    public static void RemoveRenderSurface(uint id)
    {
        if (!_isLoaded) return;
        // 引擎侧 RemoveRenderSurface 对失效 id 会 assert(id < surfaces.size())
        // 直接 SIGABRT。登记表保证同一 id 只会真正下发一次:重复销毁
        // (视口重试路径 + 窗口关闭 + 退出清理的竞态)在托管侧拦下并记日志。
        if (id == InvalidRenderSurfaceId || !LiveRenderSurfaces.Remove(id))
        {
            Logger.Warning($"RemoveRenderSurface: id {id} is not a live surface (double-remove?); skipped");
            return;
        }
        try { _RemoveRenderSurface(id); }
        catch (Exception ex) { Logger.Error("RemoveRenderSurface failed", ex); }
    }

    private static void TrackSurface(uint id)
    {
        if (id != InvalidRenderSurfaceId) LiveRenderSurfaces.Add(id);
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

    [DllImport("PrimalEngine", EntryPoint = "ImportCookedTexture", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern ulong _ImportCookedTexture(IntPtr path);

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

    public static ulong ImportCookedTexture(string path)
    {
        if (!_isLoaded || string.IsNullOrWhiteSpace(path)) return InvalidContentId;
        try { return _ImportCookedTexture(MarshalUtf8(path)); }
        catch (Exception ex) { Logger.Error("ImportCookedTexture failed", ex); return InvalidContentId; }
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

    // =======================================================================
    // A6 PCG 图编辑器绑定（EngineDLL/PCGAPI.cpp，单全局图实例）。
    // =======================================================================
    public static class Pcg
    {
        public sealed class ParamInfo
        {
            public string Name = "";
            public string Group = "";
            public byte Type;          // PCGParamType
            public float Min, Max, Step;
            public string? EnumNames;  // comma-separated labels or null
        }

        public sealed class PinInfo
        {
            public string Name = "";
            public uint Index;
            public byte Type;          // PCGDataType
            public bool IsInput;
        }

        private static bool _ready;

        // 单全局图：编辑器打开时 Create+Deserialize；打开另一个 .pcg 前 Destroy。
        public static void CreateGraph()
        {
            if (!_isLoaded) return;
            try { _PcgCreateGraph(); _ready = true; }
            catch (Exception ex) { Logger.Error("PCGCreateGraph failed", ex); }
        }

        public static void DestroyGraph()
        {
            if (!_isLoaded) return;
            try { _PcgDestroyGraph(); _ready = false; }
            catch (Exception ex) { Logger.Error("PCGDestroyGraph failed", ex); }
        }

        /// <returns>node id (index), 0xFFFFFFFF on failure.</returns>
        public static uint AddNode(string typeName)
        {
            if (!_isLoaded || !_ready) return 0xFFFFFFFF;
            try { return _PcgAddNode(typeName); }
            catch (Exception ex) { Logger.Error("PCGAddNode failed", ex); return 0xFFFFFFFF; }
        }

        public static void RemoveNode(uint nodeId)
        {
            if (!_isLoaded) return;
            try { _PcgRemoveNode(nodeId); }
            catch (Exception ex) { Logger.Error("PCGRemoveNode failed", ex); }
        }

        public static void Connect(uint fromNode, uint fromPin, uint toNode, uint toPin)
        {
            if (!_isLoaded) return;
            try { _PcgConnect(fromNode, fromPin, toNode, toPin); }
            catch (Exception ex) { Logger.Error("PCGConnect failed", ex); }
        }

        public static void Disconnect(uint fromNode, uint fromPin, uint toNode, uint toPin)
        {
            if (!_isLoaded) return;
            try { _PcgDisconnect(fromNode, fromPin, toNode, toPin); }
            catch (Exception ex) { Logger.Error("PCGDisconnect failed", ex); }
        }

        public static bool DeserializeGraph(string json)
        {
            if (!_isLoaded || !_ready) return false;
            try { return _PcgDeserializeGraph(json) != 0; }
            catch (Exception ex) { Logger.Error("PCGDeserializeGraph failed", ex); return false; }
        }

        public static string? SerializeGraph()
        {
            if (!_isLoaded || !_ready) return null;
            try
            {
                var need = _PcgSerializeGraph(null, 0);
                if (need == 0) return null;
                var buf = new byte[need];
                _PcgSerializeGraph(buf, (uint)need);
                return System.Text.Encoding.UTF8.GetString(buf, 0, (int)need - 1);
            }
            catch (Exception ex) { Logger.Error("PCGSerializeGraph failed", ex); return null; }
        }

        public static uint GetRegisteredNodeTypeCount()
        {
            if (!_isLoaded) return 0;
            try { return _PcgGetRegisteredNodeTypeCount(); }
            catch { return 0; }
        }

        public static string? GetRegisteredNodeTypeName(uint index)
        {
            if (!_isLoaded) return null;
            try { return MarshalString(_PcgGetRegisteredNodeTypeName(index)); }
            catch { return null; }
        }

        public static ParamInfo? GetParamInfo(uint nodeId, uint paramIdx)
        {
            if (!_isLoaded) return null;
            try
            {
                var ok = _PcgGetNodeParamDescriptorFlat(nodeId, paramIdx,
                    out var name, out var group, out var type,
                    out var min, out var max, out var step, out var enums);
                return ok == 0 ? null : new ParamInfo
                {
                    Name = MarshalString(name) ?? "",
                    Group = MarshalString(group) ?? "",
                    Type = type,
                    Min = min, Max = max, Step = step,
                    EnumNames = MarshalString(enums),
                };
            }
            catch (Exception ex) { Logger.Error("PCGGetNodeParamDescriptorFlat failed", ex); return null; }
        }

        public static PinInfo? GetPinInfo(uint nodeId, uint pinIdx)
        {
            if (!_isLoaded) return null;
            try
            {
                var ok = _PcgGetNodePinDescriptorFlat(nodeId, pinIdx,
                    out var name, out var index, out var type, out var isInput);
                return ok == 0 ? null : new PinInfo
                {
                    Name = MarshalString(name) ?? "",
                    Index = index,
                    Type = type,
                    IsInput = isInput != 0,
                };
            }
            catch (Exception ex) { Logger.Error("PCGGetNodePinDescriptorFlat failed", ex); return null; }
        }

        public static bool SetParamFloat(uint nodeId, string name, float value)
        {
            if (!_isLoaded) return false;
            try { return _PcgSetNodeParamFloat(nodeId, name, value) != 0; }
            catch (Exception ex) { Logger.Error("PCGSetNodeParamFloat failed", ex); return false; }
        }

        public static bool SetParamVec3(uint nodeId, string name, float x, float y, float z)
        {
            if (!_isLoaded) return false;
            try { return _PcgSetNodeParamVec3(nodeId, name, x, y, z) != 0; }
            catch (Exception ex) { Logger.Error("PCGSetParamVec3 failed", ex); return false; }
        }

        public static bool SetParamArray(uint nodeId, string name, float[] values)
        {
            if (!_isLoaded || values.Length == 0) return false;
            try { return _PcgSetNodeParamArray(nodeId, name, values, (uint)values.Length) != 0; }
            catch (Exception ex) { Logger.Error("PCGSetParamArray failed", ex); return false; }
        }

        public static void Execute()
        {
            if (!_isLoaded || !_ready) return;
            try { _PcgExecute(); }
            catch (Exception ex) { Logger.Error("PCGExecute failed", ex); }
        }

        public static uint GetOutputPointCount(uint nodeId)
        {
            if (!_isLoaded) return 0;
            try { return _PcgGetOutputPointCount(nodeId); }
            catch { return 0; }
        }

        public static uint GetErrorCount()
        {
            if (!_isLoaded) return 0;
            try { return _PcgGetErrorCount(); }
            catch { return 0; }
        }

        public static string? GetErrorMessage(uint index)
        {
            if (!_isLoaded) return null;
            try { return MarshalString(_PcgGetErrorMessage(index)); }
            catch { return null; }
        }

        private static string? MarshalString(IntPtr ptr)
        {
            if (ptr == IntPtr.Zero) return null;
            return System.Runtime.InteropServices.Marshal.PtrToStringAnsi(ptr);
        }

        // ---- A6 Apply: output readback + mesh library -------------------

        /// <summary>Output point count of the node (0 when none).</summary>
        public static uint GetNodeOutputPointCount(uint nodeId) => GetOutputPointCount(nodeId);

        /// <summary>XYZ triples (3 floats per point). Returns empty on failure.</summary>
        public static float[] GetOutputPositions(uint nodeId, uint maxCount)
        {
            if (!_isLoaded || maxCount == 0) return Array.Empty<float>();
            try
            {
                var buf = new float[maxCount * 3];
                var n = _PcgGetOutputPositions(nodeId, buf, maxCount);
                if (n == 0) return Array.Empty<float>();
                var result = new float[n * 3];
                Array.Copy(buf, result, n * 3);
                return result;
            }
            catch (Exception ex) { Logger.Error("PCGGetOutputPositions failed", ex); return Array.Empty<float>(); }
        }

        /// <summary>Per-point mesh slot index (u32 each).</summary>
        public static uint[] GetOutputMeshSlots(uint nodeId, uint maxCount)
        {
            if (!_isLoaded || maxCount == 0) return Array.Empty<uint>();
            try
            {
                var buf = new uint[maxCount];
                var n = _PcgGetOutputMeshSlots(nodeId, buf, maxCount);
                if (n == 0) return Array.Empty<uint>();
                var result = new uint[n];
                Array.Copy(buf, result, n);
                return result;
            }
            catch (Exception ex) { Logger.Error("PCGGetOutputMeshSlots failed", ex); return Array.Empty<uint>(); }
        }

        /// <summary>Per-point {scaleX, scaleY, scaleZ, rotY} (4 floats each).</summary>
        public static float[] GetOutputTransforms(uint nodeId, uint maxCount)
        {
            if (!_isLoaded || maxCount == 0) return Array.Empty<float>();
            try
            {
                var buf = new float[maxCount * 4];
                var n = _PcgGetOutputTransforms(nodeId, buf, maxCount);
                if (n == 0) return Array.Empty<float>();
                var result = new float[n * 4];
                Array.Copy(buf, result, n * 4);
                return result;
            }
            catch (Exception ex) { Logger.Error("PCGGetOutputTransforms failed", ex); return Array.Empty<float>(); }
        }

        public static uint GetMeshSlotCount()
        {
            if (!_isLoaded) return 0;
            try { return _PcgGetMeshSlotCount(); }
            catch { return 0; }
        }

        /// <summary>Mesh library slot (path, display name).</summary>
        public static (string Path, string Name)? GetMeshSlot(uint index)
        {
            if (!_isLoaded) return null;
            try
            {
                if (_PcgGetMeshSlotFlat(index, out var path, out var name) == 0) return null;
                return (MarshalString(path) ?? "", MarshalString(name) ?? "");
            }
            catch (Exception ex) { Logger.Error("PCGGetMeshSlotFlat failed", ex); return null; }
        }

        [DllImport("PrimalEngine", EntryPoint = "PCGGetOutputPositions", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgGetOutputPositions(uint nodeId, float[] outPositionsXyz, uint maxCount);
        [DllImport("PrimalEngine", EntryPoint = "PCGGetOutputMeshSlots", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgGetOutputMeshSlots(uint nodeId, uint[] outMeshSlots, uint maxCount);
        [DllImport("PrimalEngine", EntryPoint = "PCGGetOutputTransforms", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgGetOutputTransforms(uint nodeId, float[] outScaleXyzRotY, uint maxCount);
        [DllImport("PrimalEngine", EntryPoint = "PCGGetMeshSlotCount", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgGetMeshSlotCount();
        [DllImport("PrimalEngine", EntryPoint = "PCGGetMeshSlotFlat", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgGetMeshSlotFlat(uint index, out IntPtr path, out IntPtr name);

        [DllImport("PrimalEngine", EntryPoint = "PCGCreateGraph", CallingConvention = CallingConvention.Cdecl)]
        private static extern void _PcgCreateGraph();
        [DllImport("PrimalEngine", EntryPoint = "PCGDestroyGraph", CallingConvention = CallingConvention.Cdecl)]
        private static extern void _PcgDestroyGraph();
        [DllImport("PrimalEngine", EntryPoint = "PCGAddNode", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgAddNode([MarshalAs(UnmanagedType.LPStr)] string typeName);
        [DllImport("PrimalEngine", EntryPoint = "PCGRemoveNode", CallingConvention = CallingConvention.Cdecl)]
        private static extern void _PcgRemoveNode(uint nodeId);
        [DllImport("PrimalEngine", EntryPoint = "PCGConnect", CallingConvention = CallingConvention.Cdecl)]
        private static extern void _PcgConnect(uint fromNode, uint fromPin, uint toNode, uint toPin);
        [DllImport("PrimalEngine", EntryPoint = "PCGDisconnect", CallingConvention = CallingConvention.Cdecl)]
        private static extern void _PcgDisconnect(uint fromNode, uint fromPin, uint toNode, uint toPin);
        [DllImport("PrimalEngine", EntryPoint = "PCGDeserializeGraph", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgDeserializeGraph([MarshalAs(UnmanagedType.LPStr)] string json);
        [DllImport("PrimalEngine", EntryPoint = "PCGSerializeGraph", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgSerializeGraph(byte[]? buf, uint bufSize);
        [DllImport("PrimalEngine", EntryPoint = "PCGGetRegisteredNodeTypeCount", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgGetRegisteredNodeTypeCount();
        [DllImport("PrimalEngine", EntryPoint = "PCGGetRegisteredNodeTypeName", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr _PcgGetRegisteredNodeTypeName(uint index);
        [DllImport("PrimalEngine", EntryPoint = "PCGGetNodeParamDescriptorFlat", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgGetNodeParamDescriptorFlat(uint nodeId, uint paramIdx,
            out IntPtr name, out IntPtr group, out byte type,
            out float min, out float max, out float step, out IntPtr enumNames);
        [DllImport("PrimalEngine", EntryPoint = "PCGGetNodePinDescriptorFlat", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgGetNodePinDescriptorFlat(uint nodeId, uint pinIdx,
            out IntPtr name, out uint index, out byte type, out byte isInput);
        [DllImport("PrimalEngine", EntryPoint = "PCGSetNodeParamFloat", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgSetNodeParamFloat(uint nodeId, [MarshalAs(UnmanagedType.LPStr)] string name, float value);
        [DllImport("PrimalEngine", EntryPoint = "PCGSetNodeParamVec3", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgSetNodeParamVec3(uint nodeId, [MarshalAs(UnmanagedType.LPStr)] string name, float x, float y, float z);
        [DllImport("PrimalEngine", EntryPoint = "PCGSetNodeParamArray", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgSetNodeParamArray(uint nodeId, [MarshalAs(UnmanagedType.LPStr)] string name, float[] values, uint count);
        [DllImport("PrimalEngine", EntryPoint = "PCGExecute", CallingConvention = CallingConvention.Cdecl)]
        private static extern void _PcgExecute();
        [DllImport("PrimalEngine", EntryPoint = "PCGGetOutputPointCount", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgGetOutputPointCount(uint nodeId);
        [DllImport("PrimalEngine", EntryPoint = "PCGGetErrorCount", CallingConvention = CallingConvention.Cdecl)]
        private static extern uint _PcgGetErrorCount();
        [DllImport("PrimalEngine", EntryPoint = "PCGGetErrorMessage", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr _PcgGetErrorMessage(uint index);
    }
}
