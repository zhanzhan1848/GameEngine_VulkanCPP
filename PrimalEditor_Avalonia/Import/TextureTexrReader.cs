using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Import;

/// <summary>
/// Reads the engine's .texture ("TEXR") container and decodes its first
/// mip level into an Avalonia bitmap for thumbnails / previews. The
/// container layout is written by <see cref="TextureTexrWriter"/> and
/// matches <c>EngineTest/import_texture.py</c> + the engine loader.
/// </summary>
public static class TextureTexrReader
{
    private static readonly int[] CompressedPixelFormats =
    {
        // ASTC (Metal): 186, 204, 208, 212, 216, 218, 222, 226, 230, 234, 236
        152, 153, 130, 131, 132, 133, 134, 135, 140, 141, 142, 143, 150, 151,
        160, 161, 162, 163, 164, 165, 166, 167, 180, 181, 182, 183,
        186, 187, 188, 189, 190, 192, 193, 194, 195, 196, 197, 198, 199, 200,
        204, 205, 206, 207, 208, 210, 211, 212, 213, 214, 215, 216, 217, 218,
        222, 223, 224, 225, 226, 228, 229, 230, 231, 232, 233, 234, 235, 236,
    };

    internal sealed class Slice
    {
        public uint Width;
        public uint Height;
        public uint RowPitch;
        public uint SlicePitch;
        public byte[]? Pixels;
    }

    internal sealed class TexrDocument
    {
        public uint Width;
        public uint Height;
        public uint ArraySize;
        public uint Flags;
        public uint MipLevels;
        public uint Format;
        public List<Slice> Slices = new();

        public Slice? Mip0 => Slices.Count > 0 ? Slices[0] : null;
    }

