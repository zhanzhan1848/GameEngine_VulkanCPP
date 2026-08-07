using System;
using System.IO;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public sealed class AutoSaveBackupTests : IDisposable
{
    public AutoSaveBackupTests()
    {
        AutoSaveService.Stop();
        AutoSaveService.Interval = TimeSpan.FromMilliseconds(50);
        AutoSaveService.DebounceDelay = TimeSpan.FromMilliseconds(10);
    }

    public void Dispose() => AutoSaveService.Stop();

    [Fact]
    public async Task SaveBackupAsync_CapturesUnsavedSceneState_AndPreservesDirtyFlags()
    {
        using var temp = new TemporaryProject();
        var project = Project.CreateNew("p", temp.Path);
        var scene = new Scene("s");
        var entity = new GameEntity("saved-name");
        scene.Entities.Add(entity);
        project.Scenes.Add(scene);
        Serializer.SaveProject(project, temp.ProjectFilePath);
        NewProject.SaveScene(scene, project);

        entity.Name = "unsaved-name";
        scene.MarkDirty();
        AutoSaveService.Track(project);
        project.MarkDirty();
        await AutoSaveService.SaveBackupAsync();

        var autosaveDir = Path.Combine(temp.Path, ".autosave");
        Assert.True(Directory.Exists(autosaveDir), $"autosave dir missing: {autosaveDir}");
        var bundles = Directory.GetDirectories(autosaveDir, "p_*");
        Assert.NotEmpty(bundles);
        Assert.True(File.Exists(Path.Combine(bundles[0], "p.primal")), "bundle must contain .primal");
        Assert.True(Directory.Exists(Path.Combine(bundles[0], "Scenes")), "bundle must contain Scenes/");
        var sceneFiles = Directory.GetFiles(Path.Combine(bundles[0], "Scenes"), "*.scene");
        Assert.NotEmpty(sceneFiles);

        var backupProject = Serializer.LoadProject(Path.Combine(bundles[0], "p.primal"));
        Assert.NotNull(backupProject);
        Serializer.LoadProjectScenes(backupProject!);
        var backupEntity = Assert.Single(Assert.Single(backupProject.Scenes).Entities);
        Assert.Equal("unsaved-name", backupEntity.Name);
        Assert.True(project.IsDirty, "autosave must not clear the live project dirty flag");
        Assert.True(scene.IsDirty, "autosave must not clear the live scene dirty flag");
        Assert.Empty(Directory.GetDirectories(autosaveDir, ".partial-*"));
    }

    [Fact]
    public async Task SaveBackupAsync_WhenSnapshotIsInvalid_LeavesNoPublishedOrPartialBundle()
    {
        using var temp = new TemporaryProject();
        var project = Project.CreateNew("p", temp.Path);
        var invalidScene = new Scene("invalid")
        {
            Guid = default
        };
        invalidScene.Entities.Add(new GameEntity("unsaved"));
        invalidScene.MarkDirty();
        project.Scenes.Add(invalidScene);
        project.MarkDirty();

        AutoSaveService.Track(project);
        await AutoSaveService.SaveBackupAsync();

        var autosaveDir = Path.Combine(temp.Path, ".autosave");
        if (Directory.Exists(autosaveDir))
        {
            Assert.Empty(Directory.GetDirectories(autosaveDir, "p_*"));
            Assert.Empty(Directory.GetDirectories(autosaveDir, ".partial-*"));
        }
        Assert.True(project.IsDirty);
        Assert.True(invalidScene.IsDirty);
    }

    // OnTick must not call SaveBackupAsync more frequently than Interval.
    // We exercise OnTick by reflection so the test does not depend on the
    // Avalonia DispatcherTimer firing on a non-UI thread.
    [Fact]
    public async Task OnTick_Respects_Interval_Gate()
    {
        using var temp = new TemporaryProject();
        var project = Project.CreateNew("p", temp.Path);
        var scene = new Scene("s");
        project.Scenes.Add(scene);
        Serializer.SaveProject(project, temp.ProjectFilePath);

        AutoSaveService.Track(project);
        AutoSaveService.Interval = TimeSpan.FromHours(1);
        AutoSaveService.DebounceDelay = TimeSpan.Zero;
        project.MarkDirty();

        var onTick = typeof(AutoSaveService).GetMethod(
            "OnTick",
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static);
        Assert.NotNull(onTick);

        // First tick: should run the backup (one bundle).
        onTick!.Invoke(null, new object?[] { null, EventArgs.Empty });
        await WaitForBundleCountAsync(temp.Path, expected: 1);
        var firstCount = Directory.Exists(Path.Combine(temp.Path, ".autosave"))
            ? Directory.GetDirectories(Path.Combine(temp.Path, ".autosave"), "p_*").Length
            : 0;
        Assert.Equal(1, firstCount);

        // Second tick immediately afterwards: Interval gate should suppress.
        project.MarkDirty();
        onTick.Invoke(null, new object?[] { null, EventArgs.Empty });
        var secondCount = Directory.GetDirectories(Path.Combine(temp.Path, ".autosave"), "p_*").Length;
        Assert.Equal(1, secondCount);
    }

    private static async Task WaitForBundleCountAsync(string projectPath, int expected)
    {
        var autosaveDir = Path.Combine(projectPath, ".autosave");
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (DateTime.UtcNow < deadline)
        {
            if (Directory.Exists(autosaveDir) &&
                Directory.GetDirectories(autosaveDir, "p_*").Length >= expected)
                return;
            await Task.Delay(25);
        }
    }
}
