using System;
using System.Runtime.InteropServices;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native.MaterialGraph;

/// <summary>
/// P/Invoke bindings for <c>EngineDLL/MaterialGraphReflectionAPI.cpp</c> —
/// node type registry reflection + per-node pin/param queries. Used by the
/// material editor to populate its node palette and parameter panel.
/// </summary>
public static class MaterialGraphReflectionNative
{
    private const string Lib = "PrimalEngine";

    // POD mirrors of C structs

    [StructLayout(LayoutKind.Sequential)]
    public struct MaterialPinDesc
    {
        public uint DataType;
        public uint IsInput;
        public uint Index;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MaterialParamDesc
    {
        public uint Type;
        public float MinVal;
        public float MaxVal;
        public float Step;
        public uint Offset;
        public uint Size;
        public uint EnumNameCount;
    }

    // Registry reflection

    [DllImport(Lib, EntryPoint = "GetRegisteredMaterialNodeTypeCount", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GetRegisteredMaterialNodeTypeCount();

    [DllImport(Lib, EntryPoint = "GetRegisteredMaterialNodeTypeInfo",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GetRegisteredMaterialNodeTypeInfo(
        uint index,
        IntPtr outTypeName,    ulong nameBuf,
        IntPtr outDisplayName, ulong dispBuf,
        IntPtr outCategory,    ulong catBuf,
        out uint outIsTerminal);

    public static uint GetRegisteredTypeCount() => Safe(_GetRegisteredMaterialNodeTypeCount);

    public static bool TryGetRegisteredTypeInfo(
        uint index,
        int nameBuf,
        int dispBuf,
        int catBuf,
        out string typeName,
        out string displayName,
        out string category,
        out bool isTerminal)
    {
        typeName = displayName = category = string.Empty;
        isTerminal = false;
        var nameBytes = new byte[nameBuf];
        var dispBytes = new byte[dispBuf];
        var catBytes  = new byte[catBuf];
        var namePin = GCHandle.Alloc(nameBytes, GCHandleType.Pinned);
        var dispPin = GCHandle.Alloc(dispBytes, GCHandleType.Pinned);
        var catPin  = GCHandle.Alloc(catBytes,  GCHandleType.Pinned);
        try
        {
            uint term = 0;
            uint ok;
            try
            {
                ok = _GetRegisteredMaterialNodeTypeInfo(
                    index,
                    namePin.AddrOfPinnedObject(), (ulong)nameBuf,
                    dispPin.AddrOfPinnedObject(), (ulong)dispBuf,
                    catPin.AddrOfPinnedObject(),  (ulong)catBuf,
                    out term);
            }
            catch (DllNotFoundException) { ok = 0; }
            catch (Exception ex) { Logger.Warning($"MaterialGraphReflectionNative call failed: {ex.Message}"); ok = 0; }
            if (ok == 0) return false;
            typeName = ReadUtf8(nameBytes);
            displayName = ReadUtf8(dispBytes);
            category = ReadUtf8(catBytes);
            isTerminal = term != 0;
            return true;
        }
        finally
        {
            namePin.Free(); dispPin.Free(); catPin.Free();
        }
    }

    // Pin reflection

    [DllImport(Lib, EntryPoint = "GraphNodeGetPinCount", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeGetPinCount(uint graphId, uint nodeId);

    [DllImport(Lib, EntryPoint = "GraphNodeGetPinDescriptor", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeGetPinDescriptor(uint graphId, uint nodeId, uint pinIndex, out MaterialPinDesc desc, IntPtr outName, ulong bufSize);

    public static uint GetPinCount(uint graphId, uint nodeId) =>
        Safe(() => _GraphNodeGetPinCount(graphId, nodeId));

    public static bool TryGetPinDescriptor(uint graphId, uint nodeId, uint pinIndex, int nameBuf, out MaterialPinDesc desc, out string name)
    {
        desc = default;
        name = string.Empty;
        var nameBytes = new byte[nameBuf];
        var namePin = GCHandle.Alloc(nameBytes, GCHandleType.Pinned);
        try
        {
            var local = default(MaterialPinDesc);
            uint ok;
            try { ok = _GraphNodeGetPinDescriptor(graphId, nodeId, pinIndex, out local, namePin.AddrOfPinnedObject(), (ulong)nameBuf); }
            catch (DllNotFoundException) { ok = 0; }
            catch (Exception ex) { Logger.Warning($"MaterialGraphReflectionNative call failed: {ex.Message}"); ok = 0; }
            if (ok == 0) return false;
            desc = local;
            name = ReadUtf8(nameBytes);
            return true;
        }
        finally { namePin.Free(); }
    }

    // Param reflection

    [DllImport(Lib, EntryPoint = "GraphNodeGetParamCount", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeGetParamCount(uint graphId, uint nodeId);

    [DllImport(Lib, EntryPoint = "GraphNodeGetParamDescriptor", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeGetParamDescriptor(uint graphId, uint nodeId, uint paramIndex, out MaterialParamDesc desc, IntPtr outName, ulong nameBuf, IntPtr outGroup, ulong groupBuf);

    public static uint GetParamCount(uint graphId, uint nodeId) =>
        Safe(() => _GraphNodeGetParamCount(graphId, nodeId));

    public static bool TryGetParamDescriptor(uint graphId, uint nodeId, uint paramIndex, int nameBuf, int groupBuf, out MaterialParamDesc desc, out string name, out string group)
    {
        desc = default;
        name = group = string.Empty;
        var nameBytes = new byte[nameBuf];
        var groupBytes = new byte[groupBuf];
        var namePin  = GCHandle.Alloc(nameBytes,  GCHandleType.Pinned);
        var groupPin = GCHandle.Alloc(groupBytes, GCHandleType.Pinned);
        try
        {
            var local = default(MaterialParamDesc);
            uint ok;
            try
            {
                ok = _GraphNodeGetParamDescriptor(graphId, nodeId, paramIndex, out local,
                    namePin.AddrOfPinnedObject(),  (ulong)nameBuf,
                    groupPin.AddrOfPinnedObject(), (ulong)groupBuf);
            }
            catch (DllNotFoundException) { ok = 0; }
            catch (Exception ex) { Logger.Warning($"MaterialGraphReflectionNative call failed: {ex.Message}"); ok = 0; }
            if (ok == 0) return false;
            desc = local;
            name  = ReadUtf8(nameBytes);
            group = ReadUtf8(groupBytes);
            return true;
        }
        finally { namePin.Free(); groupPin.Free(); }
    }

    // Param get/set

    [DllImport(Lib, EntryPoint = "GraphNodeSetParamFloat", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeSetParamFloat(uint g, uint n, IntPtr name, float v);

    [DllImport(Lib, EntryPoint = "GraphNodeSetParamFloat2", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeSetParamFloat2(uint g, uint n, IntPtr name, IntPtr v);

    [DllImport(Lib, EntryPoint = "GraphNodeSetParamFloat3", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeSetParamFloat3(uint g, uint n, IntPtr name, IntPtr v);

    [DllImport(Lib, EntryPoint = "GraphNodeSetParamFloat4", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeSetParamFloat4(uint g, uint n, IntPtr name, IntPtr v);

    [DllImport(Lib, EntryPoint = "GraphNodeSetParamString", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeSetParamString(uint g, uint n, IntPtr name, IntPtr v);

    [DllImport(Lib, EntryPoint = "GraphNodeGetParamFloat", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeGetParamFloat(uint g, uint n, IntPtr name, out float v);

    [DllImport(Lib, EntryPoint = "GraphNodeGetParamFloat4", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeGetParamFloat4(uint g, uint n, IntPtr name, IntPtr v);

    [DllImport(Lib, EntryPoint = "GraphNodeGetParamString", CallingConvention = CallingConvention.Cdecl)]
    private static extern uint _GraphNodeGetParamString(uint g, uint n, IntPtr name, IntPtr outBuf, ulong bufSize);

    public static bool SetParamFloat(uint g, uint n, string name, float v) =>
        WithUtf8(name, np => Safe(() => _GraphNodeSetParamFloat(g, n, np, v)) != 0);

    public static bool SetParamFloat2(uint g, uint n, string name, float x, float y)
    {
        var v = new[] { x, y };
        return WithUtf8(name, np => WithPinned(v, vp => Safe(() => _GraphNodeSetParamFloat2(g, n, np, vp)) != 0));
    }

    public static bool SetParamFloat3(uint g, uint n, string name, float x, float y, float z)
    {
        var v = new[] { x, y, z };
        return WithUtf8(name, np => WithPinned(v, vp => Safe(() => _GraphNodeSetParamFloat3(g, n, np, vp)) != 0));
    }

    public static bool SetParamFloat4(uint g, uint n, string name, float x, float y, float z, float w)
    {
        var v = new[] { x, y, z, w };
        return WithUtf8(name, np => WithPinned(v, vp => Safe(() => _GraphNodeSetParamFloat4(g, n, np, vp)) != 0));
    }

    public static bool SetParamString(uint g, uint n, string name, string v) =>
        WithUtf8(name, np => WithUtf8(v ?? string.Empty, vp => Safe(() => _GraphNodeSetParamString(g, n, np, vp)) != 0));

    public static bool TryGetParamFloat(uint g, uint n, string name, out float v)
    {
        float local = 0;
        var found = WithUtf8(name, np =>
        {
            try { return _GraphNodeGetParamFloat(g, n, np, out local) != 0; }
            catch (DllNotFoundException) { return false; }
            catch (Exception ex) { Logger.Warning($"MaterialGraphReflectionNative call failed: {ex.Message}"); return false; }
        });
        v = local;
        return found;
    }

    public static bool TryGetParamFloat4(uint g, uint n, string name, out float[] v)
    {
        var local = new float[4];
        var found = WithUtf8(name, np => WithPinned(local, vp =>
        {
            try { return _GraphNodeGetParamFloat4(g, n, np, vp) != 0; }
            catch (DllNotFoundException) { return false; }
            catch (Exception ex) { Logger.Warning($"MaterialGraphReflectionNative call failed: {ex.Message}"); return false; }
        }));
        v = local;
        return found;
    }

    public static bool TryGetParamString(uint g, uint n, string name, int bufSize, out string value)
    {
        value = string.Empty;
        var buf = new byte[bufSize];
        var found = WithUtf8(name, np =>
        {
            var pin = GCHandle.Alloc(buf, GCHandleType.Pinned);
            try
            {
                uint ok;
                try { ok = _GraphNodeGetParamString(g, n, np, pin.AddrOfPinnedObject(), (ulong)bufSize); }
                catch (DllNotFoundException) { ok = 0; }
                catch (Exception ex) { Logger.Warning($"MaterialGraphReflectionNative call failed: {ex.Message}"); ok = 0; }
                return ok != 0;
            }
            finally { pin.Free(); }
        });
        if (found) value = ReadUtf8(buf);
        return found;
    }

    private static T Safe<T>(Func<T> f)
    {
        try { return f(); }
        catch (DllNotFoundException) { return default!; }
        catch (Exception ex) { Logger.Warning($"MaterialGraphReflectionNative call failed: {ex.Message}"); return default!; }
    }

    private static T WithUtf8<T>(string s, Func<IntPtr, T> f)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(s + '\0');
        var pin = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try { return f(pin.AddrOfPinnedObject()); }
        finally { pin.Free(); }
    }

    private static T WithPinned<T>(Array arr, Func<IntPtr, T> f)
    {
        var pin = GCHandle.Alloc(arr, GCHandleType.Pinned);
        try { return f(pin.AddrOfPinnedObject()); }
        finally { pin.Free(); }
    }

    private static string ReadUtf8(byte[] buffer)
    {
        var len = 0;
        while (len < buffer.Length && buffer[len] != 0) len++;
        return len == 0 ? string.Empty : System.Text.Encoding.UTF8.GetString(buffer, 0, len);
    }
}
