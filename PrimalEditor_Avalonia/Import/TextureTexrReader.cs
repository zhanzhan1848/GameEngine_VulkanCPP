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
        var astcenc = PrimalEditor_Avalonia.Native.AstcencLocator.Resolve();
        if (string.IsNullOrEmpty(astcenc)) return null;

        var tempAstc = Path.Combine(Path.GetTempPath(), "primal_mip0_" + Guid.NewGuid().ToString("N") + ".astc");
        var tempPng = Path.Combine(Path.GetTempPath(), "primal_mip0_" + Guid.NewGuid().ToString("N") + ".png");
        try
        {
            // Write an ASTC header (block 4x4) + the mip0 payload.
            var header = new byte[16];
            header[0] = 0x13; header[1] = 0xAB; header[2] = 0xA1; header[3] = 0x5C;
            header[4] = 4; header[5] = 4; header[6] = 1;
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
                Arguments = $"-ds \"{tempAstc}\" \"{tempPng}\"",
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            using var proc = System.Diagnostics.Process.Start(psi);
            if (proc == null) return null;
            proc.WaitForExit(30_000);
            if (proc.ExitCode != 0 || !File.Exists(tempPng)) return null;

            var bmp = new Avalonia.Media.Imaging.Bitmap(tempPng);
            using var ms2 = new MemoryStream();
            bmp.Save(ms2);
            var pngBytes = ms2.ToArray();
            var slice = new Slice
            {
                Width = (uint)bmp.PixelSize.Width,
                Height = (uint)bmp.PixelSize.Height,
                RowPitch = (uint)(bmp.PixelSize.Width * 4),
                SlicePitch = (uint)(bmp.PixelSize.Width * 4 * bmp.PixelSize.Height),
                Pixels = pngBytes,
            };
            return slice;
        }
        catch
        {
            return null;
        }
        finally
        {
            try { if (File.Exists(tempAstc)) File.Delete(tempAstc); } catch { }
            try { if (File.Exists(tempPng)) File.Delete(tempPng); } catch { }
        }
    }

    private static Bitmap? BitmapFromSlice(Slice slice)
    {
        if (slice.Pixels == null || slice.Pixels.Length == 0) return null;
        var handle = System.Runtime.InteropServices.GCHandle.Alloc(slice.Pixels, System.Runtime.InteropServices.GCHandleType.Pinned);
        try
        {
            // Slice is RGBA8 (loaded by ContentTools into a 32bpp RGBA buffer).
            return new WriteableBitmap(
                PixelFormat.Rgba8888,
                AlphaFormat.Premul,
                handle.AddrOfPinnedObject(),
                new Avalonia.PixelSize((int)slice.Width, (int)slice.Height),
                new Avalonia.Vector(96, 96),
                (int)slice.RowPitch);
        }
        finally
        {
            handle.Free();
        }
    }

    private static bool IsCompressed(int format) => Array.IndexOf(CompressedPixelFormats, format) >= 0;
}
