using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content;

[Collection("AssetRegistry")]
public sealed class V2ProjectDocumentCleanlinessTests
{
    [Fact]
    public void V2ProjectJson_DoesNotEmbedRuntimeIdsOrAbsoluteAssetPaths()
    {
        using var temp = new TemporaryProject();
        var projectPath = Path.Combine(temp.Path, "Project.primal");
        var project = Project.CreateNew("Clean", temp.Path);
        var scene = new Scene("Main");
        var entity = new GameEntity("Hero");
        entity.Components.Add(new Transform { PositionX = 1 });
        scene.Entities.Add(entity);
        project.Scenes.Add(scene);
        project.ActiveScene = scene;

        Serializer.SaveProject(project, projectPath);
        var json = File.ReadAllText(projectPath);

        Assert.DoesNotContain("AssetPath", json, StringComparison.Ordinal);
        Assert.DoesNotContain("EntityId", json, StringComparison.Ordinal);
        Assert.DoesNotContain("ContentId", json, StringComparison.Ordinal);
        Assert.DoesNotContain("RhiHandle", json, StringComparison.Ordinal);
        Assert.DoesNotContain("NativeHandle", json, StringComparison.Ordinal);
        Assert.DoesNotContain(temp.Path, json, StringComparison.Ordinal);
        Assert.DoesNotContain("/Users/", json, StringComparison.Ordinal);
        Assert.Contains("\"SchemaVersion\": 2", json, StringComparison.Ordinal);
    }
}
