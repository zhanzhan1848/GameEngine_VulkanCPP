using System;
using System.Collections.Generic;
using System.IO;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native.MaterialGraph;

/// <summary>
/// Managed wrapper that owns a native <c>MaterialGraph</c> handle and
/// exposes a queryable, save/loadable session for the editor's node-graph
/// UI. All mutation goes through the C ABI; this class only caches
/// metadata and handles UTF8 marshalling.
///
/// Lifecycle:
/// <list type="number">
/// <item><see cref="Create"/> → fresh empty graph, or
/// <see cref="LoadFromFile"/> → JSON sidecar.</item>
/// <item>Add nodes / connect pins / set params.</item>
/// <item><see cref="Execute"/> evaluates topology and reports errors via
/// <see cref="GetErrors"/>.</item>
/// <item><see cref="SaveToFile"/> or <see cref="SerializeJson"/> for persistence.</item>
/// <item><see cref="Dispose"/> releases the native slot.</item>
/// </list>
///
/// </summary>
public sealed class MaterialGraphSession : IDisposable
{
    public uint GraphId { get; private set; }

    public bool IsValid => GraphId != 0;

    private MaterialGraphSession(uint graphId)
    {
        GraphId = graphId;
    }

    /// <summary>
    /// Adopt an externally-created native graph handle (e.g. one returned
    /// from <see cref="MaterialGraphNative.LoadFromFile"/>). Internal so
    /// only trusted callers can wrap a foreign handle — the public path
    /// is <see cref="Create"/> / <see cref="LoadFromFile"/>.
    /// </summary>
    internal static MaterialGraphSession Adopt(uint graphId) =>
        graphId == 0 ? throw new InvalidOperationException("Cannot adopt invalid graph id") : new MaterialGraphSession(graphId);

    public static MaterialGraphSession Create()
    {
        var id = MaterialGraphNative.Create();
        return id == 0 ? throw new InvalidOperationException("MaterialGraphNative.Create returned invalid id") : new MaterialGraphSession(id);
    }

    public static MaterialGraphSession? LoadFromFile(string path)
    {
        if (string.IsNullOrEmpty(path)) return null;
        var id = MaterialGraphNative.LoadFromFile(path);
        return id == 0 ? null : new MaterialGraphSession(id);
    }

    // --- Node ops ---

    /// <summary>
    /// Add a node of <paramref name="typeName"/> (e.g. "ConstantFloat3", "Multiply", "MaterialOutput").
    /// Returns the 1-based node id (0 on failure).
    /// </summary>
    public uint AddNode(string typeName) => MaterialGraphNative.AddNode(GraphId, typeName);

    public void RemoveNode(uint nodeId) => MaterialGraphNative.RemoveNode(GraphId, nodeId);

    public uint NodeCount => MaterialGraphNative.GetNodeCount(GraphId);

    public uint GetNodeIdByIndex(uint index) => MaterialGraphNative.GetNodeIdByIndex(GraphId, index);

    public string? GetNodeTypeName(uint nodeId) => MaterialGraphNative.GetNodeTypeName(GraphId, nodeId);

    /// <summary>
    /// Read the editor-canvas position recorded for <paramref name="nodeId"/>.
    /// Returns false when the node isn't positioned (newly created or the
    /// engine dylib isn't loaded); callers should fall back to a default
    /// cascade position in that case.
    /// </summary>
    public bool TryGetNodePosition(uint nodeId, out float x, out float y) =>
        MaterialGraphNative.TryGetNodePosition(GraphId, nodeId, out x, out y);

    /// <summary>
    /// Persist the editor-canvas position for <paramref name="nodeId"/>.
    /// Called from the node-drag handler so the next
    /// <see cref="SaveToFile"/> round-trips the layout exactly where the
    /// user dropped it.
    /// </summary>
    public bool SetNodePosition(uint nodeId, float x, float y) =>
        MaterialGraphNative.SetNodePosition(GraphId, nodeId, x, y);

    // --- Connection ops ---

    public bool Connect(uint fromNode, uint fromPin, uint toNode, uint toPin) =>
        MaterialGraphNative.Connect(GraphId, fromNode, fromPin, toNode, toPin) == 0;

