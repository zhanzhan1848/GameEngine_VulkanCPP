using System;
using System.IO;
using System.IO.Compression;
using System.Text;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Import;

/// <summary>
/// Writes the engine's native .texture asset format ("TEXR") — text-binary
/// header followed by zlib-compressed cooked subresource slices. Layout
/// matches the existing importer in <c>EngineTest/import_texture.py</c>
/// and the loader in <c>Engine/Content/ContentToEngine.cpp::create_texture_resource</c>.
/// </summary>
public static class TextureTexrWriter
{
    public const uint FormatVersion = 1;
    public const string FormatMagic = "TEXR";

    public static void Write(string outputPath, NativeEngineBridge.CookedTexturePayload payload)
    {
        if (string.IsNullOrEmpty(outputPath)) throw new ArgumentException(nameof(outputPath));
        ArgumentNullException.ThrowIfNull(payload);

        var sources = payload.SourcePath ?? string.Empty;

        using var fs = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.None);
        using var writer = new BinaryWriter(fs, Encoding.UTF8, leaveOpen: false);

        writer.Write(Encoding.ASCII.GetBytes(FormatMagic));
        writer.Write(FormatVersion);

        var sourcesBytes = Encoding.UTF8.GetBytes(sources);
        writer.Write(sourcesBytes.Length);
        writer.Write(sourcesBytes);
        writer.Write(payload.SourceCount == 0 ? 1u : payload.SourceCount);

        writer.Write(payload.SettingsDimension);
        writer.Write(payload.SettingsMipLevels);
        writer.Write(payload.SettingsAlphaThreshold);
        writer.Write(payload.SettingsPreferBc7);
        writer.Write(payload.SettingsOutputFormat);
        writer.Write(payload.SettingsCompress);

        writer.Write(payload.Width);
        writer.Write(payload.Height);
        writer.Write(payload.ArraySize);
        writer.Write(payload.Flags);
        writer.Write(payload.MipLevels);
        writer.Write(payload.Format);

        var compressed = ZlibCompress(payload.Slices);
        writer.Write((uint)compressed.Length);
        writer.Write(compressed);
    }

    private static byte[] ZlibCompress(byte[] input)
    {
        if (input == null || input.Length == 0) return Array.Empty<byte>();
        using var output = new MemoryStream();
        // ZLibStream in net8 emits the zlib-wrapped (RFC 1950) stream that
        // <c>uncompress()</c> on the engine side expects — header byte 0x78,
        // a deflate payload, adler-32 trailer.
        using (var zlib = new ZLibStream(output, CompressionLevel.Optimal, leaveOpen: true))
        {
            zlib.Write(input, 0, input.Length);
        }
        return output.ToArray();
    }
}
