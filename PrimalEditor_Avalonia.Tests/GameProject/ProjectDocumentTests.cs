using System;
using System.Text.Json;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Documents;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.GameProject;

public sealed class ProjectDocumentTests
{
    [Fact]
    public void SaveProject_WritesThinV2Manifest()
    {
        using var temp = new TemporaryProject();
        var project = Project.CreateNew("MyGame", temp.Path);
        var scene = new Scene("Main") { Guid = new SceneGuid(Guid.NewGuid()) };
        scene.Entities.Add(new GameEntity("Robot"));
        project.Scenes.Add(scene);
        project.ActiveScene = scene;
        project.BuildConfigName = "DebugEditor";

        temp.SaveProject(project);

        using var json = JsonDocument.Parse(temp.ReadProjectJson());
        var root = json.RootElement;
        Assert.Equal(2, root.GetProperty("SchemaVersion").GetInt32());
        Assert.Equal("MyGame", root.GetProperty("Name").GetString());
        Assert.Equal("DebugEditor", root.GetProperty("BuildConfig").GetString());
        Assert.Equal(scene.Guid.Value.ToString("D"), root.GetProperty("DefaultScene").GetString());
        Assert.Single(root.GetProperty("Scenes").EnumerateArray());
        Assert.False(root.TryGetProperty("Path", out _));
        Assert.False(root.TryGetProperty("ActiveScene", out _));
        Assert.DoesNotContain("Entities", root.GetRawText(), StringComparison.Ordinal);
    }

    [Fact]
    public void LoadProject_DerivesRuntimePathAndNameFromOpenedFile()
    {
        using var temp = new TemporaryProject();
        var file = System.IO.Path.Combine(temp.Path, "Renamed.primal");
        var document = new ProjectDocument
        {
            ProjectGuid = new ProjectGuid(Guid.NewGuid()),
            Name = "LogicalName",
            BuildConfig = "ReleaseEditor"
        };
        System.IO.File.WriteAllText(file, JsonSerializer.Serialize(document, ProjectDocument.JsonOptions));

        var loaded = Serializer.LoadProject(file);

        Assert.NotNull(loaded);
        Assert.Equal(temp.Path, loaded!.Path);
        Assert.Equal("Renamed", loaded.Name);
        Assert.Equal("ReleaseEditor", loaded.BuildConfigName);
    }
}
