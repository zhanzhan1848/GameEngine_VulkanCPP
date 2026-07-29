using System;
using System.IO;
using Avalonia;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace PrimalEditor_Avalonia.Editors.Texture;

/// <summary>
/// Decoded result: which (array, mip, depth) slices are available and
/// the raw pixel data backing the editor preview. The view layer
/// (Avalonia) is responsible for turning this into a Bitmap; this
/// type only carries the geometry + RGBA bytes so that we can keep
/// the editor VM unit-testable.
/// </summary>
public sealed class TextureDecoded
{
    public TextureDecoded(int width, int height, int arrayCount, int mipCount, int depth, ReadOnlyMemory<byte> rgba)
    {
        Width = width;
        Height = height;
        ArrayCount = arrayCount;
        MipCount = mipCount;
        Depth = depth;
        Rgba = rgba;
    }

    public int Width { get; }
    public int Height { get; }
    public int ArrayCount { get; }
    public int MipCount { get; }
    public int Depth { get; }
    public ReadOnlyMemory<byte> Rgba { get; }
}

public interface ITextureSliceDecoder
{
    /// <summary>
    /// Returns a decoded slice set for the given asset path, or
    /// <c>null</c> if the source cannot be decoded by this decoder.
    /// Implementations must not throw for unsupported files; they
    /// should return null so the editor can fall back to a "no preview"
    /// state.
    /// </summary>
    TextureDecoded? TryDecode(string assetPath);
}

/// <summary>
/// Default Avalonia decoder: opens the asset via
/// <see cref="Bitmap"/> and copies the top-level RGBA into memory.
/// This is the v1 behaviour: it exposes one array slice, one mip,
/// one depth plane — the sidecar's "array/mip/depth" fields are
/// surfaced as available indices so the editor can still navigate
/// them, but only index 0 contains real pixels.
/// </summary>
public sealed class AvaloniaTextureSliceDecoder : ITextureSliceDecoder
{
    public TextureDecoded? TryDecode(string assetPath)
    {
        if (string.IsNullOrEmpty(assetPath) || !File.Exists(assetPath)) return null;
        try
        {
            using var fs = File.OpenRead(assetPath);
            var bmp = new Bitmap(fs);
            int w = bmp.PixelSize.Width;
            int h = bmp.PixelSize.Height;
            if (w <= 0 || h <= 0) return null;
            var rgba = new byte[w * h * 4];
            var handle = System.Runtime.InteropServices.GCHandle.Alloc(rgba, System.Runtime.InteropServices.GCHandleType.Pinned);
            try
            {
                bmp.CopyPixels(new PixelRect(0, 0, w, h), handle.AddrOfPinnedObject(), rgba.Length, w * 4);
            }
            finally
            {
                handle.Free();
            }
            return new TextureDecoded(w, h, 1, 1, 1, rgba);
        }
        catch
        {
            return null;
        }
    }
}
