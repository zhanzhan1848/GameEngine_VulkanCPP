using PrimalEditor_Avalonia.Editors.Texture;
using Xunit;

namespace PrimalEditor_Avalonia.Tests;

public sealed class SmokeTests
{
    [Fact]
    public void TextureImportSettings_DefaultIsBC7()
    {
        var s = new TextureImportSettings();
        Assert.Equal(TextureFormat.BC7_UNORM, s.OutputFormat);
        Assert.True(s.Compress);
        Assert.True(s.IsSRGB);
    }
}
