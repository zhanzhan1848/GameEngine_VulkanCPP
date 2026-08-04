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
    public async Task SaveBackupAsync_WritesTo_ProjectLocalAutosaveDirectory_AndBundlesScenes()
    {
        using var temp = new TemporaryProject();
        var project = Project.CreateNew("p", temp.Path);
        var scene = new Scene("s");
        scene.Entities.Add(new GameEntity("e"));
        project.Scenes.Add(scene);
        // Serializer.SaveProject writes the .primal manifest only; we must
        // also write each .scene file via NewProject.SaveScene to give the
        // autosave bundler real material to copy.
        Serializer.SaveProject(project, temp.ProjectFilePath);
        NewProject.SaveScene(scene, project);

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
    }

    // OnTick must not call SaveBackupAsync more frequently than Interval.
    // We exercise OnTick by reflection so the test does not depend on the
    // Avalonia DispatcherTimer firing on a non-UI thread.
    [Fact]
    public void OnTick_Respects_Interval_Gate()
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
}
