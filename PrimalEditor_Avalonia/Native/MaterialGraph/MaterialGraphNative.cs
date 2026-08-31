using System;
using System.Runtime.InteropServices;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native.MaterialGraph;

/// <summary>
/// P/Invoke bindings for <c>EngineDLL/MaterialGraphAPI.cpp</c> — thin wrapper around
/// <c>material_graph::MaterialGraph</c> for the C# material editor. All ids are
/// 1-based (0 is the invalid sentinel). All functions are no-throw; non-zero
/// returns from Connect/Disconnect/etc. indicate the call did not modify state.
///
/// Naming convention: <c>_C_*</c> = raw <c>[DllImport]</c>, public method wraps
/// it with GCHandle pinning for arrays/strings + exception swallowing, matching
/// the established pattern in <see cref="NativeEngineBridge"/>.
/// </summary>
public static class MaterialGraphNative
{
    private const string Lib = "PrimalEngine";

    // --- POD mirrors of C structs ---

    [StructLayout(LayoutKind.Sequential)]
    public struct MaterialGraphConnection
    {
        public uint FromNode;
        public uint FromPin;
        public uint ToNode;
        public uint ToPin;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MaterialInitInfo
    {
        public uint Technique;
        public float BaseColorR;
        public float BaseColorG;
        public float BaseColorB;
        public float BaseColorA;
        public float Roughness;
        public float Metallic;
        public float AlphaCutoff;
        public ulong AlbedoTexture;
        public ulong NormalTexture;
        public ulong OrmTexture;
    }

    // --- Lifecycle ---

