using System.IO;
using PrimalEditor_Avalonia.Import;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Import;

public sealed class TextureTexrReaderTests
{
    private static NativeEngineBridge.CookedTexturePayload MakeRgbaPayload(int width, int height, int mips)
    {
        var slices = new System.Collections.Generic.List<byte>();
        var w = width;
        var h = height;
        for (var m = 0; m < mips; ++m)
        {
            var rowPitch = (uint)(w * 4);
            var slicePitch = rowPitch * (uint)h;
            using var ms = new MemoryStream();
            using var bw = new BinaryWriter(ms);
            bw.Write((uint)w);
            bw.Write((uint)h);
            bw.Write(rowPitch);
            bw.Write(slicePitch);
            var pixels = new byte[slicePitch];
            for (var y = 0; y < h; ++y)
                for (var x = 0; x < w; ++x)
                    pixels[y * rowPitch + x * 4] = (byte)((x + y) % 256);
            bw.Write(pixels);
            slices.AddRange(ms.ToArray());
            w = System.Math.Max(1, w / 2);
            h = System.Math.Max(1, h / 2);
        }

        return new NativeEngineBridge.CookedTexturePayload(
            "/tmp/reader_test.png", slices.ToArray(), System.Array.Empty<byte>(),
            (uint)width, (uint)height, 1, (uint)mips,
            70 /* RGBA8Unorm */, 0x42,
            0, 0,
            0, 0, 0.5f, 0, 70, 0);
    }

    [Fact]
    public void Reader_parses_mip_chain_of_uncompressed_TEXR()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalTexReader_" + System.Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var path = Path.Combine(dir, "test.texture");
            TextureTexrWriter.Write(path, MakeRgbaPayload(128, 128, 5));

            var doc = TextureTexrReader.TryParse(path);
            Assert.NotNull(doc);
            Assert.Equal(128u, doc!.Width);
            Assert.Equal(128u, doc.Height);
            Assert.Equal(5u, doc.MipLevels);
            Assert.Equal(5, doc.Slices.Count);
            Assert.NotNull(doc.Mip0);
            Assert.Equal(128u, doc.Mip0!.Width);
            Assert.Equal(128u, doc.Mip0.Height);
            Assert.Equal(128u * 4, doc.Mip0.RowPitch);
            Assert.True(doc.Mip0.Pixels!.Length > 0);
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    [Fact]
    public void Reader_returns_null_for_non_TEXR_file()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalTexReader_" + System.Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var path = Path.Combine(dir, "not_texture.bin");
            File.WriteAllBytes(path, new byte[] { 1, 2, 3, 4, 5, 6, 7, 8 });

            Assert.Null(TextureTexrReader.TryParse(path));
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }
}
