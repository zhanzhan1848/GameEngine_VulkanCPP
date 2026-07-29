using System;

namespace PrimalEditor_Avalonia.Editors.Texture;

/// <summary>
/// One decoded RGBA pixel: 8-bit per channel, premultiplied off.
/// </summary>
public readonly record struct TexturePixel(byte R, byte G, byte B, byte A);

/// <summary>
/// Immutable decoded slice container for a single texture asset.
///
/// This is a UI-independent model: a slice set describes the
/// (array × mip × depth) layout the editor can navigate. Real CPU-side
/// pixel data is only stored for the slices that have been decoded —
/// in v1 we store a single mip/array/depth image; callers that have
/// real mip chains can construct alternative sets in the future.
///
/// The setter is <see cref="WithIndices"/>: it returns a new instance
/// with the selected (array, mip, depth) triple, never mutating the
/// receiver, so Undo/Redo and PropertyChanged callers can keep the
/// original slice data alive.
/// </summary>
public sealed class TextureSliceSet
{
    public TextureSliceSet(int width, int height, int arrayCount, int mipCount, ReadOnlyMemory<byte> rgba)
    {
        if (width <= 0) throw new ArgumentOutOfRangeException(nameof(width));
        if (height <= 0) throw new ArgumentOutOfRangeException(nameof(height));
        if (arrayCount <= 0) throw new ArgumentOutOfRangeException(nameof(arrayCount));
        if (mipCount <= 0) throw new ArgumentOutOfRangeException(nameof(mipCount));
        if (rgba.Length != width * height * 4)
            throw new ArgumentException("rgba buffer must be width*height*4 bytes", nameof(rgba));

        Width = width;
        Height = height;
        ArrayCount = arrayCount;
        MipCount = mipCount;
        Depth = 1;
        ArrayIndex = 0;
        MipIndex = 0;
        DepthIndex = 0;
        Rgba = rgba;
    }

    public int Width { get; }
    public int Height { get; }
    public int ArrayCount { get; }
    public int MipCount { get; }
    public int Depth { get; private set; }

    public int ArrayIndex { get; private set; }
    public int MipIndex { get; private set; }
    public int DepthIndex { get; private set; }

    public ReadOnlyMemory<byte> Rgba { get; }

    public TexturePixel PixelAt(int x, int y)
    {
        if ((uint)x >= (uint)Width || (uint)y >= (uint)Height)
            throw new ArgumentOutOfRangeException();
        int offset = (y * Width + x) * 4;
        var span = Rgba.Span;
        return new TexturePixel(span[offset + 0], span[offset + 1], span[offset + 2], span[offset + 3]);
    }

    public TextureSliceSet WithIndices(int arrayIndex, int mipIndex, int depthIndex)
    {
        if (arrayIndex < 0 || arrayIndex >= ArrayCount) throw new ArgumentOutOfRangeException(nameof(arrayIndex));
        if (mipIndex < 0 || mipIndex >= MipCount) throw new ArgumentOutOfRangeException(nameof(mipIndex));
        if (depthIndex < 0 || depthIndex >= Depth) throw new ArgumentOutOfRangeException(nameof(depthIndex));

        var copy = new TextureSliceSet(Width, Height, ArrayCount, MipCount, Rgba)
        {
            ArrayIndex = arrayIndex,
            MipIndex = mipIndex,
            DepthIndex = depthIndex,
        };
        return copy;
    }
}
