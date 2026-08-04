using System;
using System.IO;
using System.Text.Json;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.GameProject;

public sealed class PersistenceBoundaryTests
{
    private static readonly string FixturesPath = Path.Combine(AppContext.BaseDirectory, "Fixtures");

    private static readonly string V1ProjectFixture = Path.Combine(
        FixturesPath, "Projects", "V1", "EmbeddedScenes.primal");

    [Fact]
    public void ProjectManifest_DoesNotEmbedSceneEntities()
    {
        using var tempProject = new TemporaryProject();
        var project = Project.CreateNew("ManifestTest", tempProject.Path);
        project.Scenes.Add(new Scene("Main")
        {
            Entities =
            {
                new GameEntity("Robot")
            }
        });

        tempProject.SaveProject(project);

        using var document = JsonDocument.Parse(tempProject.ReadProjectJson());
        var root = document.RootElement;
        Assert.True(root.TryGetProperty("Scenes", out var scenes));
        Assert.Equal(JsonValueKind.Array, scenes.ValueKind);
        Assert.DoesNotContain("Entities", scenes.GetRawText(), StringComparison.Ordinal);
        Assert.DoesNotContain("Components", scenes.GetRawText(), StringComparison.Ordinal);
    }

    [Fact]
    public void PersistedSourceData_DoesNotContainRuntimeIdentifiers()
    {
        using var tempProject = new TemporaryProject();
        var project = Project.CreateNew("RuntimeBoundaryTest", tempProject.Path);
        project.Scenes.Add(new Scene("Main")
        {
            Entities = { new GameEntity("Entity") }
        });

        tempProject.SaveProject(project);

        var json = tempProject.ReadProjectJson();
        Assert.DoesNotContain("EntityId", json, StringComparison.Ordinal);
        Assert.DoesNotContain("ContentId", json, StringComparison.Ordinal);
        Assert.DoesNotContain("RhiHandle", json, StringComparison.Ordinal);
        Assert.DoesNotContain("RuntimeMaterialId", json, StringComparison.Ordinal);
        Assert.DoesNotContain("NativeHandle", json, StringComparison.Ordinal);
    }

    [Fact]
    public void PersistedSourceData_DoesNotContainAbsolutePaths()
    {
        using var tempProject = new TemporaryProject();
        var project = Project.CreateNew("PathBoundaryTest", tempProject.Path);
        project.Scenes.Add(new Scene("Main"));

        tempProject.SaveProject(project);

        var json = tempProject.ReadProjectJson();
        Assert.DoesNotContain(tempProject.Path, json, StringComparison.Ordinal);
        Assert.DoesNotContain("/Users/", json, StringComparison.Ordinal);
        Assert.DoesNotContain("C:\\", json, StringComparison.Ordinal);
    }

    [Fact]
    public void LegacyFixture_ContainsEmbeddedSceneAndAssetPathForMigrationCoverage()
    {
        Assert.True(File.Exists(V1ProjectFixture));
        var json = File.ReadAllText(V1ProjectFixture);
        Assert.Contains("\"Entities\"", json, StringComparison.Ordinal);
        Assert.Contains("\"AssetPath\"", json, StringComparison.Ordinal);
    }
}
