using System;
using System.Buffers.Binary;
using System.IO;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Import;
using PrimalEditor_Avalonia.Native;
using CookedTexturePayload = PrimalEditor_Avalonia.Native.NativeEngineBridge.CookedTexturePayload;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Import;

/// <summary>
/// Regression coverage for the double-click texture editor path: the
/// editor's slice decoder must decode engine-cooked TEXR containers
/// (it previously only handled Skia-decodable images and silently
/// returned null for every .texture asset).
/// </summary>
public sealed class TextureTexrRgbaDecodeTests
{
    private const uint MTLFormatRGBA8Unorm = 70;
    private const uint MTLFormatBC1RGBA = 130;

    private static CookedTexturePayload Payload(
        uint width, uint height, uint format, byte[] slices, uint mipLevels = 1)
        => new(
            sourcePath: "test.png",
            slices: slices,
            icon: Array.Empty<byte>(),
            width: width, height: height, arraySize: 1, mipLevels: mipLevels,
            format: format, flags: 0,
            sourcesLen: 7, sourceCount: 1,
            settingsDimension: 2, settingsMipLevels: mipLevels, settingsAlphaThreshold: 0.5f,
            settingsPreferBc7: 0, settingsOutputFormat: format, settingsCompress: 1);

    private static byte[] SliceRecord(uint w, uint h, uint rowPitch, byte[] pixels)
    {
        var record = new byte[16 + pixels.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(record.AsSpan(0), w);
        BinaryPrimitives.WriteUInt32LittleEndian(record.AsSpan(4), h);
        BinaryPrimitives.WriteUInt32LittleEndian(record.AsSpan(8), rowPitch);
        BinaryPrimitives.WriteUInt32LittleEndian(record.AsSpan(12), (uint)pixels.Length);
        pixels.CopyTo(record, 16);
        return record;
    }

    private static string WriteTempTexr(CookedTexturePayload payload)
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalTexrRgba_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, "test.texture");
        TextureTexrWriter.Write(path, payload);
        return path;
    }

    [Fact]
    public void Uncompressed_Rgba8_Decodes_Tightly_Packed()
    {
        // 4x4 RGBA gradient.
        var pixels = new byte[4 * 4 * 4];
        for (var i = 0; i < pixels.Length; ++i) pixels[i] = (byte)(i * 3 + 1);
        var path = WriteTempTexr(Payload(4, 4, MTLFormatRGBA8Unorm, SliceRecord(4, 4, 16, pixels)));
        try
        {
            var decoded = TextureTexrReader.TryDecodeRgba(path, out var error);
            Assert.True(decoded != null, error ?? "decode returned null");
            Assert.Equal(4, decoded!.Width);
            Assert.Equal(4, decoded.Height);
            Assert.Equal(1, decoded.MipLevels);
            Assert.Equal(1, decoded.ArraySize);
            Assert.Equal(pixels, decoded.Rgba);
        }
        finally
        {
            try { Directory.Delete(Path.GetDirectoryName(path)!, true); } catch { }
        }
    }

    [Fact]
    public void Padded_Row_Pitch_Is_Squeezed_To_Width_Stride()
    {
        // 2x2 with a 12-byte row pitch (4 bytes of padding per row).
        var pixels = new byte[2 * 12];
        for (var y = 0; y < 2; ++y)
            for (var x = 0; x < 8; ++x)
                pixels[y * 12 + x] = (byte)(y * 8 + x);
        var path = WriteTempTexr(Payload(2, 2, MTLFormatRGBA8Unorm, SliceRecord(2, 2, 12, pixels)));
        try
        {
            var decoded = TextureTexrReader.TryDecodeRgba(path, out var error);
            Assert.True(decoded != null, error ?? "decode returned null");
            var expected = new byte[2 * 2 * 4];
            Buffer.BlockCopy(pixels, 0, expected, 0, 8);
            Buffer.BlockCopy(pixels, 12, expected, 8, 8);
            Assert.Equal(expected, decoded!.Rgba);
        }
        finally
        {
            try { Directory.Delete(Path.GetDirectoryName(path)!, true); } catch { }
        }
    }

    [Fact]
    public void NonAstc_Compressed_Format_Is_Rejected_Not_Garbled()
    {
        var path = WriteTempTexr(Payload(4, 4, MTLFormatBC1RGBA, SliceRecord(4, 4, 16, new byte[64])));
        try
        {
            var decoded = TextureTexrReader.TryDecodeRgba(path, out var error);
            Assert.Null(decoded);
            Assert.Contains("astcenc", error);
        }
        finally
        {
            try { Directory.Delete(Path.GetDirectoryName(path)!, true); } catch { }
        }
    }

    [Fact]
    public void Editor_Decoder_Decodes_Real_Astc_Texture()
    {
        // Real engine-cooked ASTC4x4 (fmt=204) asset. Requires the
        // astcenc CLI — skip cleanly when it has not been built.
        var fixture = Path.Combine(AppContext.BaseDirectory, "Fixtures", "textures", "gi_flag_astc.texture");
        Assert.True(File.Exists(fixture), $"missing fixture: {fixture}");
        if (AstcencLocator.Resolve() == null) return;

        var decoded = new AvaloniaTextureSliceDecoder().TryDecode(fixture);
        Assert.True(decoded != null, "editor decoder returned null for ASTC TEXR fixture");
        Assert.Equal(128, decoded!.Width);
        Assert.Equal(128, decoded.Height);
        Assert.NotEqual(0L, SumNonZero(decoded.Rgba.Span));
    }

    private static long SumNonZero(ReadOnlySpan<byte> data)
    {
        long sum = 0;
        foreach (var b in data) sum += b;
        return sum;
    }
}
