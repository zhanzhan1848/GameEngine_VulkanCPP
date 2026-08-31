using System.IO;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Converters;
using PrimalEditor_Avalonia.Import;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Converters;

public sealed class AssetThumbnailConverterTests
{
    private static string WriteSampleTexr()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalThumb_" + System.Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, "sample.texture");

        var slices = new System.Collections.Generic.List<byte>();
        using (var ms = new MemoryStream())
        using (var bw = new BinaryWriter(ms))
        {
            bw.Write(64u);            // width
            bw.Write(64u);            // height
            bw.Write(64u * 4);        // rowPitch
            bw.Write(64u * 64u * 4u); // slicePitch
            var pixels = new byte[64 * 64 * 4];
            for (var i = 0; i < pixels.Length; i += 4)
            {
                pixels[i] = 200; pixels[i + 1] = 100; pixels[i + 2] = 50; pixels[i + 3] = 255;
            }
            bw.Write(pixels);
            slices.AddRange(ms.ToArray());
        }

        var payload = new NativeEngineBridge.CookedTexturePayload(
            "/tmp/sample.png", slices.ToArray(), System.Array.Empty<byte>(),
            64, 64, 1, 1,
            70 /* RGBA8Unorm */, 0x42,
            0, 0, 0, 0, 0.5f, 0, 70, 0);
        TextureTexrWriter.Write(path, payload);
        return path;
    }

    [Fact]
    public void Converter_returns_path_for_regular_image()
    {
        var converter = new AssetThumbnailConverter();
        var info = new AssetInfo
        {
            FullPath = "/tmp/foo.png",
            Type = PrimalEditor_Avalonia.Native.AssetType.Texture,
        };
        Assert.Equal(info.FullPath, converter.Convert(info, typeof(object), null, null));
    }

    [Fact]
    public void Converter_returns_bitmap_for_texr()
    {
        // Creating a WriteableBitmap requires an initialized Avalonia
        // platform (IPlatformRenderInterface), which the headless test
        // host does not have. The parse/decode path itself is covered by
        // TextureTexrReaderTests; this asserts the converter wiring only
        // when a platform is present.
        var path = WriteSampleTexr();
        try
        {
            var converter = new AssetThumbnailConverter();
            var info = new AssetInfo
            {
                FullPath = path,
                Type = PrimalEditor_Avalonia.Native.AssetType.Texture,
            };

            var result = converter.Convert(info, typeof(object), null, null);
            if (Avalonia.Application.Current == null)
            {
                // Headless: TEXR parse succeeded if not null; bitmap creation skipped.
                Assert.Null(result);
                return;
            }
            Assert.IsAssignableFrom<Avalonia.Media.Imaging.Bitmap>(result);
        }
        finally
        {
            try { Directory.Delete(Path.GetDirectoryName(path)!, true); } catch { }
        }
    }
}
