using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Migration;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.GameProject;

public sealed class ProjectV1ToV2MigrationTests
{
    private static readonly string Fixture = Path.Combine(AppContext.BaseDirectory, "Fixtures", "Projects", "V1", "EmbeddedScenes.primal");

    [Fact]
    public void Migration_ExtractsScenesAndWritesThinManifestWithBackup()
    {
        using var temp = new TemporaryProject();
        temp.CopyFixture(Fixture, "TestProject.primal");

        var result = new ProjectMigrationService().MigrateIfNeeded(temp.ProjectFilePath);

        Assert.True(result.Success, result.ErrorMessage);
        using var manifest = JsonDocument.Parse(File.ReadAllText(temp.ProjectFilePath));
        Assert.Equal(2, manifest.RootElement.GetProperty("SchemaVersion").GetInt32());
        Assert.DoesNotContain("Entities", manifest.RootElement.GetRawText(), StringComparison.Ordinal);
        Assert.Single(Directory.GetFiles(temp.ScenesPath, "*.scene"));
        Assert.Single(Directory.GetFiles(temp.Path, "TestProject.primal.v1.*.bak"));
        var scene = Serializer.LoadScene(Directory.GetFiles(temp.ScenesPath, "*.scene").Single());
        Assert.NotNull(scene);
        Assert.Equal("MainScene", scene!.Name);
        Assert.Single(scene.Entities);
        Assert.NotEqual(Guid.Empty, scene.Entities[0].EntityGuid.Value);
    }

    [Fact]
    public void Migration_IsIdempotent()
    {
        using var temp = new TemporaryProject();
        temp.CopyFixture(Fixture, "TestProject.primal");
        var service = new ProjectMigrationService();
        Assert.True(service.MigrateIfNeeded(temp.ProjectFilePath).Success);
        var firstManifest = File.ReadAllText(temp.ProjectFilePath);
        var firstScene = File.ReadAllText(Directory.GetFiles(temp.ScenesPath, "*.scene").Single());

        var second = service.MigrateIfNeeded(temp.ProjectFilePath);

        Assert.True(second.Success);
        Assert.False(second.Migrated);
        Assert.Equal(firstManifest, File.ReadAllText(temp.ProjectFilePath));
        Assert.Equal(firstScene, File.ReadAllText(Directory.GetFiles(temp.ScenesPath, "*.scene").Single()));
    }
}
