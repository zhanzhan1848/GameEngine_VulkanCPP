using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Native.MaterialGraph;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

/// <summary>
/// Smoke tests for the Phase 4 P/Invoke bridge layer (MaterialGraphNative,
/// MaterialGraphReflectionNative, MaterialGraphSession). Each test
/// gracefully skips when the native engine dylib cannot be loaded (e.g.
/// running in CI on a non-macOS host).
/// </summary>
public sealed class MaterialGraphSessionTests
{
    private static bool EngineAvailable()
    {
        try { return NativeEngineBridgeProxy.IsLoaded; }
        catch (DllNotFoundException) { return false; }
    }

    [Fact]
    public void Create_Destroy_RoundTrips()
    {
        if (!EngineAvailable()) return;

        using var session = MaterialGraphSession.Create();
        Assert.NotEqual(0u, session.GraphId);
        Assert.Equal(0u, session.NodeCount);
        Assert.Equal(0u, session.ConnectionCount);
    }

    [Fact]
    public void AddNode_ConstantFloat3_Registers()
    {
        if (!EngineAvailable()) return;

        using var session = MaterialGraphSession.Create();
        var nodeId = session.AddNode("ConstantFloat3");
        Assert.NotEqual(0u, nodeId);
        Assert.Equal(1u, session.NodeCount);
        Assert.Equal("ConstantFloat3", session.GetNodeTypeName(nodeId));
    }

    [Fact]
    public void Connect_Disconnect_RoundTrips()
    {
        if (!EngineAvailable()) return;

        using var session = MaterialGraphSession.Create();
        var src = session.AddNode("ConstantFloat3");
        var dst = session.AddNode("MaterialOutput");
        Assert.NotEqual(0u, src);
        Assert.NotEqual(0u, dst);

        var ok = session.Connect(src, 0, dst, 0);
        Assert.True(ok);
        Assert.Equal(1u, session.ConnectionCount);

        var conn = session.Connections.Single();
        Assert.Equal(src, conn.FromNode);
        Assert.Equal(dst, conn.ToNode);

        ok = session.Disconnect(src, 0, dst, 0);
        Assert.True(ok);
        Assert.Equal(0u, session.ConnectionCount);
    }

    [Fact]
    public void MaterialOutput_HasExpectedPins()
    {
        if (!EngineAvailable()) return;

        using var session = MaterialGraphSession.Create();
        var outId = session.AddNode("MaterialOutput");
        Assert.NotEqual(0u, outId);

        var pinCount = session.GetPinCount(outId);
        // MaterialOutput exposes 8 input pins (BaseColor / Roughness / Metallic /
        // AlphaCutoff / AlbedoTex / NormalTex / ORMTex / Technique). Output
        // pins would also be there if the node has them — we assert ≥8 to
        // cover the inputs even if outputs grow.
        Assert.True(pinCount >= 8u, $"MaterialOutput pin count = {pinCount}, expected ≥ 8");
    }

    [Fact]
    public void SampleTexture_ReflectionPreservesIndependentOutputIndex()
    {
        if (!EngineAvailable()) return;

        using var session = MaterialGraphSession.Create();
        var nodeId = session.AddNode("SampleTexture");

        Assert.Equal(3u, session.GetPinCount(nodeId));
        Assert.True(session.TryGetPin(nodeId, 0, out var texture, out _));
        Assert.True(session.TryGetPin(nodeId, 1, out var uv, out _));
        Assert.True(session.TryGetPin(nodeId, 2, out var rgba, out _));
        Assert.Equal(0u, texture.Index);
        Assert.Equal(1u, uv.Index);
        Assert.Equal(0u, rgba.Index);
        Assert.Equal(0u, rgba.IsInput);
    }

    [Fact]
    public void SaveToFile_LoadFromFile_RoundTrips()
    {
        if (!EngineAvailable()) return;

        var tmp = Path.Combine(Path.GetTempPath(), $"mg_test_{Guid.NewGuid():N}.graph.json");
        try
        {
            uint loadedId;
            using (var s1 = MaterialGraphSession.Create())
            {
                var a = s1.AddNode("ConstantFloat3");
                var b = s1.AddNode("MaterialOutput");
                Assert.True(s1.Connect(a, 0, b, 0));

                Assert.True(s1.SaveToFile(tmp));
                loadedId = MaterialGraphNative.LoadFromFile(tmp);
            }
            Assert.NotEqual(0u, loadedId);

            var loadedSession = MaterialGraphSession.Adopt(loadedId);
            try
            {
                Assert.Equal(2u, loadedSession.NodeCount);
                Assert.Equal(1u, loadedSession.ConnectionCount);
            }
            finally { loadedSession.Dispose(); }
        }
        finally
        {
            if (File.Exists(tmp)) File.Delete(tmp);
        }
    }

