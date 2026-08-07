using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public sealed class AutoSaveRecoveryTests : IDisposable
{
    public AutoSaveRecoveryTests()
    {
        AutoSaveService.Stop();
    }

    public void Dispose() => AutoSaveService.Stop();

    private static string CreateFreshProjectDir()
    {
        var dir = Path.Combine(
            Path.GetTempPath(),
            $"PrimalRecovery_{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        Directory.CreateDirectory(Path.Combine(dir, "Scenes"));
        return dir;
    }

    private static string ManifestPath(string projectDir) =>
        Path.Combine(projectDir, "p.primal");

    private static string WriteBundle(string projectDir, string projectName, string entityName, DateTime timestamp, string? collisionGuid = null)
    {
        var autosaveDir = Path.Combine(projectDir, ".autosave");
        Directory.CreateDirectory(autosaveDir);
        var bundleName = $"{projectName}_{timestamp:yyyyMMdd_HHmmssfff}" +
                         (collisionGuid != null ? "_" + collisionGuid : string.Empty);
        var bundleDir = Path.Combine(autosaveDir, bundleName);
        var bundleScenes = Path.Combine(bundleDir, "Scenes");
        Directory.CreateDirectory(bundleDir);
        Directory.CreateDirectory(bundleScenes);

        var project = new Project
        {
            Name = projectName,
            Path = bundleDir,
        };
        var scene = new Scene("s")
        {
            Guid = new SceneGuid(Guid.NewGuid())
        };
        scene.Entities.Add(new GameEntity(entityName));
        project.Scenes.Add(scene);
        project.ActiveScene = scene;

        var manifestPath = Path.Combine(bundleDir, projectName + Project.Extension);
        Serializer.SaveProject(project, manifestPath);

        var sceneGuidStr = scene.Guid.Value.ToString("D").ToLowerInvariant();
        var scenePath = Path.Combine(bundleScenes, sceneGuidStr + Scene.Extension);
        Serializer.SaveScene(scene, scenePath);

        return bundleDir;
    }

    private static (string ProjectDir, byte[] ManifestBytes, byte[][] SceneBytes) CreateLiveProject()
    {
        var temp = new TemporaryProject();
        try
        {
            var project = Project.CreateNew("p", temp.Path);
            var scene = new Scene("s");
            var entity = new GameEntity("live-entity");
            scene.Entities.Add(entity);
            project.Scenes.Add(scene);
            project.ActiveScene = scene;
            Serializer.SaveProject(project, temp.ProjectFilePath);
            NewProject.SaveScene(scene, project);

            var customPath = Path.Combine(temp.Path, "p.primal");
            File.Copy(temp.ProjectFilePath, customPath, overwrite: true);

            var manifestBytes = File.ReadAllBytes(customPath);
            var sceneDir = Path.Combine(temp.Path, "Scenes");
            var sceneFile = Directory.GetFiles(sceneDir, "*.scene").Single();
            var sceneBytes = File.ReadAllBytes(sceneFile);

            var persistent = Path.Combine(
                Path.GetTempPath(),
                $"PrimalRecovery_{Guid.NewGuid():N}");
            Directory.Move(temp.Path, persistent);

            temp.Dispose();
            return (persistent, manifestBytes, new[] { sceneBytes });
        }
        catch
        {
            temp.Dispose();
            throw;
        }
    }

    [Fact]
    public void DiscoverBundles_IgnoresHiddenPartialDirectories()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            var autosaveDir = Path.Combine(projectDir, ".autosave");
            Directory.CreateDirectory(autosaveDir);
            WriteBundle(projectDir, "p", "e1", DateTime.UtcNow);
            Directory.CreateDirectory(Path.Combine(autosaveDir, ".partial-abcd1234"));

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);

            Assert.Single(bundles);
            Assert.DoesNotContain(bundles, b => b.BundlePath.Contains(".partial-"));
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void DiscoverBundles_OrdersNewestFirstByParsedTimestamp()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            var older = DateTime.UtcNow.AddMinutes(-10);
            var newer = DateTime.UtcNow.AddMinutes(-5);
            var middle = DateTime.UtcNow.AddMinutes(-7);

            var olderBundle = WriteBundle(projectDir, "p", "older", older);
            var middleBundle = WriteBundle(projectDir, "p", "middle", middle);
            var newerBundle = WriteBundle(projectDir, "p", "newer", newer);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);

            Assert.Equal(3, bundles.Count);
            Assert.Equal(newerBundle, bundles[0].BundlePath);
            Assert.Equal(middleBundle, bundles[1].BundlePath);
            Assert.Equal(olderBundle, bundles[2].BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void DiscoverBundles_ParsesProjectNameWithUnderscoresAndCollisionSuffix()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            var ts = new DateTime(2026, 8, 4, 5, 6, 7, 123, DateTimeKind.Utc);
            var collisionGuid = "0123456789abcdef0123456789abcdef";

            var underscoreBundle = WriteBundle(projectDir, "game_alpha", "u1", ts);
            var collisionBundle = WriteBundle(projectDir, "p", "p1", ts, collisionGuid);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);

            Assert.Equal(2, bundles.Count);
            var underscore = bundles.Single(b => b.BundlePath == underscoreBundle);
            var collision = bundles.Single(b => b.BundlePath == collisionBundle);
            Assert.Equal("game_alpha", underscore.ProjectName);
            Assert.Equal(DateTimeKind.Utc, underscore.Timestamp.Kind);
            Assert.Equal(ts, underscore.Timestamp);
            Assert.Equal("p", collision.ProjectName);
            Assert.Equal(ts, collision.Timestamp);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void DiscoverBundles_RejectsBundleWithInvalidManifest()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            var autosaveDir = Path.Combine(projectDir, ".autosave");
            Directory.CreateDirectory(autosaveDir);
            var validBundle = WriteBundle(projectDir, "p", "good", DateTime.UtcNow);

            var broken = Path.Combine(autosaveDir, $"p_{DateTime.UtcNow.AddSeconds(1):yyyyMMdd_HHmmssfff}");
            Directory.CreateDirectory(broken);
            Directory.CreateDirectory(Path.Combine(broken, "Scenes"));
            File.WriteAllText(Path.Combine(broken, "p.primal"), "{ this is not valid json");

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);

            Assert.Single(bundles);
            Assert.Equal(validBundle, bundles[0].BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void DiscoverBundles_RejectsBundleWithCorruptScene()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            var autosaveDir = Path.Combine(projectDir, ".autosave");
            Directory.CreateDirectory(autosaveDir);
            var validBundle = WriteBundle(projectDir, "p", "good", DateTime.UtcNow);

            var broken = Path.Combine(autosaveDir, $"p_{DateTime.UtcNow.AddSeconds(1):yyyyMMdd_HHmmssfff}");
            Directory.CreateDirectory(broken);
            Directory.CreateDirectory(Path.Combine(broken, "Scenes"));
            var project = new Project { Name = "p", Path = broken };
            var scene = new Scene("s")
            {
                Guid = new SceneGuid(Guid.NewGuid())
            };
            scene.Entities.Add(new GameEntity("e"));
            project.Scenes.Add(scene);
            Serializer.SaveProject(project, Path.Combine(broken, "p.primal"));
            var scenePath = Path.Combine(broken, "Scenes",
                scene.Guid.Value.ToString("D").ToLowerInvariant() + Scene.Extension);
            File.WriteAllText(scenePath, "garbage");

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);

            Assert.Single(bundles);
            Assert.Equal(validBundle, bundles[0].BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void DiscoverBundles_RejectsDismissedBundle()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            var validBundle = WriteBundle(projectDir, "p", "good", DateTime.UtcNow);
            var dismissedBundle = WriteBundle(projectDir, "p", "dismissed",
                DateTime.UtcNow.AddMinutes(-1));
            AutoSaveRecoveryService.MarkDismissed(new AutoSaveRecoveryService.AutoSaveBundle
            {
                BundlePath = dismissedBundle,
                ProjectName = "p",
                Timestamp = DateTime.UtcNow.AddMinutes(-1),
            });

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);

            Assert.Single(bundles);
            Assert.Equal(validBundle, bundles[0].BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void SelectNewestEligible_ReturnsNewestNewerThanLiveProject()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var newerBundle = WriteBundle(projectDir, "p", "newer",
                DateTime.UtcNow.AddSeconds(1));

            File.SetLastWriteTimeUtc(newerBundle, DateTime.UtcNow.AddMinutes(1));

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var eligible = AutoSaveRecoveryService.SelectNewestEligible(
                ManifestPath(projectDir), bundles);

            Assert.NotNull(eligible);
            Assert.Equal(newerBundle, eligible!.BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void SelectNewestEligible_SkipsStaleBundles()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var staleBundle = WriteBundle(projectDir, "p", "stale",
                DateTime.UtcNow.AddMinutes(-30));
            File.SetLastWriteTimeUtc(staleBundle, DateTime.UtcNow.AddMinutes(-30));
            File.SetLastWriteTimeUtc(ManifestPath(projectDir), DateTime.UtcNow);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var eligible = AutoSaveRecoveryService.SelectNewestEligible(
                ManifestPath(projectDir), bundles);

            Assert.Null(eligible);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void SelectNewestEligible_AllowsValidBackupWhenLiveMissing()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            var bundle = WriteBundle(projectDir, "p", "live-missing",
                DateTime.UtcNow);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var eligible = AutoSaveRecoveryService.SelectNewestEligible(
                ManifestPath(projectDir), bundles);

            Assert.NotNull(eligible);
            Assert.Equal(bundle, eligible!.BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void SelectNewestEligible_AllowsValidBackupWhenLiveManifestIsInvalid()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            File.WriteAllText(ManifestPath(projectDir), "not a valid manifest");

            var bundle = WriteBundle(projectDir, "p", "live-corrupt",
                DateTime.UtcNow);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var eligible = AutoSaveRecoveryService.SelectNewestEligible(
                ManifestPath(projectDir), bundles);

            Assert.NotNull(eligible);
            Assert.Equal(bundle, eligible!.BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void SelectNewestEligible_AllowsValidBackupWhenLiveSceneMissing()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            var sourceProject = Project.CreateNew("p", projectDir);
            var sourceScene = new Scene("s")
            {
                Guid = new SceneGuid(Guid.NewGuid())
            };
            sourceScene.Entities.Add(new GameEntity("e"));
            sourceProject.Scenes.Add(sourceScene);
            sourceProject.ActiveScene = sourceScene;
            Serializer.SaveProject(sourceProject, ManifestPath(projectDir));
            NewProject.SaveScene(sourceScene, sourceProject);

            var scenePath = Directory.GetFiles(Path.Combine(projectDir, "Scenes"),
                "*.scene").Single();
            File.Move(scenePath, scenePath + ".bak", overwrite: true);
            File.SetLastWriteTimeUtc(ManifestPath(projectDir), DateTime.UtcNow.AddMinutes(-5));

            var bundle = WriteBundle(projectDir, "p", "scene-missing",
                DateTime.UtcNow);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var eligible = AutoSaveRecoveryService.SelectNewestEligible(
                ManifestPath(projectDir), bundles);

            Assert.NotNull(eligible);
            Assert.Equal(bundle, eligible!.BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void SelectNewestEligible_SortsInputsAndIgnoresDismissed()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var ts = DateTime.UtcNow;

            var oldest = WriteBundle(projectDir, "p", "old",
                ts.AddMinutes(-10));
            var middle = WriteBundle(projectDir, "p", "mid",
                ts.AddMinutes(-5));
            var newest = WriteBundle(projectDir, "p", "new",
                ts);

            File.SetLastWriteTimeUtc(newest, DateTime.UtcNow.AddMinutes(2));
            File.SetLastWriteTimeUtc(middle, DateTime.UtcNow.AddMinutes(1));
            File.SetLastWriteTimeUtc(oldest, DateTime.UtcNow);
            File.SetLastWriteTimeUtc(ManifestPath(projectDir), DateTime.UtcNow.AddMinutes(-2));

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var shuffled = bundles.OrderBy(b => b.BundlePath.Length).ToList();

            var eligible = AutoSaveRecoveryService.SelectNewestEligible(
                ManifestPath(projectDir), shuffled);

            Assert.NotNull(eligible);
            Assert.Equal(newest, eligible!.BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void SelectNewestEligible_IgnoresBundlesForOtherProjects()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var otherProjectBundle = WriteBundle(projectDir, "other", "o1",
                DateTime.UtcNow.AddSeconds(1));
            File.SetLastWriteTimeUtc(otherProjectBundle, DateTime.UtcNow.AddMinutes(2));

            var targetBundle = WriteBundle(projectDir, "p", "p1",
                DateTime.UtcNow.AddSeconds(2));
            File.SetLastWriteTimeUtc(targetBundle, DateTime.UtcNow.AddMinutes(1));

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var shuffled = bundles.OrderBy(b => b.BundlePath.Length).ToList();

            var eligible = AutoSaveRecoveryService.SelectNewestEligible(
                ManifestPath(projectDir), shuffled);

            Assert.NotNull(eligible);
            Assert.Equal(targetBundle, eligible!.BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void LoadBundle_ProducesProjectRebasedToOriginalPathWithUnsavedData()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var bundleDir = WriteBundle(projectDir, "p", "unsaved-name",
                DateTime.UtcNow);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var bundle = bundles.Single();
            var loaded = AutoSaveRecoveryService.LoadBundle(
                bundle, ManifestPath(projectDir));

            Assert.NotNull(loaded);
            Assert.Equal(projectDir, loaded!.Path);
            Assert.Equal("p", loaded.Name);
            Assert.Equal(ManifestPath(projectDir), loaded.FullPath);

            var loadedScene = Assert.Single(loaded.Scenes);
            var loadedEntity = Assert.Single(loadedScene.Entities);
            Assert.Equal("unsaved-name", loadedEntity.Name);
            Assert.Same(loadedScene, loadedEntity.ParentScene);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void LoadBundle_DoesNotOverwriteAuthoritativeFiles()
    {
        var (projectDir, manifestBytes, sceneBytes) = CreateLiveProject();
        try
        {
            var bundleDir = WriteBundle(projectDir, "p", "unsaved-name",
                DateTime.UtcNow);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var bundle = bundles.Single();

            var sceneFile = Directory.GetFiles(Path.Combine(projectDir, "Scenes"),
                "*.scene").Single();

            var loaded = AutoSaveRecoveryService.LoadBundle(
                bundle, ManifestPath(projectDir));
            Assert.NotNull(loaded);

            Assert.Equal(manifestBytes, File.ReadAllBytes(ManifestPath(projectDir)));
            Assert.Equal(sceneBytes.Single(), File.ReadAllBytes(sceneFile));
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void LoadBundle_ReturnsNullWhenSceneMissingAfterDiscovery()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var bundleDir = WriteBundle(projectDir, "p", "will-disappear",
                DateTime.UtcNow);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var bundle = bundles.Single();

            var sceneFile = Directory.GetFiles(
                Path.Combine(bundleDir, "Scenes"), "*.scene").Single();
            File.Delete(sceneFile);

            var loaded = AutoSaveRecoveryService.LoadBundle(
                bundle, ManifestPath(projectDir));

            Assert.Null(loaded);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void LoadBundle_ReturnsNullWhenSceneCorruptAfterDiscovery()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var bundleDir = WriteBundle(projectDir, "p", "will-corrupt",
                DateTime.UtcNow);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var bundle = bundles.Single();

            var sceneFile = Directory.GetFiles(
                Path.Combine(bundleDir, "Scenes"), "*.scene").Single();
            File.WriteAllText(sceneFile, "{ broken scene document");

            var loaded = AutoSaveRecoveryService.LoadBundle(
                bundle, ManifestPath(projectDir));

            Assert.Null(loaded);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void MarkDismissed_WritesOnlyMarkerInsideBundle()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            var bundle = WriteBundle(projectDir, "p", "e", DateTime.UtcNow);
            var manifestBytes = File.ReadAllBytes(Path.Combine(bundle, "p.primal"));
            var sceneFile = Directory.GetFiles(Path.Combine(bundle, "Scenes"),
                "*.scene").Single();
            var sceneBytes = File.ReadAllBytes(sceneFile);

            AutoSaveRecoveryService.MarkDismissed(new AutoSaveRecoveryService.AutoSaveBundle
            {
                BundlePath = bundle,
                ProjectName = "p",
                Timestamp = DateTime.UtcNow,
            });

            Assert.True(File.Exists(Path.Combine(bundle, AutoSaveRecoveryService.DismissalMarkerName)));
            Assert.Equal(manifestBytes,
                File.ReadAllBytes(Path.Combine(bundle, "p.primal")));
            Assert.Equal(sceneBytes, File.ReadAllBytes(sceneFile));
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void SelectNewestEligible_IgnoresBundleDismissedAfterDiscovery()
    {
        var projectDir = CreateFreshProjectDir();
        try
        {
            var bundlePath = WriteBundle(projectDir, "p", "e", DateTime.UtcNow);
            var discovered = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var bundle = Assert.Single(discovered);

            AutoSaveRecoveryService.MarkDismissed(bundle);

            var eligible = AutoSaveRecoveryService.SelectNewestEligible(
                ManifestPath(projectDir), discovered);
            Assert.Null(eligible);
            Assert.True(File.Exists(Path.Combine(bundlePath,
                AutoSaveRecoveryService.DismissalMarkerName)));
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void OpenProjectSelection_ReturnsMissingLivePathForRecoveryCheck()
    {
        var missingPath = Path.Combine(Path.GetTempPath(),
            $"missing_{Guid.NewGuid():N}.primal");
        var vm = new OpenProjectViewModel
        {
            SelectedProject = new RecentProjectEntry
            {
                Name = "missing",
                Path = missingPath,
            }
        };

        Assert.Equal(missingPath, vm.GetSelectedProjectPath());
        Assert.False(File.Exists(missingPath));
    }

    [Fact]
    public void AssignProject_MarksRecoveredProjectAndScenesDirty()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var bundleDir = WriteBundle(projectDir, "p", "dirty-entity",
                DateTime.UtcNow);

            var bundles = AutoSaveRecoveryService.DiscoverBundles(projectDir);
            var bundle = bundles.Single();
            var loaded = AutoSaveRecoveryService.LoadBundle(
                bundle, ManifestPath(projectDir));

            Assert.NotNull(loaded);
            Assert.False(loaded!.IsDirty);
            Assert.All(loaded.Scenes, s => Assert.False(s.IsDirty));

            var vm = new MainWindowViewModel();
            vm.AssignProject(loaded, markRecoveredDirty: true);

            Assert.True(vm.Project == loaded);
            Assert.True(loaded.IsDirty);
            Assert.NotEmpty(loaded.Scenes);
            Assert.All(loaded.Scenes, s => Assert.True(s.IsDirty));
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void AssignProject_DoesNotMarkNormalOpenProjectDirty()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var project = OpenProject.Open(ManifestPath(projectDir));

            var vm = new MainWindowViewModel();
            vm.AssignProject(project, markRecoveredDirty: false);

            Assert.NotNull(vm.Project);
            Assert.False(project!.IsDirty);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void ScanForRecoveryCandidates_ReturnsHighestPriorityCandidate()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var dismissedBundle = WriteBundle(projectDir, "p", "old",
                DateTime.UtcNow.AddSeconds(1));
            AutoSaveRecoveryService.MarkDismissed(new AutoSaveRecoveryService.AutoSaveBundle
            {
                BundlePath = dismissedBundle,
                ProjectName = "p",
                Timestamp = DateTime.UtcNow.AddSeconds(1),
            });

            var newer = WriteBundle(projectDir, "p", "newer",
                DateTime.UtcNow.AddSeconds(2));
            File.SetLastWriteTimeUtc(newer, DateTime.UtcNow.AddMinutes(1));

            var coordinator = new AutoSaveRecoveryCoordinator(
                new[] { ManifestPath(projectDir) });

            var recovery = coordinator.ScanForRecoveryCandidates();

            Assert.NotNull(recovery);
            Assert.Equal(newer, recovery!.Bundle.BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void ScanForRecoveryCandidates_RunsOncePerInstance()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var newer = WriteBundle(projectDir, "p", "newer",
                DateTime.UtcNow.AddSeconds(1));
            File.SetLastWriteTimeUtc(newer, DateTime.UtcNow.AddMinutes(1));

            var coordinator = new AutoSaveRecoveryCoordinator(
                new[] { ManifestPath(projectDir) });

            var first = coordinator.ScanForRecoveryCandidates();
            Assert.NotNull(first);

            Directory.Delete(newer, recursive: true);

            var second = coordinator.ScanForRecoveryCandidates();
            Assert.NotNull(second);
            Assert.Equal(first!.Bundle.BundlePath, second!.Bundle.BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void ScanForRecoveryCandidates_FindsRecentPathWithMissingLiveManifest()
    {
        var (projectDir, _, _) = CreateLiveProject();
        try
        {
            var bundle = WriteBundle(projectDir, "p", "live-missing",
                DateTime.UtcNow);
            File.Delete(ManifestPath(projectDir));

            var coordinator = new AutoSaveRecoveryCoordinator(
                new[] { ManifestPath(projectDir) });

            var recovery = coordinator.ScanForRecoveryCandidates();

            Assert.NotNull(recovery);
            Assert.Equal(bundle, recovery!.Bundle.BundlePath);
        }
        finally
        {
            try { Directory.Delete(projectDir, recursive: true); } catch { }
        }
    }
}
