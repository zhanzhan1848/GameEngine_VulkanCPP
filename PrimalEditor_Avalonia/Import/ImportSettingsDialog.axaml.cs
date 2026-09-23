using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Import.Settings;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Import;

public partial class ImportSettingsDialog : Window
{
    private IAssetImportSettings? _acceptedSettings;
    private bool _accepted;

    public string PluginName { get; }
    public string HeaderLine { get; }
    public UserControl PanelContent { get; }

    public ImportSettingsDialog() { InitializeComponent(); }

    public ImportSettingsDialog(IAssetImporterPlugin plugin, IReadOnlyList<string> files, IAssetImportSettings defaults)
    {
        InitializeComponent();
        PluginName = plugin.DisplayName;
        HeaderLine = files.Count == 1
            ? $"Import {System.IO.Path.GetFileName(files[0])}"
            : $"Import {files.Count} files ({plugin.DisplayName})";
        _acceptedSettings = defaults;
        PanelContent = plugin.CreatePanel(defaults);

        // The geometry panel gates pipeline-section visibility on whether any
        // of the selected files is a format ContentTools' pipeline path
        // supports. Other plugins' VMs don't have this concept, so we only
        // touch GeometryImportPanelViewModel.
        if (PanelContent.DataContext is GeometryImportPanelViewModel geoVm)
        {
            geoVm.PipelineAvailable = HasAnySupportedPipelineFormat(files);
        }

        DataContext = this;
    }

    private static bool HasAnySupportedPipelineFormat(IReadOnlyList<string> files)
    {
        for (int i = 0; i < files.Count; ++i)
        {
            var ext = Path.GetExtension(files[i]).ToLowerInvariant();
            // .gltf/.glb: ContentTools glTF 2.0 importer (plain glTF — no Draco).
            if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") return true;
        }
        return false;
    }

    public static async Task<(bool accepted, IAssetImportSettings? settings)> ShowAsync(
        Window owner, IAssetImporterPlugin plugin, IReadOnlyList<string> files, IAssetImportSettings defaults)
    {
        var dlg = new ImportSettingsDialog(plugin, files, defaults);
        var tcs = new TaskCompletionSource<(bool, IAssetImportSettings?)>();
        dlg.Closed += (_, _) => tcs.TrySetResult(dlg._accepted
            ? (true, dlg._acceptedSettings)
            : (false, null));
        await dlg.ShowDialog(owner);
        return await tcs.Task;
    }

    private void OnAccept(object? sender, RoutedEventArgs e)
    {
        _accepted = true;
        if (PanelContent.DataContext is ImportSettingsViewModelBase vm) _acceptedSettings = vm.Settings;
        Close();
    }

    private void OnCancel(object? sender, RoutedEventArgs e) => Close();
}
