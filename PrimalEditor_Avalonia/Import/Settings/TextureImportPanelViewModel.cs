using System;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Import.Settings;

public sealed class TextureImportPanelViewModel : ImportSettingsViewModelBase
{
    private readonly TextureImportSettings _settings;

    public TextureImportPanelViewModel(IAssetImportSettings defaults)
    {
        _settings = (TextureImportSettings)(defaults ?? new TextureImportSettings());
    }

    public override IAssetImportSettings Settings => _settings;
    public override string ImporterName => "TextureImporter";

    public System.Collections.Generic.IReadOnlyList<TextureDimension> AvailableDimensions { get; } =
        (TextureDimension[])Enum.GetValues(typeof(TextureDimension));
    public System.Collections.Generic.IReadOnlyList<TextureFormat> AvailableFormats { get; } =
        (TextureFormat[])Enum.GetValues(typeof(TextureFormat));
    public System.Collections.Generic.IReadOnlyList<TextureCompressionMode> AvailableCompressionModes { get; } =
        (TextureCompressionMode[])Enum.GetValues(typeof(TextureCompressionMode));

    public TextureDimension Dimension { get => _settings.Dimension; set { _settings.Dimension = value; OnPropertyChanged(); } }
    public int MipLevels { get => _settings.MipLevels; set { _settings.MipLevels = value; OnPropertyChanged(); } }
    public float AlphaThreshold { get => _settings.AlphaThreshold; set { _settings.AlphaThreshold = value; OnPropertyChanged(); } }
    public bool PreferBC7 { get => _settings.PreferBC7; set { _settings.PreferBC7 = value; OnPropertyChanged(); } }
    public TextureFormat OutputFormat { get => _settings.OutputFormat; set { _settings.OutputFormat = value; OnPropertyChanged(); } }
    public bool Compress { get => _settings.Compress; set { _settings.Compress = value; OnPropertyChanged(); } }
    public bool IsSRGB { get => _settings.IsSRGB; set { _settings.IsSRGB = value; OnPropertyChanged(); } }
    public TextureCompressionMode CompressionMode { get => _settings.CompressionMode; set { _settings.CompressionMode = value; OnPropertyChanged(); } }
    public int ArraySize { get => _settings.ArraySize; set { _settings.ArraySize = value; OnPropertyChanged(); } }
    public int Depth { get => _settings.Depth; set { _settings.Depth = value; OnPropertyChanged(); } }
    public bool IsNormalMap { get => _settings.IsNormalMap; set { _settings.IsNormalMap = value; OnPropertyChanged(); } }
}