    public bool Disconnect(uint fromNode, uint fromPin, uint toNode, uint toPin) =>
        MaterialGraphNative.Disconnect(GraphId, fromNode, fromPin, toNode, toPin) == 0;

    public uint ConnectionCount => MaterialGraphNative.GetConnectionCount(GraphId);

    public IEnumerable<MaterialGraphNative.MaterialGraphConnection> Connections
    {
        get
        {
            for (uint i = 0; i < ConnectionCount; i++)
            {
                if (MaterialGraphNative.TryGetConnection(GraphId, i, out var c))
                    yield return c;
            }
        }
    }

    // --- Param ops ---

    public bool SetParamFloat(uint nodeId, string name, float v) =>
        MaterialGraphReflectionNative.SetParamFloat(GraphId, nodeId, name, v);

    public bool SetParamFloat2(uint nodeId, string name, float x, float y) =>
        MaterialGraphReflectionNative.SetParamFloat2(GraphId, nodeId, name, x, y);

    public bool SetParamFloat3(uint nodeId, string name, float x, float y, float z) =>
        MaterialGraphReflectionNative.SetParamFloat3(GraphId, nodeId, name, x, y, z);

    public bool SetParamFloat4(uint nodeId, string name, float x, float y, float z, float w) =>
        MaterialGraphReflectionNative.SetParamFloat4(GraphId, nodeId, name, x, y, z, w);

    public bool SetParamString(uint nodeId, string name, string v) =>
        MaterialGraphReflectionNative.SetParamString(GraphId, nodeId, name, v);

    public bool TryGetParamFloat(uint nodeId, string name, out float v) =>
        MaterialGraphReflectionNative.TryGetParamFloat(GraphId, nodeId, name, out v);

    public bool TryGetParamFloat4(uint nodeId, string name, out float[] v) =>
        MaterialGraphReflectionNative.TryGetParamFloat4(GraphId, nodeId, name, out v);

    public bool TryGetParamString(uint nodeId, string name, out string value) =>
        MaterialGraphReflectionNative.TryGetParamString(GraphId, nodeId, name, 1024, out value);

    // --- Pin ops ---

    public uint GetPinCount(uint nodeId) => MaterialGraphReflectionNative.GetPinCount(GraphId, nodeId);

    public bool TryGetPin(uint nodeId, uint pinIndex, out MaterialGraphReflectionNative.MaterialPinDesc desc, out string name) =>
        MaterialGraphReflectionNative.TryGetPinDescriptor(GraphId, nodeId, pinIndex, 64, out desc, out name);

    // --- Execute / Errors ---

    public uint Execute() => MaterialGraphNative.Execute(GraphId);

    public IReadOnlyList<(uint NodeId, string Message)> GetErrors()
    {
        var result = new List<(uint, string)>();
        var count = MaterialGraphNative.GetErrorCount(GraphId);
        for (uint i = 0; i < count; i++)
        {
            if (MaterialGraphNative.TryGetError(GraphId, i, 512, out var nodeId, out var msg))
                result.Add((nodeId, msg ?? string.Empty));
        }
        return result;
    }

    public void ClearErrors() => MaterialGraphNative.ClearErrors(GraphId);

    // --- Bridge to material init_info ---

    public bool TryEvaluate(out MaterialGraphNative.MaterialInitInfo info) =>
        MaterialGraphNative.TryEvaluate(GraphId, out info);

    // --- Persistence ---

    public string? SerializeJson() => MaterialGraphNative.SerializeJson(GraphId);

    public bool LoadFromJson(string json) => MaterialGraphNative.LoadFromJson(GraphId, json);

    public bool SaveToFile(string path)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) Directory.CreateDirectory(dir);
        return MaterialGraphNative.SaveToFile(GraphId, path);
    }

    // --- Raw pointer for ShaderCompiler ---

    /// <summary>
    /// Native raw pointer for passing into <see cref="ShaderCompilerNative.Compile"/>.
    /// Valid until <see cref="Dispose"/>. Do not free.
    /// </summary>
    public IntPtr RawPtr => MaterialGraphNative.GetRawPtr(GraphId);

    public void Dispose()
    {
        if (GraphId != 0)
        {
            MaterialGraphNative.Destroy(GraphId);
            GraphId = 0;
        }
    }
}