    [DllImport(Lib, EntryPoint = "CreateMaterialGraph", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CreateMaterialGraph();

    [DllImport(Lib, EntryPoint = "DestroyMaterialGraph", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DestroyMaterialGraph(uint graphId);

    [DllImport(Lib, EntryPoint = "DestroyAllMaterialGraphs", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _DestroyAllMaterialGraphs();

    [DllImport(Lib, EntryPoint = "CloneMaterialGraph", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _CloneMaterialGraph(uint srcGraphId);

    public static uint Create() => Safe(() => _CreateMaterialGraph());

    public static void Destroy(uint graphId) => Safe(() => _DestroyMaterialGraph(graphId));

    public static void DestroyAll() => Safe(() => _DestroyAllMaterialGraphs());

    public static uint Clone(uint srcGraphId) => Safe(() => _CloneMaterialGraph(srcGraphId));

    // --- Node CRUD ---

    [DllImport(Lib, EntryPoint = "GraphAddNode", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphAddNode(uint graphId, IntPtr typeName);

    [DllImport(Lib, EntryPoint = "GraphRemoveNode", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphRemoveNode(uint graphId, uint nodeId);

    [DllImport(Lib, EntryPoint = "GraphGetNodeCount", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphGetNodeCount(uint graphId);

    [DllImport(Lib, EntryPoint = "GraphGetNodeIdByIndex", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphGetNodeIdByIndex(uint graphId, uint index);

    [DllImport(Lib, EntryPoint = "GraphNodeGetTypeId", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeGetTypeId(uint graphId, uint nodeId, IntPtr outBuf, ulong bufSize);

    [DllImport(Lib, EntryPoint = "GraphGetNodePosition", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphGetNodePosition(uint graphId, uint nodeId, out float x, out float y);

    [DllImport(Lib, EntryPoint = "GraphSetNodePosition", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphSetNodePosition(uint graphId, uint nodeId, float x, float y);

    public static uint AddNode(uint graphId, string typeName)
    {
        if (string.IsNullOrEmpty(typeName)) return 0;
        return WithUtf8Pinned(typeName, p => Safe(() => _GraphAddNode(graphId, p)));
    }

    public static uint RemoveNode(uint graphId, uint nodeId) =>
        Safe(() => _GraphRemoveNode(graphId, nodeId));

    public static uint GetNodeCount(uint graphId) =>
        Safe(() => _GraphGetNodeCount(graphId));

    public static uint GetNodeIdByIndex(uint graphId, uint index) =>
        Safe(() => _GraphGetNodeIdByIndex(graphId, index));

    public static string? GetNodeTypeName(uint graphId, uint nodeId, int bufSize = 64)
    {
        var buffer = new byte[bufSize];
        var pin = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        try
        {
            var ok = Safe(() => _GraphNodeGetTypeId(graphId, nodeId, pin.AddrOfPinnedObject(), (ulong)bufSize));
            return ok == 0 ? ReadUtf8String(buffer) : null;
        }
        finally { pin.Free(); }
    }

    /// <summary>
    /// Read the editor-canvas position this node was last saved with.
    /// Returns true on success; <paramref name="x"/> / <paramref name="y"/>
    /// are 0 when the node was never positioned (legacy sidecars, pre-
    /// position serialization or a freshly-created node).
    /// </summary>
    public static bool TryGetNodePosition(uint graphId, uint nodeId, out float x, out float y)
    {
        x = 0f; y = 0f;
        try { return _GraphGetNodePosition(graphId, nodeId, out x, out y) == 0; }
        catch (DllNotFoundException) { return false; }
        catch (Exception ex) { Logger.Warning($"MaterialGraphNative.TryGetNodePosition: {ex.Message}"); return false; }
    }

    /// <summary>
    /// Persist the editor-canvas position for <paramref name="nodeId"/>.
    /// Called from the node-drag handler so the next Save writes the
    /// layout exactly where the user dropped it.
    /// </summary>
    public static bool SetNodePosition(uint graphId, uint nodeId, float x, float y)
    {
        try { return _GraphSetNodePosition(graphId, nodeId, x, y) == 0; }
        catch (DllNotFoundException) { return false; }
        catch (Exception ex) { Logger.Warning($"MaterialGraphNative.SetNodePosition: {ex.Message}"); return false; }
    }

    // --- Connection CRUD ---

    [DllImport(Lib, EntryPoint = "GraphConnect", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphConnect(uint graphId, uint fromNode, uint fromPin, uint toNode, uint toPin);

    [DllImport(Lib, EntryPoint = "GraphDisconnect", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphDisconnect(uint graphId, uint fromNode, uint fromPin, uint toNode, uint toPin);

    [DllImport(Lib, EntryPoint = "GraphGetConnectionCount", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphGetConnectionCount(uint graphId);

    [DllImport(Lib, EntryPoint = "GraphGetConnectionByIndex", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphGetConnectionByIndex(uint graphId, uint index, out MaterialGraphConnection conn);

    public static uint Connect(uint graphId, uint fromNode, uint fromPin, uint toNode, uint toPin) =>
        Safe(() => _GraphConnect(graphId, fromNode, fromPin, toNode, toPin));

    public static uint Disconnect(uint graphId, uint fromNode, uint fromPin, uint toNode, uint toPin) =>
        Safe(() => _GraphDisconnect(graphId, fromNode, fromPin, toNode, toPin));

    public static uint GetConnectionCount(uint graphId) =>
        Safe(() => _GraphGetConnectionCount(graphId));

    public static bool TryGetConnection(uint graphId, uint index, out MaterialGraphConnection conn)
    {
        conn = default;
        try { return _GraphGetConnectionByIndex(graphId, index, out conn) == 0; }
        catch (DllNotFoundException) { return false; }
        catch (Exception ex) { Logger.Warning($"MaterialGraphNative call failed: {ex.Message}"); return false; }
    }

    // --- Bulk / Execute / Errors ---

    [DllImport(Lib, EntryPoint = "GraphClear", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _GraphClear(uint graphId);

    [DllImport(Lib, EntryPoint = "GraphExecute", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphExecute(uint graphId);

    [DllImport(Lib, EntryPoint = "GraphGetErrorCount", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphGetErrorCount(uint graphId);

    [DllImport(Lib, EntryPoint = "GraphGetErrorByIndex", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphGetErrorByIndex(uint graphId, uint index, out uint outNodeId, IntPtr outMsg, ulong bufSize);

    [DllImport(Lib, EntryPoint = "GraphClearErrors", CallingConvention = CallingConvention.Cdecl)]
    private static extern void _GraphClearErrors(uint graphId);

    public static void Clear(uint graphId) => Safe(() => _GraphClear(graphId));

    public static uint Execute(uint graphId) => Safe(() => _GraphExecute(graphId));

    public static uint GetErrorCount(uint graphId) => Safe(() => _GraphGetErrorCount(graphId));

    public static bool TryGetError(uint graphId, uint index, int msgBufSize, out uint nodeId, out string? message)
    {
        nodeId = 0;
        message = null;
        var buffer = new byte[msgBufSize];
        var pin = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        try
        {
            uint ok;
            try { ok = _GraphGetErrorByIndex(graphId, index, out nodeId, pin.AddrOfPinnedObject(), (ulong)msgBufSize); }
            catch (DllNotFoundException) { ok = 1; }
            catch (Exception ex) { Logger.Warning($"MaterialGraphNative call failed: {ex.Message}"); ok = 1; }
            if (ok != 0) return false;
            message = ReadUtf8String(buffer);
            return true;
        }
        finally { pin.Free(); }
    }

    public static void ClearErrors(uint graphId) => Safe(() => _GraphClearErrors(graphId));

    // --- JSON / File IO ---

    [DllImport(Lib, EntryPoint = "GraphGetJsonSize", CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong _GraphGetJsonSize(uint graphId);

    [DllImport(Lib, EntryPoint = "GraphGetJson", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphGetJson(uint graphId, IntPtr outBuf, ulong bufSize);

    [DllImport(Lib, EntryPoint = "GraphLoadFromJson", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphLoadFromJson(uint graphId, IntPtr json);

    [DllImport(Lib, EntryPoint = "LoadMaterialGraphFromFile", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _LoadMaterialGraphFromFile(IntPtr path);

    [DllImport(Lib, EntryPoint = "SaveMaterialGraphToFile", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _SaveMaterialGraphToFile(uint graphId, IntPtr path);

    public static string? SerializeJson(uint graphId)
    {
        var size = Safe(() => _GraphGetJsonSize(graphId));
        if (size == 0) return null;
        var buffer = new byte[(int)size];
        var pin = GCHandle.Alloc(buffer, GCHandleType.Pinned);
        try
        {
            var ok = Safe(() => _GraphGetJson(graphId, pin.AddrOfPinnedObject(), size));
            return ok == 0 ? System.Text.Encoding.UTF8.GetString(buffer, 0, buffer.Length - 1) : null;
        }
        finally { pin.Free(); }
    }

    public static bool LoadFromJson(uint graphId, string json)
    {
        if (string.IsNullOrEmpty(json)) return false;
        return WithUtf8Pinned(json, p => Safe(() => _GraphLoadFromJson(graphId, p)) == 0);
    }

    public static uint LoadFromFile(string path)
    {
        if (string.IsNullOrEmpty(path)) return 0;
        return WithUtf8Pinned(path, p => Safe(() => _LoadMaterialGraphFromFile(p)));
    }

    public static bool SaveToFile(uint graphId, string path)
    {
        if (string.IsNullOrEmpty(path)) return false;
        return WithUtf8Pinned(path, p => Safe(() => _SaveMaterialGraphToFile(graphId, p)) == 0);
    }

    // --- Bridge: graph -> material init_info ---

    [DllImport(Lib, EntryPoint = "GraphEvaluateToMaterialInfo", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphEvaluateToMaterialInfo(uint graphId, out MaterialInitInfo info);

    public static bool TryEvaluate(uint graphId, out MaterialInitInfo info)
    {
        info = default;
        try { return _GraphEvaluateToMaterialInfo(graphId, out info) == 0; }
        catch (DllNotFoundException) { return false; }
        catch (Exception ex) { Logger.Warning($"MaterialGraphNative call failed: {ex.Message}"); return false; }
    }

    // --- Raw pointer accessor (for ShaderCompiler) ---

    [DllImport(Lib, EntryPoint = "GetMaterialGraphRawPtr", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr _GetMaterialGraphRawPtr(uint graphId);

    /// <summary>
    /// Returns the raw native <c>MaterialGraph*</c> for passing into
    /// <see cref="ShaderCompilerNative.Compile"/>, which takes <c>const void*</c>.
    /// The pointer is owned by the engine — do not free.
    /// </summary>
    public static IntPtr GetRawPtr(uint graphId) => Safe(() => _GetMaterialGraphRawPtr(graphId));

    // --- helpers ---

    private static void Safe(Action a)
    {
        try { a(); } catch (DllNotFoundException) { /* engine not loaded — silent */ }
        catch (Exception ex) { Logger.Warning($"MaterialGraphNative call failed: {ex.Message}"); }
    }

    private static T Safe<T>(Func<T> f)
    {
        try { return f(); }
        catch (DllNotFoundException) { return default!; }
        catch (Exception ex)
        {
            Logger.Warning($"MaterialGraphNative call failed: {ex.Message}");
            return default!;
        }
    }

    private static T WithUtf8Pinned<T>(string s, Func<IntPtr, T> f)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(s + '\0');
        var pin = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try { return f(pin.AddrOfPinnedObject()); }
        finally { pin.Free(); }
    }

    private static string? ReadUtf8String(byte[] buffer)
    {
        var len = 0;
        while (len < buffer.Length && buffer[len] != 0) len++;
        return len == 0 ? string.Empty : System.Text.Encoding.UTF8.GetString(buffer, 0, len);
    }
}