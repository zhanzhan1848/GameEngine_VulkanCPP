using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Editors.PrimitiveMesh;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Common;

public static class ImportSettingsDefaults
{
    public static IAssetImportSettings ResolveDefaults(
        IAssetImporterPlugin plugin,
        Project? project,
        IReadOnlyList<string> sampleFilePaths)
    {
        var fromProject = TryLoadFromProject(plugin, project);
        if (fromProject != null) return fromProject;

        var fromApp = TryLoadFromApp(plugin);
        if (fromApp != null) return fromApp;

        return (IAssetImportSettings)Activator.CreateInstance(plugin.SettingsType)!;
    }

    private static IAssetImportSettings? TryLoadFromProject(IAssetImporterPlugin plugin, Project? project)
    {
        if (project == null) return null;
        var node = ProjectEditorSettings.GetImporterSettings(project, ImporterNameOf(plugin));
        return node == null ? null : FromJson(plugin, node);
    }

    private static IAssetImportSettings? TryLoadFromApp(IAssetImporterPlugin plugin)
    {
        var node = EditorSettings.GetImporterSettings(ImporterNameOf(plugin));
        return node == null ? null : FromJson(plugin, node);
    }

    public static string ImporterNameOf(IAssetImporterPlugin p)
    {
        return p.SettingsType.Name switch
        {
            nameof(TextureImportSettings) => "TextureImporter",
            nameof(GeometryImportSettings) => "GeometryImporter",
            nameof(PrimitiveInitInfo) => "PrimitiveMeshImporter",
            _ => p.SettingsType.Name,
        };
    }

    public static IAssetImportSettings FromJson(IAssetImporterPlugin plugin, System.Text.Json.Nodes.JsonObject json)
    {
        var s = (IAssetImportSettings)Activator.CreateInstance(plugin.SettingsType)!;
        s.FromJson(json);
        return s;
    }
}
