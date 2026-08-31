namespace PrimalEditor_Avalonia.Editors.Texture;

public class TextureSlice
{
    public int Width { get; set; }
    public int Height { get; set; }
    public int RowPitch { get; set; }
    public int SlicePitch { get; set; }
    public byte[] RawContent { get; set; } = System.Array.Empty<byte>();
}

public enum TextureFlags { None = 0, IsVolumeMap = 1, IsCubeMap = 2 }

public class TextureInfo
{
    public int Width { get; set; }
    public int Height { get; set; }
    public int ArraySize { get; set; } = 1;
    public int MipLevels { get; set; } = 1;
    public int Depth { get; set; } = 1;
    public TextureFormat Format { get; set; }
    public TextureFlags Flags { get; set; }
    public bool IsNormalMap { get; set; }

    public string Summary => $"{Width}x{Height} arr={ArraySize} mip={MipLevels} depth={Depth} fmt={Format}";
}