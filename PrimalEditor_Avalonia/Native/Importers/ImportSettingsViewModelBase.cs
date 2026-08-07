using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Native.Importers;

public abstract class ImportSettingsViewModelBase : ViewModelBase
{
    public abstract IAssetImportSettings Settings { get; }
    public abstract string ImporterName { get; }
}