    [Fact]
    public void Execute_NoErrors_OnValidGraph()
    {
        if (!EngineAvailable()) return;

        using var session = MaterialGraphSession.Create();
        var c = session.AddNode("ConstantFloat3");
        var o = session.AddNode("MaterialOutput");
        Assert.True(session.Connect(c, 0, o, 0));

        session.SetParamFloat3(c, "value", 0.5f, 0.5f, 0.5f);
        var errCount = session.Execute();
        Assert.Equal(0u, errCount);
        Assert.Empty(session.GetErrors());
    }

    [Fact]
    public void Execute_SampledTextureTimesFloat3_DoesNotCrashAndPreservesTint()
    {
        if (!EngineAvailable()) return;

        using var session = MaterialGraphSession.Create();
        var sample = session.AddNode("SampleTexture");
        var tint = session.AddNode("ConstantFloat3");
        var multiply = session.AddNode("Multiply");
        var output = session.AddNode("MaterialOutput");

        Assert.True(session.Connect(sample, 0, multiply, 0));
        Assert.True(session.Connect(tint, 0, multiply, 1));
        Assert.True(session.Connect(multiply, 0, output, 0));
        session.SetParamFloat3(tint, "value", 0.8f, 0.4f, 0.2f);

        Assert.Equal(0u, session.Execute());
        Assert.True(session.TryEvaluate(out var info));
        Assert.Equal(0.8f, info.BaseColorR, 2);
        Assert.Equal(0.4f, info.BaseColorG, 2);
        Assert.Equal(0.2f, info.BaseColorB, 2);
    }

    [Fact]
    public void TryEvaluate_ReturnsPlausibleValues()
    {
        if (!EngineAvailable()) return;

        using var session = MaterialGraphSession.Create();
        var c = session.AddNode("ConstantFloat3");
        var o = session.AddNode("MaterialOutput");
        Assert.True(session.Connect(c, 0, o, 0));  // BaseColor

        session.SetParamFloat3(c, "value", 0.8f, 0.2f, 0.1f);
        session.Execute();

        Assert.True(session.TryEvaluate(out var info));
        Assert.InRange(info.BaseColorR, 0f, 1f);
        Assert.InRange(info.BaseColorG, 0f, 1f);
        Assert.InRange(info.BaseColorB, 0f, 1f);
    }

    [Fact]
    public void Registry_EnumeratesAtLeastOneNodeType()
    {
        if (!EngineAvailable()) return;

        var count = MaterialGraphReflectionNative.GetRegisteredTypeCount();
        Assert.True(count > 0u, "Expected at least one registered node type");

        Assert.True(MaterialGraphReflectionNative.TryGetRegisteredTypeInfo(
            0, 64, 64, 64, out var typeName, out var display, out var category, out _));
        Assert.False(string.IsNullOrEmpty(typeName));
        Assert.False(string.IsNullOrEmpty(display));
    }

    [Fact]
    public void ApplyMaterialGraphToEntity_RoundTrip()
    {
        if (!EngineAvailable()) return;

        // 1. Build a graph in memory: ConstantFloat3 → MaterialOutput.BaseColor.
        using var session = MaterialGraphSession.Create();
        var c = session.AddNode("ConstantFloat3");
        var o = session.AddNode("MaterialOutput");
        Assert.True(session.Connect(c, 0, o, 0));
        session.SetParamFloat3(c, "value", 0.2f, 0.4f, 0.6f);
        Assert.Equal(0u, session.Execute());
        Assert.True(session.TryEvaluate(out var info));
        Assert.Equal(0.2f, info.BaseColorR, 2);
        Assert.Equal(0.4f, info.BaseColorG, 2);
        Assert.Equal(0.6f, info.BaseColorB, 2);

        // 2. Persist to disk and re-load (this is the path SceneRuntimeSynchronizer uses).
        var tmp = Path.GetTempFileName();
        try
        {
            Assert.True(session.SaveToFile(tmp));
            var loadedId = MaterialGraphNative.LoadFromFile(tmp);
            Assert.NotEqual(0u, loadedId);
            try
            {
                // 3. Apply via the same code path the synchronizer uses:
                //    evaluate the graph, then push scalar/texture params through
                //    the MaterialAPI.SetMaterial* setters keyed by entity id.
                var adapter = new NativeEngineBridgeAdapter();
                var entityId = adapter.CreateEntity(0f, 0f, 0f);
                Assert.NotEqual(0u, entityId);

                try
                {
                    var ex = Record.Exception(() => adapter.ApplyMaterialGraph(entityId, loadedId));
                    Assert.Null(ex);
                }
                finally { adapter.DestroyEntity(entityId); }
            }
            finally { MaterialGraphNative.Destroy(loadedId); }
        }
        finally
        {
            if (File.Exists(tmp)) File.Delete(tmp);
        }
    }
}

/// <summary>
/// Probes <see cref="NativeEngineBridge.IsLoaded"/> indirectly without
/// taking a hard dependency on the editor's static type from this assembly.
/// The native bridge is loaded by the editor process at startup; the test
/// host inherits the same environment when launched via dotnet test.
/// </summary>
internal static class NativeEngineBridgeProxy
{
    public static bool IsLoaded => PrimalEditor_Avalonia.Native.NativeEngineBridge.IsLoaded;
}