    /// <summary>
    /// Parse a TEXR file into its header + decompressed slice list without
    /// creating any Avalonia bitmap (testable headless).
    /// </summary>
    internal static TexrDocument? TryParse(string path)
    {
        try
        {
            return Read(path);
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// Mip-0 decoded into tightly packed RGBA8888 bytes (stride == width*4)
    /// plus the container metadata the texture editor surfaces (array/mip
    /// counts). Unlike <see cref="TryDecodeBitmap"/> this never touches
    /// Avalonia bitmap types, so it is safe to call from view models and
    /// test hosts.
    /// </summary>
    public sealed class TexrRgbaData
    {
        public int Width;
        public int Height;
        public int ArraySize;
        public int MipLevels;
        public byte[] Rgba = Array.Empty<byte>();
    }

    public static TexrRgbaData? TryDecodeRgba(string path, out string? error)
    {
        error = null;
        try
        {
            var doc = Read(path);
            if (doc == null) { error = "Not a valid TEXR file"; return null; }

            var mip0 = doc.Mip0;
            if (mip0 == null || mip0.Pixels == null || mip0.SlicePitch == 0)
            {
                error = "TEXR has no mip-0 data";
                return null;
            }

            if (IsCompressed((int)doc.Format))
            {
                var decoded = TryDecodeCompressedMip0(doc, mip0);
                if (decoded == null) { error = "Compressed TEXR requires astcenc to preview"; return null; }
                mip0 = decoded;
            }

            var w = (int)mip0.Width;
            var h = (int)mip0.Height;
            var stride = (int)mip0.RowPitch;
            var rgba = new byte[Math.Max(1, w * h * 4)];
            if (stride == w * 4)
            {
                Buffer.BlockCopy(mip0.Pixels!, 0, rgba, 0, Math.Min(mip0.Pixels!.Length, rgba.Length));
            }
            else
            {
                // Packed-at-rest rows can carry padding; copy row by row.
                for (var y = 0; y < h; ++y)
                    Buffer.BlockCopy(mip0.Pixels!, y * stride, rgba, y * w * 4, w * 4);
            }

            return new TexrRgbaData
            {
                Width = w,
                Height = h,
                ArraySize = (int)doc.ArraySize,
                MipLevels = (int)doc.MipLevels,
                Rgba = rgba,
            };
        }
        catch (Exception ex)
        {
            error = $"TEXR decode failed: {ex.Message}";
            Logger.Warning($"TextureTexrReader: {error}");
            return null;
        }
    }

    /// <summary>
    /// Map an MTL ASTC format enum value to its block footprint. The sRGB
    /// (186..200), LDR (204..218) and HDR (222..236) ranges share the same
    /// footprint order: 4x4, 5x4, 5x5, 6x5, 6x6, 8x5, 8x6, 8x8, 10x5, 10x6,
    /// 10x8, 10x10, 12x10, 12x12.
    /// </summary>
    private static bool TryAstcBlockDims(uint format, out int blockWidth, out int blockHeight)
    {
        var baseValue = format switch
        {
            >= 186 and <= 200 => 186,
            >= 204 and <= 218 => 204,
            >= 222 and <= 236 => 222,
            _ => 0,
        };
        if (baseValue == 0) { blockWidth = blockHeight = 0; return false; }
        (blockWidth, blockHeight) = ((int)format - baseValue) switch
        {
            0 => (4, 4), 1 => (5, 4), 2 => (5, 5), 3 => (6, 5), 4 => (6, 6),
            5 => (8, 5), 6 => (8, 6), 7 => (8, 8), 8 => (10, 5), 9 => (10, 6),
            10 => (10, 8), 11 => (10, 10), 12 => (12, 10), 13 => (12, 12),
            _ => (4, 4),
        };
        return true;
    }

    public static bool TryDecodeBitmap(string path, out Bitmap? bitmap, out string? error)
    {
        bitmap = null;
        error = null;
        try
        {
            var doc = Read(path);
            if (doc == null) { error = "Not a valid TEXR file"; return false; }

            var mip0 = doc.Mip0;
            if (mip0 == null || mip0.Pixels == null || mip0.SlicePitch == 0)
            {
                error = "TEXR has no mip-0 data";
                return false;
            }

            if (IsCompressed((int)doc.Format))
            {
                var decoded = TryDecodeCompressedMip0(doc, mip0);
                if (decoded == null) { error = "Compressed TEXR (ASTC) requires astcenc to preview"; return false; }
                mip0 = decoded;
            }

            bitmap = BitmapFromSlice(mip0);
            return bitmap != null;
        }
        catch (Exception ex)
        {
            error = $"TEXR decode failed: {ex.Message}";
            Logger.Warning($"TextureTexrReader: {error}");
            return false;
        }
    }

    private static TexrDocument? Read(string path)
    {
        if (!File.Exists(path)) return null;
        using var fs = File.OpenRead(path);
        using var reader = new BinaryReader(fs, Encoding.UTF8, leaveOpen: false);

        var magic = reader.ReadBytes(4);
        if (magic.Length != 4 || magic[0] != (byte)'T' || magic[1] != (byte)'E' || magic[2] != (byte)'X' || magic[3] != (byte)'R')
            return null;
        var version = reader.ReadUInt32();
        if (version != TextureTexrWriter.FormatVersion) return null;

        var sourcesLen = reader.ReadInt32();
        if (sourcesLen < 0 || sourcesLen > 1 << 20) return null;
        reader.ReadBytes(sourcesLen);
        reader.ReadUInt32(); // source_count

        reader.ReadUInt32(); // settings.dimension
        reader.ReadUInt32(); // settings.mip_levels
        reader.ReadSingle(); // settings.alpha_threshold
        reader.ReadUInt32(); // settings.prefer_bc7
        reader.ReadUInt32(); // settings.output_format
        reader.ReadUInt32(); // settings.compress

        var doc = new TexrDocument
        {
            Width = reader.ReadUInt32(),
            Height = reader.ReadUInt32(),
            ArraySize = reader.ReadUInt32(),
            Flags = reader.ReadUInt32(),
            MipLevels = reader.ReadUInt32(),
            Format = reader.ReadUInt32(),
        };

        var compressedLen = reader.ReadUInt32();
        if (compressedLen == 0 || compressedLen > 512 * 1024 * 1024) return null;
        var compressed = reader.ReadBytes((int)compressedLen);

        byte[] slices;
        using (var input = new MemoryStream(compressed))
        using (var output = new MemoryStream())
        {
            using (var zlib = new ZLibStream(input, CompressionMode.Decompress))
            {
                zlib.CopyTo(output);
            }
            slices = output.ToArray();
        }

        ParseSlices(doc, slices);
        return doc;
    }

    private static void ParseSlices(TexrDocument doc, byte[] slices)
    {
        using var ms = new MemoryStream(slices);
        using var reader = new BinaryReader(ms);

        var depthPerMip = new List<uint>();
        if ((doc.Flags & 0x20) != 0) // is_volume_map
        {
            var depth = doc.ArraySize;
            doc.ArraySize = 1;
            for (uint i = 0; i < doc.MipLevels; ++i)
            {
                depthPerMip.Add(depth);
                depth = Math.Max(depth >> 1, 1);
            }
        }
        else
        {
            for (uint i = 0; i < doc.MipLevels; ++i) depthPerMip.Add(1);
        }

        for (uint a = 0; a < doc.ArraySize; ++a)
        {
            for (uint m = 0; m < doc.MipLevels; ++m)
            {
                for (uint d = 0; d < depthPerMip[(int)m]; ++d)
                {
                    if (reader.BaseStream.Position + 16 > reader.BaseStream.Length) return;
                    var slice = new Slice
                    {
                        Width = reader.ReadUInt32(),
                        Height = reader.ReadUInt32(),
                        RowPitch = reader.ReadUInt32(),
                        SlicePitch = reader.ReadUInt32(),
                    };
                    if (reader.BaseStream.Position + slice.SlicePitch > reader.BaseStream.Length) return;
                    slice.Pixels = reader.ReadBytes((int)slice.SlicePitch);
                    doc.Slices.Add(slice);
                }
            }
        }
    }

    private static Slice? TryDecodeCompressedMip0(TexrDocument doc, Slice mip0)
    {
        // astcenc only understands ASTC payloads. BC/other compressed
        // containers wrapped in an ASTC header would decode to garbage,
        // so refuse them instead.
        if (!TryAstcBlockDims(doc.Format, out var blockW, out var blockH)) return null;

        var astcenc = PrimalEditor_Avalonia.Native.AstcencLocator.Resolve();
        if (string.IsNullOrEmpty(astcenc)) return null;

        var tempAstc = Path.Combine(Path.GetTempPath(), "primal_mip0_" + Guid.NewGuid().ToString("N") + ".astc");
        var tempTga = Path.Combine(Path.GetTempPath(), "primal_mip0_" + Guid.NewGuid().ToString("N") + ".tga");
        try
        {
            // Write an ASTC header + the mip0 payload.
            var header = new byte[16];
            header[0] = 0x13; header[1] = 0xAB; header[2] = 0xA1; header[3] = 0x5C;
            header[4] = (byte)blockW; header[5] = (byte)blockH; header[6] = 1;
            header[7] = (byte)(mip0.Width & 0xFF);
            header[8] = (byte)((mip0.Width >> 8) & 0xFF);
            header[9] = (byte)((mip0.Width >> 16) & 0xFF);
            header[10] = (byte)(mip0.Height & 0xFF);
            header[11] = (byte)((mip0.Height >> 8) & 0xFF);
            header[12] = (byte)((mip0.Height >> 16) & 0xFF);
            header[13] = 1; header[14] = 0; header[15] = 0;

            File.WriteAllBytes(tempAstc, header.Concat(mip0.Pixels!).ToArray());
            var psi = new System.Diagnostics.ProcessStartInfo
            {
                FileName = astcenc,
                Arguments = $"-ds \"{tempAstc}\" \"{tempTga}\"",
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            using var proc = System.Diagnostics.Process.Start(psi);
            if (proc == null) return null;
            proc.WaitForExit(30_000);
            if (proc.ExitCode != 0 || !File.Exists(tempTga)) return null;

            // Parse the TGA astcenc writes instead of going through
            // Avalonia's Bitmap — the RGBA decode path must also work
            // in hosts with no render platform (test runner, view
            // models before the UI platform boots).
            var tga = File.ReadAllBytes(tempTga);
            var (w, h, rgba) = ParseTrueColorTga(tga);
            if (w <= 0 || h <= 0 || rgba == null) return null;

            return new Slice
            {
                Width = (uint)w,
                Height = (uint)h,
                RowPitch = (uint)(w * 4),
                SlicePitch = (uint)(rgba.Length),
                Pixels = rgba,
            };
        }
        catch
        {
            return null;
        }
        finally
        {
            try { if (File.Exists(tempAstc)) File.Delete(tempAstc); } catch { }
            try { if (File.Exists(tempTga)) File.Delete(tempTga); } catch { }
        }
    }

    /// <summary>
    /// Decode an uncompressed or RLE 32-bit true-color TGA into tightly
    /// packed RGBA8888 (TGA stores BGRA). Honors the top-left origin
    /// descriptor bit; everything astcenc emits is 32-bit type-10, but
    /// type-2 falls through to the same packet loop.
    /// </summary>
    private static (int w, int h, byte[]? rgba) ParseTrueColorTga(byte[] tga)
    {
        if (tga.Length < 18) return (0, 0, null);
        var idLength = tga[0];
        var imageType = tga[2];
        var w = tga[12] | (tga[13] << 8);
        var h = tga[14] | (tga[15] << 8);
        var pixelDepth = tga[16];
        var descriptor = tga[17];
        var topLeft = (descriptor & 0x20) != 0;
        if (imageType is not (2 or 10) || pixelDepth != 32 || w <= 0 || h <= 0) return (0, 0, null);

        var pos = 18 + idLength;
        var pixels = new byte[w * h * 4];
        var srcStride = w * 4;

        void EmitRow(int y, int x, byte[] src, int srcOffset, int count)
        {
            var dstRow = (topLeft ? y : h - 1 - y) * srcStride;
            for (var i = 0; i < count && x + i < w; ++i)
            {
                var s = srcOffset + i * 4;
                var d = dstRow + (x + i) * 4;
                pixels[d + 0] = tga[s + 2]; // B->R
                pixels[d + 1] = tga[s + 1];
                pixels[d + 2] = tga[s + 0]; // R->B
                pixels[d + 3] = tga[s + 3];
            }
        }

        var y = 0;
        var x = 0;
        if (imageType == 2)
        {
            if (pos + w * h * 4 > tga.Length) return (0, 0, null);
            while (y < h)
            {
                var remaining = w - x;
                EmitRow(y, x, tga, pos, remaining);
                pos += remaining * 4;
                x = 0;
                ++y;
            }
            return (w, h, pixels);
        }

        // RLE (type 10) packets.
        while (y < h && pos < tga.Length)
        {
            var packet = tga[pos++];
            var count = (packet & 0x7F) + 1;
            if ((packet & 0x80) != 0)
            {
                if (pos + 4 > tga.Length) return (0, 0, null);
                var pixel = new byte[4];
                Array.Copy(tga, pos, pixel, 0, 4);
                pos += 4;
                var done = 0;
                while (done < count && y < h)
                {
                    var run = Math.Min(count - done, w - x);
                    // Replicate the single pixel across the run.
                    var dstRow = (topLeft ? y : h - 1 - y) * srcStride;
                    for (var i = 0; i < run; ++i)
                    {
                        var d = dstRow + (x + i) * 4;
                        pixels[d + 0] = pixel[2];
                        pixels[d + 1] = pixel[1];
                        pixels[d + 2] = pixel[0];
                        pixels[d + 3] = pixel[3];
                    }
                    x += run;
                    done += run;
                    if (x == w) { x = 0; ++y; }
                }
            }
            else
            {
                if (pos + count * 4 > tga.Length) return (0, 0, null);
                var done = 0;
                while (done < count && y < h)
                {
                    var run = Math.Min(count - done, w - x);
                    EmitRow(y, x, tga, pos + done * 4, run);
                    x += run;
                    done += run;
                    if (x == w) { x = 0; ++y; }
                }
                pos += count * 4;
            }
        }
        return (w, h, pixels);
    }


    private static Bitmap? BitmapFromSlice(Slice slice)
    {
        if (slice.Pixels == null || slice.Pixels.Length == 0) return null;

        var pixels = slice.Pixels;

        // Avalonia's `WriteableBitmap(IntPtr, ...)` captures the address
        // without copying, so the pin must survive as long as the bitmap.
        // The `WriteableBitmap` ctor needs an address up-front, so we pin
        // twice: the first slot is temporary, the second is rebound to
        // the bitmap itself and outlives the call.
        var addr = BitmapPinStore.PinAndAttach(new object(), pixels);
        var bitmap = new WriteableBitmap(
            PixelFormat.Rgba8888,
            AlphaFormat.Premul,
            addr,
            new Avalonia.PixelSize((int)slice.Width, (int)slice.Height),
            new Avalonia.Vector(96, 96),
            (int)slice.RowPitch);
        if (bitmap is WriteableBitmap wb)
        {
            BitmapPinStore.PinAndAttach(wb, pixels);
        }
        return bitmap;
    }

    private static bool IsCompressed(int format) => Array.IndexOf(CompressedPixelFormats, format) >= 0;
}
