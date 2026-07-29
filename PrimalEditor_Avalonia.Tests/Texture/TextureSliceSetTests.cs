using System;
using PrimalEditor_Avalonia.Editors.Texture;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Texture;

public sealed class TextureSliceSetTests
{
    private static byte[] Fill(int w, int h, byte value)
    {
        var data = new byte[w * h * 4];
        for (int i = 0; i < data.Length; i += 4)
        {
            data[i + 0] = value;
            data[i + 1] = value;
            data[i + 2] = value;
            data[i + 3] = 255;
        }
        return data;
    }

    [Fact]
    public void Constructor_RejectsZeroDimensions()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new TextureSliceSet(0, 0, 1, 1, Array.Empty<byte>()));
        Assert.Throws<ArgumentOutOfRangeException>(() => new TextureSliceSet(0, 0, 1, 0, Array.Empty<byte>()));
    }

    [Fact]
    public void Constructor_RejectsMismatchedBufferLength()
    {
        Assert.Throws<ArgumentException>(() => new TextureSliceSet(2, 2, 1, 1, new byte[3 * 4]));
    }

    [Fact]
    public void SliceExposesArrayZeroAndMipZeroByDefault()
    {
        var slice = new TextureSliceSet(2, 1, 1, 1, Fill(2, 1, 7));
        Assert.Equal(2, slice.Width);
        Assert.Equal(1, slice.Height);
        Assert.Equal(1, slice.ArrayCount);
        Assert.Equal(1, slice.MipCount);
        Assert.Equal(1, slice.Depth);
        Assert.Equal(0, slice.ArrayIndex);
        Assert.Equal(0, slice.MipIndex);
        Assert.Equal(0, slice.DepthIndex);
    }

    [Fact]
    public void RejectNegativeIndices()
    {
        var slice = new TextureSliceSet(1, 1, 1, 1, Fill(1, 1, 1));
        Assert.Throws<ArgumentOutOfRangeException>(() => slice.WithIndices(-1, 0, 0));
    }

    [Fact]
    public void RejectOutOfRangeIndices()
    {
        var slice = new TextureSliceSet(1, 1, 2, 2, Fill(1, 1, 1));
        Assert.Throws<ArgumentOutOfRangeException>(() => slice.WithIndices(2, 0, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => slice.WithIndices(0, 2, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => slice.WithIndices(0, 0, 2));
    }

    [Fact]
    public void PixelAtReturnsByte()
    {
        var data = Fill(1, 1, 9);
        data[0] = 11; data[1] = 22; data[2] = 33; data[3] = 44;
        var slice = new TextureSliceSet(1, 1, 1, 1, data);
        var px = slice.PixelAt(0, 0);
        Assert.Equal(11, px.R);
        Assert.Equal(22, px.G);
        Assert.Equal(33, px.B);
        Assert.Equal(44, px.A);
    }

    [Fact]
    public void RejectsMipLargerThanOnePixelWithoutMultipleSizes()
    {
        // When callers do not provide a mip chain, slice set is 1-mip only; we
        // must not pretend we have a mip2 (1x1) when source was 2x2.
        var slice = new TextureSliceSet(2, 2, 1, 1, Fill(2, 2, 1));
        Assert.Equal(1, slice.MipCount);
        Assert.Equal(2, slice.Width);
    }
}
