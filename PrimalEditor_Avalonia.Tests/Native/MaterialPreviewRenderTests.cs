using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using PrimalEditor_Avalonia.Import;
using PrimalEditor_Avalonia.Editors.Material;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.Native.MaterialGraph;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Native;

/// <summary>
/// Smoke tests for the Phase-5 3D material preview rendering path. Each
/// test gracefully skips when the native engine dylib can't be loaded (e.g.
/// CI on a non-macOS host or a clean sandbox).
/// </summary>
public sealed class MaterialPreviewRenderTests
{
    [Fact]
    public void StandardPreviewMeshes_MatchUnrealPrimitiveSet()
    {
        var meshes = MaterialPreviewViewModel.StandardPreviewMeshes;

        Assert.Collection(meshes,
            mesh => Assert.Equal(PreviewModelType.Sphere, mesh.Model),
            mesh => Assert.Equal(PreviewModelType.Cylinder, mesh.Model),
            mesh => Assert.Equal(PreviewModelType.Plane, mesh.Model),
            mesh => Assert.Equal(PreviewModelType.Box, mesh.Model));
    }

    private static bool EngineAvailable()
    {
        try
        {
            return NativeEngineBridgeProxy.IsLoaded;
        }
        catch (DllNotFoundException) { return false; }
    }

    [Fact]
    public void SampleTextureGraph_BindsCookedTextureAndRendersIt()
    {
        if (!EngineAvailable()) return;
        var deviceHandle = NativeEngineBridge.GetEngineDeviceHandle();
        if (deviceHandle == 0) return;

        var dir = Path.Combine(Path.GetTempPath(), "PrimalMaterialPreview_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        uint previewId = 0;
        try
        {
            const int size = 8;
            var pixels = new byte[size * size * 4];
            for (var i = 0; i < size * size; ++i)
            {
                pixels[i * 4 + 0] = 0;
                pixels[i * 4 + 1] = 255;
                pixels[i * 4 + 2] = 0;
                pixels[i * 4 + 3] = 255;
            }
            byte[] slices;
            using (var stream = new MemoryStream())
            using (var writer = new BinaryWriter(stream))
            {
                writer.Write((uint)size);
                writer.Write((uint)size);
                writer.Write((uint)(size * 4));
                writer.Write((uint)pixels.Length);
                writer.Write(pixels);
                slices = stream.ToArray();
            }
            var texturePath = Path.Combine(dir, "green.texture");
            TextureTexrWriter.Write(texturePath, new NativeEngineBridge.CookedTexturePayload(
                "green.png", slices, Array.Empty<byte>(), size, size, 1, 1,
                28, 0, 0, 1, 0, 1, 0.5f, 0, 28, 0));

            previewId = MaterialPreviewNative.Create(deviceHandle, 64, 64);
            Assert.NotEqual(0u, previewId);
            var pathJson = JsonSerializer.Serialize(texturePath);
            var graph = "{\"nodes\":[" +
                        "{\"type\":\"ConstantTexture\",\"params\":{\"asset_path\":" + pathJson + "}}," +
                        "{\"type\":\"UV\"},{\"type\":\"SampleTexture\"},{\"type\":\"MaterialOutput\"}]," +
                        "\"connections\":[" +
                        "{\"from_node\":0,\"from_pin\":0,\"to_node\":2,\"to_pin\":0}," +
                        "{\"from_node\":1,\"from_pin\":0,\"to_node\":2,\"to_pin\":1}," +
                        "{\"from_node\":2,\"from_pin\":0,\"to_node\":3,\"to_pin\":0}]}";
            Assert.True(MaterialPreviewNative.SetGraph(previewId, graph));

            var rgba = new byte[64 * 64 * 4];
            Assert.Equal((ulong)rgba.Length,
                MaterialPreviewNative.RenderToRGBA(previewId, deviceHandle, rgba));
            var center = (32 * 64 + 32) * 4;
            Assert.True(rgba[center + 1] > rgba[center] + 50 &&
                        rgba[center + 1] > rgba[center + 2] + 50,
                $"expected green sampled center pixel, got [{rgba[center]}, {rgba[center + 1]}, {rgba[center + 2]}, {rgba[center + 3]}]");
        }
        finally
        {
            if (previewId != 0) try { MaterialPreviewNative.Destroy(previewId); } catch { }
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    [Fact]
    public void RenderToRGBA_WritesBytesForValidPreview()
    {
        if (!EngineAvailable()) return;

        var deviceHandle = NativeEngineBridge.GetEngineDeviceHandle();
        if (deviceHandle == 0) return; // engine not initialized — skip

        const uint w = 64;
        const uint h = 64;
        uint previewId;
        try { previewId = MaterialPreviewNative.Create(deviceHandle, w, h); }
        catch (DllNotFoundException) { return; }
        Assert.NotEqual(0u, previewId);

        try
        {
            // Push a tiny empty graph (MaterialOutput only) so the renderer
            // has something to evaluate. An empty JSON is fine for the
            // smoke test — the preview still draws the gradient background
            // and the default sphere.
            Assert.True(MaterialPreviewNative.SetGraph(previewId, "{}"));

            var pixels = new byte[w * h * 4];
            ulong bytesWritten = MaterialPreviewNative.RenderToRGBA(previewId, deviceHandle, pixels);
            Assert.Equal(w * h * 4UL, bytesWritten);

            // At least one byte must be non-zero (the background gradient
            // is always non-black, so a fully-zero buffer means the
            // readback silently failed).
            bool anyNonZero = false;
            for (int i = 0; i < pixels.Length; i++)
            {
                if (pixels[i] != 0) { anyNonZero = true; break; }
            }
            Assert.True(anyNonZero, "preview readback returned all-zero pixels");
        }
        finally
        {
            try { MaterialPreviewNative.Destroy(previewId); }
            catch { /* ignore — best-effort cleanup */ }
        }
    }

    [Fact]
    public void RenderToRGBA_RejectsBufferTooSmall()
    {
        if (!EngineAvailable()) return;

        var deviceHandle = NativeEngineBridge.GetEngineDeviceHandle();
        if (deviceHandle == 0) return;

        uint previewId;
        try { previewId = MaterialPreviewNative.Create(deviceHandle, 64, 64); }
        catch (DllNotFoundException) { return; }
        if (previewId == 0) return;

        try
        {
            // 100 bytes is far less than 64*64*4 = 16384 — the ABI should
            // refuse rather than overflow the caller's buffer.
            var tooSmall = new byte[100];
            ulong bytesWritten = MaterialPreviewNative.RenderToRGBA(previewId, deviceHandle, tooSmall);
            Assert.Equal(0UL, bytesWritten);
        }
        finally
        {
            try { MaterialPreviewNative.Destroy(previewId); }
            catch { /* ignore */ }
        }
    }

    [Fact]
    public void SetModel_Plane_Works()
    {
        if (!EngineAvailable()) return;

        var deviceHandle = NativeEngineBridge.GetEngineDeviceHandle();
        if (deviceHandle == 0) return;

        uint previewId;
        try { previewId = MaterialPreviewNative.Create(deviceHandle, 64, 64); }
        catch (DllNotFoundException) { return; }
        if (previewId == 0) return;

        try
        {
            // Plane = 7 in the PreviewModelType enum. Just exercises the
            // path doesn't crash on a different model — no return value to
            // assert since the native side is void.
            MaterialPreviewNative.SetModel(previewId, (uint)PreviewModelType.Plane);
        }
        finally
        {
            try { MaterialPreviewNative.Destroy(previewId); }
            catch { /* ignore */ }
        }
    }
}
