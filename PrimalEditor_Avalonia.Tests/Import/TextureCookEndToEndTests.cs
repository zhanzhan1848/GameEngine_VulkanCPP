using System;
using System.IO;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Import;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Import;

public sealed class TextureCookEndToEndTests
{
    private static bool TryLoadEngine()
    {
        // P/Invoke resolves libContentTools.dylib at runtime; if the
        // macOS dylib is missing from the test bin dir, skip cleanly.
        if (!NativeEngineBridge.LoadEngine()) return false;

        // Compressed cooks shell out to astcenc; make it available to the
        // native side or the tests will fail with nativeError=2 (compress).
        return AstcencLocator.EnsureExported() != null;
    }

    private static NativeEngineBridge.CookTextureStatus CookOnce(string fixturePath, TextureImportSettings settings, out uint nativeError)
    {
        nativeError = 0;
        var dir = Path.Combine(Path.GetTempPath(), "PrimalTexSmoke_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var fullPath = Path.Combine(AppContext.BaseDirectory, "Fixtures", "textures", fixturePath);
            if (!File.Exists(fullPath)) return NativeEngineBridge.CookTextureStatus.InvalidArgument;

            var (payload, status, err) = NativeEngineBridge.CookTexture(fullPath, settings);
            nativeError = err;
            if (payload != null)
            {
                TextureTexrWriter.Write(Path.Combine(dir, fixturePath + ".texture"), payload);
            }
            return status;
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    [Fact]
    public void Real_8bit_grayscale_PNG_cooks_to_TEXR()
    {
        if (!TryLoadEngine()) return;

        var settings = new TextureImportSettings
        {
            Compress = true,
            OutputFormat = TextureFormat.BC5_UNORM,
            IsSRGB = false,
            PreferBC7 = false,
            Dimension = TextureDimension.Texture2D,
        };

        var status = CookOnce("ChainTexture_Metallic.png", settings, out var err);
        Assert.True(status == NativeEngineBridge.CookTextureStatus.ProbeSucceeded, $"status={status} nativeError={err}");
    }

    [Fact]
    public void Real_chain_normal_PNG_cooks_to_TEXR()
    {
        if (!TryLoadEngine()) return;

        var settings = new TextureImportSettings
        {
            Compress = true,
            OutputFormat = TextureFormat.BC5_UNORM,
            IsSRGB = false,
            PreferBC7 = false,
            Dimension = TextureDimension.Texture2D,
        };

        var status = CookOnce("ChainTexture_Normal.png", settings, out var err2);
        Assert.True(status == NativeEngineBridge.CookTextureStatus.ProbeSucceeded, $"status={status} nativeError={err2}");
    }

    [Fact]
    public void Real_albedo_PNG_cooks_to_TEXR()
    {
        if (!TryLoadEngine()) return;

        var settings = new TextureImportSettings
        {
            Compress = true,
            OutputFormat = TextureFormat.BC7_UNORM,
            IsSRGB = true,
            AlphaThreshold = 0.5f,
            Dimension = TextureDimension.Texture2D,
        };

        var status = CookOnce("viking_room.png", settings, out var err3);
        Assert.True(status == NativeEngineBridge.CookTextureStatus.ProbeSucceeded, $"status={status} nativeError={err3}");
    }
}
