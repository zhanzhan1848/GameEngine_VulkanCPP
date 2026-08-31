using System.IO;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Import;
using PrimalEditor_Avalonia.Native;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Import;

public sealed class TextureTexrWriterTests
{
    private static NativeEngineBridge.CookedTexturePayload MakePayload(string sourcePath, byte[] slices, byte[]? icon = null)
    {
        return new NativeEngineBridge.CookedTexturePayload(
            sourcePath, slices, icon ?? System.Array.Empty<byte>(),
            128, 128, 1, 6,
            70, 0x42,
            0, 0,
            0, 0, 0.5f, 0, 70, 1);
    }

    [Fact]
    public void Write_emits_TEXR_magic_and_version_1()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalTex_" + System.Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var outPath = Path.Combine(dir, "in.texture");
            var payload = MakePayload("/tmp/in.png", new byte[] { 1, 2, 3, 4 });

            TextureTexrWriter.Write(outPath, payload);

            using var fs = File.OpenRead(outPath);
            using var br = new BinaryReader(fs);
            var magic = br.ReadBytes(4);
            Assert.Equal(new byte[] { (byte)'T', (byte)'E', (byte)'X', (byte)'R' }, magic);
            Assert.Equal(TextureTexrWriter.FormatVersion, br.ReadUInt32());
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    [Fact]
    public void Write_round_trip_settings_and_info()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalTex_" + System.Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var outPath = Path.Combine(dir, "round_trip.texture");
            var slices = new byte[1024];
            for (int i = 0; i < slices.Length; ++i) slices[i] = (byte)(i & 0xff);

            var payload = new NativeEngineBridge.CookedTexturePayload(
                "/tmp/source.png", slices, new byte[256],
                256, 512, 4, 8,
                70, 0x42,
                16, 1,
                0, 0, 0.75f, 1, 130, 0);

            TextureTexrWriter.Write(outPath, payload);

            using var fs = File.OpenRead(outPath);
            using var br = new BinaryReader(fs);
            br.ReadBytes(4);
            br.ReadUInt32();
            var sourcesLen = br.ReadInt32();
            var sourcesBytes = br.ReadBytes(sourcesLen);
            Assert.Equal(System.Text.Encoding.UTF8.GetBytes("/tmp/source.png"), sourcesBytes);
            Assert.Equal(payload.SourceCount == 0 ? 1u : payload.SourceCount, br.ReadUInt32());

            Assert.Equal(payload.SettingsDimension, br.ReadUInt32());
            Assert.Equal(payload.SettingsMipLevels, br.ReadUInt32());
            Assert.Equal(payload.SettingsAlphaThreshold, br.ReadSingle());
            Assert.Equal(payload.SettingsPreferBc7, br.ReadUInt32());
            Assert.Equal(payload.SettingsOutputFormat, br.ReadUInt32());
            Assert.Equal(payload.SettingsCompress, br.ReadUInt32());

            Assert.Equal(payload.Width, br.ReadUInt32());
            Assert.Equal(payload.Height, br.ReadUInt32());
            Assert.Equal(payload.ArraySize, br.ReadUInt32());
            Assert.Equal(payload.Flags, br.ReadUInt32());
            Assert.Equal(payload.MipLevels, br.ReadUInt32());
            Assert.Equal(payload.Format, br.ReadUInt32());

            var compressedLen = br.ReadUInt32();
            var compressedBytes = br.ReadBytes((int)compressedLen);
            Assert.NotEmpty(compressedBytes);

            var decompressed = TextureTexrWriterForTests.ZlibDecompress(compressedBytes);
            Assert.Equal(slices, decompressed);
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }
}

internal static class TextureTexrWriterForTests
{
    public static byte[] ZlibDecompress(byte[] compressed)
    {
        using var input = new MemoryStream(compressed);
        using var output = new MemoryStream();
        using (var zlib = new System.IO.Compression.ZLibStream(input, System.IO.Compression.CompressionMode.Decompress))
        {
            zlib.CopyTo(output);
        }
        return output.ToArray();
    }
}
