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

    [DllImport("PrimalEngine", EntryPoint = "RemoveRenderSurface", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _RemoveRenderSurface(uint id);

    [DllImport("PrimalEngine", EntryPoint = "GetWindowHandle", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr _GetWindowHandle(uint id);

    [DllImport("PrimalEngine", EntryPoint = "ResizeRenderSurface", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _ResizeRenderSurface(uint id);

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

    public static void ResizeRenderSurface(uint id)
    {
        if (!_isLoaded) return;
        try { _ResizeRenderSurface(id); }
        catch (Exception ex) { Logger.Error("ResizeRenderSurface failed", ex); }
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