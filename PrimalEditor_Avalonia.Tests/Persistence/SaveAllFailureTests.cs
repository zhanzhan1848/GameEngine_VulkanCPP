using System;
using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public sealed class SaveAllFailureTests
{
    [Fact]
    public void SaveAll_WhenAnySceneSaveFails_ReturnsFalse_AndKeepsIsDirty_AndRecordsFailures()
    {
        if (OperatingSystem.IsWindows()) return;

        using var temp = new TemporaryProject();
        var project = Project.CreateNew("p", temp.Path);
        var scene = new Scene("s");
        scene.Entities.Add(new GameEntity("e"));
        project.Scenes.Add(scene);
        // Mark the project dirty before SaveAll so the test distinguishes
        // 'SaveAll preserved dirty state' from 'SaveAll cleared dirty state'.
        project.MarkDirty();
        Assert.True(project.IsDirty);

        var sceneFilePath = Path.Combine(temp.ScenesPath, $"{scene.Guid.Value.ToString("D").ToLowerInvariant()}{Scene.Extension}");
        Directory.CreateDirectory(sceneFilePath);

        var ok = EditorSaveService.Instance.SaveAll(project);

        Assert.False(ok, "SaveAll must return false when scene save fails.");
        Assert.NotEmpty(EditorSaveService.LastSaveAllFailures);
        Assert.True(project.IsDirty, "Project must remain dirty when any scene failed.");
    }
}
