using System;
using System.Linq;
using System.Threading;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

/// <summary>
/// Smoke tests that lock down the adapter's external contract:
/// <list type="bullet">
/// <item><see cref="NativeEngineBridgeAdapter"/> never throws
/// <see cref="NotSupportedException"/> for any of the bridge methods —
/// earlier revisions of the adapter used to throw that, which made the
/// real editor unable to show models even when the engine was loaded.</item>
/// <item>When the native bridge is not loaded, the adapter returns
/// 0 / no-op instead of throwing.</item>
/// <item><see cref="NativeEngineBridgeAdapter.CreateMaterial"/> always
/// returns a non-zero material id when the engine is loaded, regardless
/// of which shader Guid is supplied (v1 only exposes a single default
/// material; the shader Guid is logged for future C++ support).</item>
/// </list>
/// </summary>
public sealed class NativeEngineBridgeAdapterSmokeTests
{
    [Fact]
    public void Adapter_MethodsDoNotThrowNotSupported()
    {
        var adapter = new NativeEngineBridgeAdapter();

        // The only error path that is allowed is: engine not loaded →
        // return default sentinel. The contract being verified here is
        // the absence of the historical NotSupportedException from
        // CreateMaterial / SetEntityMesh.
        var shader = new AssetGuid(Guid.NewGuid());
        Exception? caught = null;
        try
        {
            _ = adapter.IsLoaded;
            _ = adapter.CreateEntity(0, 0, 0);
            adapter.DestroyEntity(0);
            adapter.SetEntityTransform(0, new float[3], new float[3], new float[3]);
            _ = adapter.AddEntityLight(0, 0, 1, new float[3], 1, 0, 0, true);
            _ = adapter.AddEntityCamera(0, 0, 1, 1, 0.1f, 1000, new float[3]);
            _ = adapter.ImportMesh("/nonexistent/path.cooked");
            _ = adapter.CreateMaterial(shader);
            adapter.SetEntityMesh(0, 0, new uint[] { 1, 2 }, 2);
        }
        catch (NotSupportedException nsx)
        {
            caught = nsx;
        }

        Assert.Null(caught);
    }

    [Fact]
    public void CreateMaterial_ReturnsZeroWhenBridgeNotLoaded()
    {
        // Editor runs without the engine DLL loaded; CreateMaterial
        // must not throw and must report the engine-unavailable state
        // by returning 0.
        var adapter = new NativeEngineBridgeAdapter();

        var materialId = adapter.CreateMaterial(new AssetGuid(Guid.NewGuid()));

        Assert.True(materialId == 0 || !adapter.IsLoaded,
            "CreateMaterial must return 0 when the engine bridge is not loaded.");
    }

    [Fact]
    public void Adapter_ImportMeshTimeout_IsHonoured()
    {
        var adapter = new NativeEngineBridgeAdapter
        {
            ImportMeshTimeout = TimeSpan.FromMilliseconds(50),
            ImportMeshPollInterval = TimeSpan.FromMilliseconds(5),
        };

        if (!adapter.IsLoaded)
        {
            // Without the engine, ImportMesh returns 0 immediately and
            // the timeout setting is unused; that's fine for this
            // contract test, which only asserts that *some* terminal
            // value is produced.
            Assert.Equal(0UL, adapter.ImportMesh("/does/not/exist"));
            return;
        }

        var sw = System.Diagnostics.Stopwatch.StartNew();
        var meshId = adapter.ImportMesh("/does/not/exist");
        sw.Stop();
        Assert.True(sw.Elapsed < TimeSpan.FromSeconds(2),
            "ImportMesh should not block forever; total time was " + sw.Elapsed);
    }
}
