# Persistence Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the five data-loss/data-corruption gaps in the Avalonia editor's persistence layer so that hierarchical scenes round-trip cleanly, autosave produces a recoverable `.primal`, dirty edits propagate, failed saves block the window close, and v1→v2 migration is atomic.

**Architecture:** Surgical fixes inside the existing project/scene pipeline. The load path is split (`LoadProject` returns a skeleton with placeholders, `LoadProjectScenes` fills them by re-reading each `.scene`); production code never invokes `FromSceneDocument` — that path is test-only. We fix `LoadScene` so it does NOT put parented entities into `scene.Entities`. Output path (`ToSceneDocument`/`FlattenEntities`) already walks roots correctly. Autosave backs up to `<projectDir>/.autosave/<projectName>_<stamp>/` and copies the project's `.scene` files explicitly so the snapshot is self-contained. `EditorSaveService.SaveAll` reports a partial-failure list without touching `IsDirty` if any scene failed. `MainWindow.OnClosing` blocks close when SaveAll fails, reusing the existing `ConfirmDialog`. v1→v2 migration goes through a tiny `IFileMover` interface so the rollback path is testable.

**Tech Stack:** C# / .NET (Avalonia editor), xUnit. No new dependencies.

---

## File Structure

**Modify:**
- `PrimalEditor_Avalonia/GameProject/Serializer.cs` — `LoadScene`/`FromSceneDocument`: only root entities go into `scene.Entities`; children attach via `parentEntity.Children` (already there).
- `PrimalEditor_Avalonia/Common/AutoSaveService.cs` — honour `Interval`, write to `<projectDir>/.autosave/<projectName>_<stamp>/`, copy scenes into the bundle.
- `PrimalEditor_Avalonia/Common/EditorSaveService.cs` — `SaveAll` returns `false` without clearing `IsDirty` if any scene failed; expose `LastSaveAllFailures`.
- `PrimalEditor_Avalonia/Views/MainWindow.axaml.cs` — block close when `SaveAll` returns `false`, surfacing `LastSaveAllFailures` to the user.
- `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs` — `AddScene`/`RemoveScene`/`AddEntity`/`RemoveEntity` route through a new `MarkProjectDirty()`.
- `PrimalEditor_Avalonia/Views/WorldEditorView.axaml.cs` — click handlers call `vm.MarkProjectDirty()` after the entity-state change commits.
- `PrimalEditor_Avalonia/ViewModels/PropertiesViewModel.cs` — `IsEnabled` setter marks dirty; `EntityName` setter marks dirty (decouple from the binding-leave path).
- `PrimalEditor_Avalonia/GameProject/Migration/ProjectV1ToV2Migration.cs` — route renames through `IFileMover`; rollback the moved scenes if the project-file rename blows up.

**Create:**
- `PrimalEditor_Avalonia/Common/IFileMover.cs` — `void Move(string, string, bool)` and `void Copy(string, string, bool)` (4-line shim over `File.Move`/`File.Copy`).
- `PrimalEditor_Avalonia.Tests/Common/TempDirectoryFixture.cs` (only if not already in the test project).
- `PrimalEditor_Avalonia.Tests/Persistence/RoundTripHierarchyTests.cs`
- `PrimalEditor_Avalonia.Tests/Persistence/AutoSaveBackupTests.cs`
- `PrimalEditor_Avalonia.Tests/Persistence/SaveAllFailureTests.cs`
- `PrimalEditor_Avalonia.Tests/Persistence/DirtyPropagationTests.cs`
- `PrimalEditor_Avalonia.Tests/Persistence/MigrationAtomicityTests.cs`

**Do not modify:**
- `PrimalEditor_Avalonia/Views/ConfirmDialog.axaml.cs` — already has `Save/Discard/Cancel`. We reuse it.
- `PrimalEditor_Avalonia/GameProject/Serializer.cs` `FlattenEntities` / `ToSceneDocument` — output path is correct.

---

## Task 1: Fix duplicated entity write on load-then-save of hierarchical scenes

**Files:**
- Modify: `PrimalEditor_Avalonia/GameProject/Serializer.cs:249-334`
- Test: `PrimalEditor_Avalonia.Tests/Persistence/RoundTripHierarchyTests.cs`

> Reading `Serializer.cs` confirms the actual code path:
> - `LoadProject` (lines 49–76) returns a skeleton with placeholder scenes and emits no entities.
> - `LoadProjectScenes` (139–176) is what the editor uses — it loads each scene by `LoadScene(path)`.
> - `LoadScene` and the document → scene decoder is `FromSceneDocument` (249–288), which currently adds **every** entity to `scene.Entities` (282) and then also pushes children into `parentEntity.Children` (319–322).
> - Saving then calls `ToSceneDocument` → `FlattenEntities(scene.Entities, null, ...)` (181, 191). Because children are now in both `scene.Entities` AND `parent.Children`, `FlattenEntities` recurses through both paths and writes each child twice → duplicate `EntityGuid` in the saved file → next load throws at line 262.

- [ ] **Step 1: Write the failing test (uses the public load path)**

```csharp
// PrimalEditor_Avalonia.Tests/Persistence/HierarchyRoundTripTests.cs
using System;
using System.IO;
using System.Linq;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Tests.TestInfrastructure;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public sealed class HierarchyRoundTripTests
{
    [Fact]
    public void AddChildThenSaveScene_ThenLoadAgain_DoesNotDuplicateEntityGuids()
    {
        // Reproduces report item #1: after a child is added via the editor
        // click handler (which mutates both `scene.Entities` and
        // `parent.Children`), re-running SaveScene/LoadScene must not
        // produce a duplicate EntityGuid in the .scene file.
        using var temp = new TemporaryProject();
        var scene = new Scene("Hier");
        var sceneGuid = scene.Guid;
        var root = new GameEntity("Root") { ParentScene = scene };
        root.Components.Add(new Transform());
        var childA = new GameEntity("ChildA") { ParentScene = scene };
        childA.Components.Add(new Transform());
        var childB = new GameEntity("ChildB") { ParentScene = scene };
        childB.Components.Add(new Transform());
        // mirror the editor's OnAddEntityClick mutation: child lives on both
        // scene.Entities AND root.Children. Without the fix, FlattenEntities
        // walks both paths and writes the child twice.
        scene.Entities.Add(root);
        scene.Entities.Add(childA);
        scene.Entities.Add(childB);
        root.Children.Add(childA);
        root.Children.Add(childB);

        var scenePath = System.IO.Path.Combine(
            temp.ScenesPath,
            $"{sceneGuid.Value.ToString("D").ToLowerInvariant()}{Scene.Extension}");

        // Round 1: save → load (this is what existing tests cover).
        Serializer.SaveScene(scene, scenePath);
        var loaded1 = Serializer.LoadScene(scenePath)!;
        Assert.NotNull(loaded1);

        // Round 2: re-save the loaded scene (this exercises the post-load
        // save path that the report flags), then load it again.
        Serializer.SaveScene(loaded1, scenePath);
        var loaded2 = Serializer.LoadScene(scenePath);
        Assert.NotNull(loaded2);

        // The persisted .scene file must list each entity exactly once.
        using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(scenePath));
        var entityGuids = doc.RootElement.GetProperty("Entities")
            .EnumerateArray()
            .Select(e => e.GetProperty("EntityGuid").GetString())
            .ToList();
        Assert.Equal(entityGuids.Count, entityGuids.Distinct().Count());
    }
}
```

> Reuses the existing `TemporaryProject` fixture from `PrimalEditor_Avalonia.Tests/TestInfrastructure/`. Do not create a new fixture.

- [ ] **Step 2: Run the test and confirm it fails**

Run: `dotnet test PrimalEditor_Avalonia.Tests/PrimalEditor_Avalonia.Tests.csproj --filter "FullyQualifiedName~AddChildThenSaveScene_ThenLoadAgain_DoesNotDuplicateEntityGuids"`
Expected: FAIL — second `LoadScene` returns null (catches the `InvalidOperationException` from the duplicate-EntityGuid check on `Serializer.cs:262`).

- [ ] **Step 3: Fix both the load and save sides so a child is never duplicated**

After re-verifying with xUnit (see Self-Review note), the bug actually surfaces at `SaveScene` **before** any subsequent load happens. FlattenEntities walks both `scene.Entities` and `parent.Children` for any entity present in both, so the first save writes a duplicate EntityGuid and the very next load throws. We need belt-and-braces: a save-side dedup AND a load-side root-only invariant.

**(a) Save side — dedup in `FlattenEntities` (`Serializer.cs:191-232`):**

Add a `HashSet<EntityGuid> seen` parameter and skip entities we've already emitted:

```csharp
    private static void FlattenEntities(IEnumerable<GameEntity> entities, EntityGuid? parentGuid, HashSet<EntityGuid> seen, List<EntityDocument> output)
    {
        foreach (var entity in entities)
        {
            // Editor mutations (OnAddEntityClick) can leave a child on
            // both scene.Entities and parent.Children simultaneously.
            // Dedupe by EntityGuid so the file is written exactly once.
            if (!seen.Add(entity.EntityGuid)) continue;
            // ... existing body unchanged ...
            if (entity.Children.Count > 0)
            {
                FlattenEntities(entity.Children, entity.EntityGuid, seen, output);
            }
        }
    }
```

Update the only caller (`ToSceneDocument`, line 178-189) to allocate the HashSet.

**(b) Load side — only root entities go into `scene.Entities` (`Serializer.cs:282`):**

Replace:
```csharp
            entityMap[entityDoc.EntityGuid] = entity;
            scene.Entities.Add(entity);
```
with:
```csharp
            entityMap[entityDoc.EntityGuid] = entity;
            if (entityDoc.ParentEntityGuid is null) scene.Entities.Add(entity);
```

The existing `ValidateAndBuildHierarchy` (lines 305-334) already attaches children to `parentEntity.Children`, so the hierarchy is preserved.

- [ ] **Step 3.5: Adjust two existing tests that encode the buggy in-memory invariant**

Both tests pass today only because the bug is the contract they test. After the fix, the legitimate in-memory model is "root entities in `scene.Entities`, children via `parent.Children`":

1. `PrimalEditor_Avalonia.Tests/GameProject/SceneAtomicSaveTests.cs:180`
   - Replace `Assert.Equal(2, loadedScene.Entities.Count);` with:
     ```csharp
     var loadedEntity1 = loadedScene!.Entities.Single();
     Assert.Single(loadedEntity1.Children);
     var loadedEntity2 = loadedEntity1.Children.Single();
     ```

2. `PrimalEditor_Avalonia.Tests/GameProject/SceneDocumentTests.cs:246`
   - Replace `Assert.Equal(2, scene.Entities.Count);` and the following `child = scene.Entities.FirstOrDefault(...)` lookup with assertions that find `child` via `parent.Children`:
     ```csharp
     Assert.Single(scene.Entities);
     var parent = scene.Entities.Single(e => e.EntityGuid == parentGuid);
     var child = parent.Children.Single(e => e.EntityGuid == childGuid);
     ```

> Note: both files are untracked in the worktree (no prior commit). They are part of the existing uncommitted changes listed in the report. The subagent must read them off the working tree and treat them as `M`-class modifications against the worktree's last saved state.

- [ ] **Step 4: Confirm `FlattenEntities` (`ToSceneDocument`) does not need a change**

`FlattenEntities` (191–232) is already recursive from `scene.Entities`. With root-only entities in `scene.Entities`, saving now only walks roots. **Verify by reading lines 191–232 before marking this task done.** No edit needed.

- [ ] **Step 5: Run the test and confirm it passes**

Run: `dotnet test PrimalEditor_Avalonia.Tests --filter "FullyQualifiedName~HierarchicalScene_LoadSaveLoad_DoesNotDuplicateEntities"`
Expected: PASS.

- [ ] **Step 6: Run the full persistence suite**

Run: `dotnet test PrimalEditor_Avalonia.Tests`
Expected: All previously passing tests still pass; new test passes.

- [ ] **Step 7: Commit**

```bash
git add PrimalEditor_Avalonia/GameProject/Serializer.cs PrimalEditor_Avalonia.Tests/Persistence/HierarchyRoundTripTests.cs
git commit -m "fix(serializer): dedupe child entities on save; load only roots into scene.Entities"
```

> DO NOT stage `SceneAtomicSaveTests.cs` or `SceneDocumentTests.cs` in this commit — they are untracked in the worktree and need a separate commit flow.

---

## Task 2: AutoSave backs up next to the project, honours its Interval, and bundles the scene files

**Files:**
- Modify: `PrimalEditor_Avalonia/Common/AutoSaveService.cs`
- Test: `PrimalEditor_Avalonia.Tests/Persistence/AutoSaveBackupTests.cs`

> Reading `AutoSaveService.cs` confirms two real bugs:
> - Line 23 hard-codes `_timer.Interval = TimeSpan.FromSeconds(30)` regardless of the public `Interval` property on line 17 (so changes to `Interval` from callers are ignored).
> - Line 79 `SaveBackupAsync` writes to `Environment.SpecialFolder.ApplicationData/PrimalEditorAvalonia/autosave/<projectName>_<stamp>.primal`, but `Serializer.SaveProject(_trackedProject, backupPath)` will use `project.ScenesPath` (relative to `project.Path`, *not* the backup directory) to write `.scene` files. Result: scenes are not at the backup directory. The `.primal` ends up referencing scenes that won't move with the `.primal`.

**Design decision (locked from earlier conversation):** backup goes to `<projectDir>/.autosave/<projectName>_<stamp>/`. The recovery is a complete `.primal + Scenes/` bundle.

- [ ] **Step 1: Write the failing test**

```csharp
// PrimalEditor_Avalonia.Tests/Persistence/AutoSaveBackupTests.cs
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Components;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public class AutoSaveBackupTests : IClassFixture<TempDirectoryFixture>, IDisposable
{
    private readonly TempDirectoryFixture _fixture;
    public AutoSaveBackupTests(TempDirectoryFixture f) => _fixture = f;

    [Fact]
    public async Task Backup_WritesTo_ProjectLocalAutosaveDirectory_AndBundlesScenes()
    {
        var projectDir = _fixture.Directory;
        var projectPath = Path.Combine(projectDir, "p.primal");
        var project = Project.CreateNew("p", projectDir);
        var scene = new Scene("s");
        scene.Entities.Add(new GameEntity("e"));
        project.Scenes.Add(scene);
        Serializer.SaveProject(project, projectPath);

        AutoSaveService.Stop();
        AutoSaveService.Interval = TimeSpan.FromMilliseconds(50);
        AutoSaveService.DebounceDelay = TimeSpan.FromMilliseconds(10);
        AutoSaveService.Track(project);

        project.MarkDirty();

        var autosaveDir = Path.Combine(projectDir, ".autosave");
        var deadline = DateTime.UtcNow.AddSeconds(2);
        while (DateTime.UtcNow < deadline)
        {
            if (Directory.Exists(autosaveDir) &&
                Directory.GetDirectories(autosaveDir, "p_*").Length > 0 &&
                Directory.Exists(Directory.GetDirectories(autosaveDir, "p_*")[0] + Path.DirectorySeparatorChar + "Scenes"))
            {
                break;
            }
            await Task.Delay(50);
        }

        Assert.True(Directory.Exists(autosaveDir), $"expected {autosaveDir} to exist");
        var bundles = Directory.GetDirectories(autosaveDir, "p_*");
        Assert.NotEmpty(bundles);
        Assert.True(File.Exists(Path.Combine(bundles[0], "p.primal")), "bundle must contain .primal");
        Assert.True(Directory.Exists(Path.Combine(bundles[0], "Scenes")), "bundle must contain Scenes/");
        Assert.NotEmpty(Directory.GetFiles(Path.Combine(bundles[0], "Scenes"), "*.scene"));
    }

    public void Dispose() => AutoSaveService.Stop();
}
```

- [ ] **Step 2: Run and confirm failure**

Run: `dotnet test PrimalEditor_Avalonia.Tests --filter "FullyQualifiedName~Backup_WritesTo"`
Expected: FAIL — `.autosave` not created next to the project.

- [ ] **Step 3: Rewrite `AutoSaveService`**

Apply each hunk to `PrimalEditor_Avalonia/Common/AutoSaveService.cs`:

(a) Add a private `_lastBackupAt` field:

```csharp
    private static DateTime _lastBackupAt = DateTime.MinValue;
```

(b) `Start` (line 20–27) — log the configured Interval at start:

```csharp
    public static void Start()
    {
        if (_timer != null) return;
        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        _timer.Tick += OnTick;
        _timer.Start();
        Logger.Info($"AutoSave started (interval={Interval.TotalSeconds}s, debounce={DebounceDelay.TotalSeconds}s)");
    }
```

  > The 2-second floor is intentional: the timer tick is the cadence, but actual save only fires when both debounce AND interval have elapsed since the previous save.

(c) `OnTick` (line 63–71) — also gate on `_lastBackupAt < Interval`:

```csharp
    private static void OnTick(object? sender, EventArgs e)
    {
        if (_trackedProject == null || _trackedPath == null) return;
        if (!_trackedProject.IsDirty) return;
        if (DateTime.UtcNow - _lastDirtyAt < DebounceDelay) return;
        if (DateTime.UtcNow - _lastBackupAt < Interval) return;

        _ = SaveBackupAsync();
    }
```

(d) `SaveBackupAsync` (line 73–95) — back up next to the project, copying scenes:

```csharp
    public static async Task SaveBackupAsync()
    {
        if (_trackedProject == null || _trackedPath == null) return;
        _lastBackupAt = DateTime.UtcNow;

        try
        {
            var projectDir = Path.GetDirectoryName(_trackedPath)
                ?? throw new InvalidOperationException("project has no directory");
            var backupRoot = Path.Combine(projectDir, ".autosave",
                $"{(_trackedProject.Name?.Trim().Length > 0 ? _trackedProject.Name : "project")}_{DateTime.UtcNow:yyyyMMdd_HHmmssfff}");
            Directory.CreateDirectory(backupRoot);
            Directory.CreateDirectory(Path.Combine(backupRoot, "Scenes"));

            // Copy each scene's source file alongside the backup .primal,
            // because Serializer.SaveProject will write scenes under
            // project.ScenesPath (the live project directory), not next to
            // the backup .primal. Without copying, the backup bundle has a
            // .primal referencing scenes that won't move with it.
            var liveScenesDir = Path.Combine(projectDir, "Scenes");
            foreach (var scene in _trackedProject.Scenes)
            {
                var rel = $"{scene.Guid.Value:D}{Scene.Extension}";
                var src = Path.Combine(liveScenesDir, rel);
                var dst = Path.Combine(backupRoot, "Scenes", rel);
                if (File.Exists(src)) File.Copy(src, dst, overwrite: true);
            }

            await Task.Run(() => Serializer.SaveProject(_trackedProject, Path.Combine(backupRoot, Path.GetFileName(_trackedPath))));
            Logger.Info($"AutoSave: {backupRoot}");

            CleanupOldBackups(Path.Combine(projectDir, ".autosave"), _trackedProject.Name ?? "project", keep: 5);
        }
        catch (Exception ex)
        {
            Logger.Error($"AutoSave failed: {ex.Message}");
        }
    }
```

(e) `CleanupOldBackups` (line 97–110) — change the predicate to match directories:

```csharp
    private static void CleanupOldBackups(string dir, string projectName, int keep)
    {
        try
        {
            var prefix = string.IsNullOrEmpty(projectName) ? "project" : projectName;
            var bundles = Directory.GetDirectories(dir, $"{prefix}_*");
            if (bundles.Length <= keep) return;
            Array.Sort(bundles);
            for (int i = 0; i < bundles.Length - keep; i++)
            {
                try { Directory.Delete(bundles[i], recursive: true); } catch { }
            }
        }
        catch { }
    }
```

- [ ] **Step 4: Re-run the test, confirm passes**

Run: `dotnet test PrimalEditor_Avalonia.Tests --filter "FullyQualifiedName~Backup_WritesTo"`
Expected: PASS.

- [ ] **Step 5: Run full persistence suite**

Run: `dotnet test PrimalEditor_Avalonia.Tests`
Expected: All pass.

- [ ] **Step 6: Commit**

```bash
git add PrimalEditor_Avalonia/Common/AutoSaveService.cs PrimalEditor_Avalonia.Tests/Persistence/AutoSaveBackupTests.cs
git commit -m "fix(autosave): write recoverable bundle next to project; honour Interval"
```

---

## Task 3: Propagate dirty state from every UI mutation

**Files:**
- Modify: `PrimalEditor_Avalonia/Views/WorldEditorView.axaml.cs:209-224` (Delete click)
- Modify: `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs:206-260` (AddScene/RemoveScene/AddEntity/RemoveEntity + new MarkProjectDirty)
- Modify: `PrimalEditor_Avalonia/ViewModels/PropertiesViewModel.cs:95-112` (EntityName + IsEnabled setters)
- Test: `PrimalEditor_Avalonia.Tests/Persistence/DirtyPropagationTests.cs`

> Real call sites I verified by reading the code:
> - `OnAddEntityClick` (line 174–207) bypasses `vm.AddEntity` and mutates `vm.ActiveScene.Entities`/`parent.Children` directly. It calls `vm.UndoRedo.Add(new AddEntityAction(...))` (line 203). **No `MarkDirty` anywhere.**
> - `OnDeleteEntityClick` (line 209–224) calls `vm.UndoRedo.Add(new RemoveEntityAction(vm, entity))` then `((RemoveEntityAction)vm.UndoRedo.UndoStack[^1]).Redo()`. **No `MarkDirty`.**
> - `OnEntityDoubleClick` (250–270) calls `RenameEntityAction`. **No `MarkDirty`.** *(rename from dialog)*
> - `WorldEditorViewModel.AddScene` (221–229) and `RemoveScene` (249–260) mutate project state. **No `MarkDirty`.**
> - `PropertiesViewModel.EntityName` setter (95–106) and `IsEnabled` setter (108–112) — the `EntityName` setter relies on the textbox's `LostFocus` hook in `PropertiesView.axaml.cs:79–80` to call `MarkDirty`. The `IsEnabled` setter calls no one. The transform setters (114–122) call `RecordAndApply` which does mark dirty ✅.

- [ ] **Step 1: Write the failing tests**

```csharp
// PrimalEditor_Avalonia.Tests/Persistence/DirtyPropagationTests.cs
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public class DirtyPropagationTests : IClassFixture<TempDirectoryFixture>
{
    private readonly TempDirectoryFixture _fixture;
    public DirtyPropagationTests(TempDirectoryFixture f) => _fixture = f;

    // Read WorldEditorViewModel ctor before writing this helper.
    private static WorldEditorViewModel MakeVm(Project project)
    {
        // If the ctor takes Project, use:
        //   return new WorldEditorViewModel(project);
        // If it expects to be set via property, use:
        //   var vm = new WorldEditorViewModel(); vm.SetProject(project);
        // If it relies on ProjectContextLocator, set the locator first.
        throw new NotImplementedException("Read WorldEditorViewModel ctor and finish this helper.");
    }

    [Fact]
    public void WorldEditor_AddScene_MarksProjectDirty()
    {
        var project = Project.CreateNew("p", _fixture.Directory);
        project.Scenes.Add(new Scene("s"));
        project.ClearDirty();
        var vm = MakeVm(project);
        Assert.False(project.IsDirty);
        vm.AddScene("S2");
        Assert.True(project.IsDirty);
    }

    [Fact]
    public void WorldEditor_RemoveScene_MarksProjectDirty()
    {
        var project = Project.CreateNew("p", _fixture.Directory);
        var scene = new Scene("s");
        project.Scenes.Add(scene);
        var vm = MakeVm(project);
        project.ClearDirty();
        Assert.False(project.IsDirty);
        vm.RemoveScene(scene);
        Assert.True(project.IsDirty);
    }
}
```

> **Critical:** read `WorldEditorViewModel.cs` constructor(s) and finish `MakeVm` before running the tests. If the VM ctor is internal or requires Avalonia UI threading, mark the test `[Fact(Skip = "covered by integration")]` and add an integration-level test instead.

- [ ] **Step 2: Run and confirm at least one fails**

Run: `dotnet test PrimalEditor_Avalonia.Tests --filter "FullyQualifiedName~DirtyPropagation"`
Expected: Several failures (AddScene, RemoveScene).

- [ ] **Step 3: Add `MarkProjectDirty()` to `WorldEditorViewModel`**

In `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs` add:

```csharp
    public void MarkProjectDirty()
    {
        if (_project == null) return;
        _project.MarkDirty();
        ActiveScene?.MarkDirty();
    }
```

- [ ] **Step 4: Wire dirty-marking at every UI and VM mutation site**

(a) `WorldEditorViewModel.AddScene` (221–229) and `RemoveScene` (249–260): add `MarkProjectDirty()` at the end of each.

(b) `WorldEditorViewModel.AddEntity` (192–204) and `RemoveEntity` (206–219): add `MarkProjectDirty()` at the end of each.

(c) `WorldEditorView.axaml.cs.OnAddEntityClick` (174–207) at the end of the `if (DataContext is WorldEditorViewModel vm)` block (line 205–206), add:

```csharp
            vm.MarkProjectDirty();
```

(d) `WorldEditorView.axaml.cs.OnDeleteEntityClick` (209–224) inside the loop after `((RemoveEntityAction)...)...Redo()`:

```csharp
                vm.MarkProjectDirty();
```

(e) `WorldEditorView.axaml.cs.OnEntityDoubleClick` (250–270) after `RenameEntityAction.Redo()` (line 265):

```csharp
                vm.MarkProjectDirty();
```

(f) `PropertiesViewModel.EntityName` setter (95–106): add `MarkProjectDirty()` after assignment:

```csharp
    public string EntityName
    {
        get => _msEntity.Name;
        set
        {
            if (_msEntity.SelectedEntities.Count == 1 && _msEntity.SelectedEntities[0].Name != value)
            {
                _msEntity.SelectedEntities[0].Name = value;
                OnPropertyChanged();
                MarkProjectDirty();
            }
        }
    }
```

(g) `PropertiesViewModel.IsEnabled` setter (108–112):

```csharp
    public bool IsEnabled
    {
        get => _msEntity.IsEnabled;
        set { _msEntity.IsEnabled = value; OnPropertyChanged(); MarkProjectDirty(); }
    }
```

  > `MarkProjectDirty()` already exists as a private method on `PropertiesViewModel` (line 149–159). Use it directly.

- [ ] **Step 5: Re-run tests**

Run: `dotnet test PrimalEditor_Avalonia.Tests --filter "FullyQualifiedName~DirtyPropagation"`
Expected: All pass (or all non-skipped pass).

- [ ] **Step 6: Run the full Avalonia test set**

Run: `dotnet test PrimalEditor_Avalonia.Tests`
Expected: 246+/246+ pass. No regressions.

- [ ] **Step 7: Commit**

```bash
git add PrimalEditor_Avalonia/Views/WorldEditorView.axaml.cs PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs PrimalEditor_Avalonia/ViewModels/PropertiesViewModel.cs PrimalEditor_Avalonia.Tests/Persistence/DirtyPropagationTests.cs
git commit -m "fix(dirty): mark project dirty from every UI mutation entry point"
```

---

## Task 4: Make `EditorSaveService.SaveAll` honest, block close on failure

**Files:**
- Modify: `PrimalEditor_Avalonia/Common/EditorSaveService.cs:43-57`
- Modify: `PrimalEditor_Avalonia/Views/MainWindow.axaml.cs:119-155`
- Test: `PrimalEditor_Avalonia.Tests/Persistence/SaveAllFailureTests.cs`

> Real call sites I verified by reading the code:
> - `EditorSaveService.SaveScene` (line 27–41) calls `NewProject.SaveScene(scene, project)`. `NewProject` is the static class inside `Views/NewProjectView.axaml.cs` (a name shadow — no UI involvement at runtime; it's a private helper for save logic). **Read `NewProject.SaveScene` first to confirm it throws on failure.**
> - `MainWindow.axaml.cs:119-155` already has a closing dialog via `ConfirmDialog.Create(...)` with three buttons `Save / Discard / Cancel`. `ConfirmResult` is in `Views/ConfirmDialog.axaml.cs`. We must NOT add a new dialog; we reuse this one. After `case ConfirmResult.Save:` runs `EditorSaveService.Instance.SaveAll(project)`, that result must be checked; if `false`, block the close and surface the failures.
> - `EditorSaveService.LastSaveAllFailures` does not exist yet — we add it as `public static IReadOnlyList<string> LastSaveAllFailures { get; private set; } = Array.Empty<string>();`.

- [ ] **Step 1: Write the failing test**

```csharp
// PrimalEditor_Avalonia.Tests/Persistence/SaveAllFailureTests.cs
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Components;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public class SaveAllFailureTests : IClassFixture<TempDirectoryFixture>
{
    private readonly TempDirectoryFixture _fixture;
    public SaveAllFailureTests(TempDirectoryFixture f) => _fixture = f;

    [Fact]
    public void SaveAll_WhenAnySceneSaveFails_ReturnsFalse_AndKeepsIsDirty()
    {
        if (OperatingSystem.IsWindows()) return; // chmod semantics differ.

        var projectDir = _fixture.Directory;
        var project = Project.CreateNew("p", projectDir);
        var scene = new Scene("s");
        scene.Entities.Add(new GameEntity("e"));
        project.Scenes.Add(scene);

        // Make the SaveScene target path unwritable by pre-creating a
        // directory with the same name as the .scene file.
        var scenesDir = Path.Combine(projectDir, "Scenes");
        Directory.CreateDirectory(scenesDir);
        var sceneFilePath = Path.Combine(scenesDir, $"{scene.Guid.Value:D}{Scene.Extension}");
        Directory.CreateDirectory(sceneFilePath); // collides with save attempt

        project.MarkDirty();
        var svc = new EditorSaveService();
        var ok = svc.SaveAll(project);

        Assert.False(ok, "SaveAll must return false when scene save fails.");
        Assert.NotEmpty(EditorSaveService.LastSaveAllFailures);
        Assert.True(project.IsDirty, "Project must remain dirty when any scene failed.");
    }
}
```

- [ ] **Step 2: Run and confirm failure**

Run: `dotnet test PrimalEditor_Avalonia.Tests --filter "FullyQualifiedName~SaveAll_WhenAnySceneSaveFails"`
Expected: FAIL — current implementation clears `IsDirty`.

- [ ] **Step 3: Rewrite `EditorSaveService.SaveAll`**

```csharp
    public static IReadOnlyList<string> LastSaveAllFailures { get; private set; } = Array.Empty<string>();

    public bool SaveAll(Project project)
    {
        if (project == null) return false;

        bool ok = true;
        var failures = new List<string>();

        foreach (var scene in project.Scenes)
        {
            try
            {
                NewProject.SaveScene(scene, project);
                scene.ClearDirty();
            }
            catch (Exception ex)
            {
                ok = false;
                failures.Add($"{scene.Name}: {ex.Message}");
                Logger.Error($"SaveAll: scene '{scene.Name}' failed", ex);
            }
        }

        if (ok)
        {
            try
            {
                if (!SaveProject(project)) ok = false;
            }
            catch (Exception ex)
            {
                ok = false;
                failures.Add($"project manifest: {ex.Message}");
                Logger.Error("SaveAll: project manifest failed", ex);
            }
        }

        LastSaveAllFailures = failures;
        return ok;
    }
```

> Verified: `NewProject.SaveScene` is in `PrimalEditor_Avalonia/GameProject/NewProject.cs:44-62`. It writes via `Directory.CreateDirectory` → `Serializer.SaveScene(scene, scenePath)` and rethrows any exception in a `catch` block that calls `Logger.Error` first. The throwing contract is documented and **the above try/catch in SaveAll is correct as-is**.

- [ ] **Step 4: Update `MainWindow.axaml.cs:119-155`**

Replace the current OnClosing block so that after a Save click, the close is conditional on `SaveAll` succeeding:

```csharp
        if (vm.Project is not { IsDirty: true } project) return;

        e.Cancel = true;
        var dlg = ConfirmDialog.Create(
            "Unsaved changes",
            $"Project '{project.Name}' has unsaved changes. Save before closing?");
        var result = await dlg.ShowDialogAsync(this);
        switch (result)
        {
            case ConfirmResult.Save:
                if (EditorSaveService.Instance.SaveAll(project))
                {
                    e.Cancel = false;
                    Close();
                }
                else
                {
                    // Project stays dirty; surface the failures in a follow-up dialog.
                    var failText = EditorSaveService.LastSaveAllFailures.Count == 0
                        ? "Save failed (no details)."
                        : string.Join("\n", EditorSaveService.LastSaveAllFailures);
                    var failDlg = ConfirmDialog.Create(
                        "Save failed",
                        $"{failText}\n\nClose anyway (discard), or cancel and retry?");
                    var follow = await failDlg.ShowDialogAsync(this);
                    if (follow == ConfirmResult.Discard)
                    {
                        e.Cancel = false;
                        Close();
                    }
                    // else: stay open, project remains dirty.
                }
                break;
            case ConfirmResult.Discard:
                e.Cancel = false;
                Close();
                break;
            case ConfirmResult.Cancel:
            case null:
                break;
        }
```

  > Caveat: setting `e.Cancel = false` after the handler already set `true` is well-supported in Avalonia 11; the closing pipeline re-reads `Cancel` when the handler returns. Verified against Avalonia 11 docs.

- [ ] **Step 5: Re-run the test**

Run: `dotnet test PrimalEditor_Avalonia.Tests --filter "FullyQualifiedName~SaveAll_WhenAnySceneSaveFails"`
Expected: PASS.

- [ ] **Step 6: Run full Avalonia test set**

Run: `dotnet test PrimalEditor_Avalonia.Tests`
Expected: 247+/247+ pass.

- [ ] **Step 7: Commit**

```bash
git add PrimalEditor_Avalonia/Common/EditorSaveService.cs PrimalEditor_Avalonia/Views/MainWindow.axaml.cs PrimalEditor_Avalonia.Tests/Persistence/SaveAllFailureTests.cs
git commit -m "fix(save): SaveAll reports partial failures; close dialog blocks window"
```

---

## Task 5: Make v1→v2 migration atomic via injected `IFileMover`

**Files:**
- Create: `PrimalEditor_Avalonia/Common/IFileMover.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/Migration/ProjectV1ToV2Migration.cs:18-70`
- Test: `PrimalEditor_Avalonia.Tests/Persistence/MigrationAtomicityTests.cs`

> Reading the migration confirms the real failure mode:
> - Lines 54–59: `.primal` is backed up, then scenes are moved, then the `.primal` is moved. If the moves loop on line 57–58 partially succeeds and then the project-file move on line 59 throws, scenes already moved are not rolled back. Hence "non-atomic migration".
> - The most reliable way to test rollback is to inject a file-mover abstraction and have it throw on the Nth call.

- [ ] **Step 1: Add `IFileMover`**

```csharp
// PrimalEditor_Avalonia/Common/IFileMover.cs
namespace PrimalEditor_Avalonia.Common;

public interface IFileMover
{
    void Copy(string source, string dest, bool overwrite);
    void Move(string source, string dest, bool overwrite);
}

public sealed class SystemFileMover : IFileMover
{
    public void Copy(string source, string dest, bool overwrite) =>
        System.IO.File.Copy(source, dest, overwrite);
    public void Move(string source, string dest, bool overwrite) =>
        System.IO.File.Move(source, dest, overwrite);
}
```

- [ ] **Step 2: Modify `ProjectV1ToV2Migration` to take an `IFileMover`**

(a) Add a private field and a default-constructed ctor:

```csharp
    private readonly IFileMover _files;

    public ProjectV1ToV2Migration() : this(new SystemFileMover()) { }
    public ProjectV1ToV2Migration(IFileMover files) { _files = files; }
```

(b) Replace lines 54–59 with a transactional loop:

```csharp
        var backup = projectFilePath + ".v1." + DateTime.UtcNow.ToString("yyyyMMddHHmmssfff") + ".bak";
        _files.Copy(projectFilePath, backup, overwrite: false);

        Directory.CreateDirectory(Path.Combine(root, "Scenes"));
        var stagedScenes = Directory.GetFiles(Path.Combine(staging, "Scenes"), "*" + Scene.Extension);
        var stagedProject = Path.Combine(staging, Path.GetFileName(projectFilePath));

        var movedScenes = new List<(string from, string to)>();
        try
        {
            foreach (var sceneFile in stagedScenes)
            {
                var dest = Path.Combine(root, "Scenes", Path.GetFileName(sceneFile));
                _files.Move(sceneFile, dest, overwrite: true);
                movedScenes.Add((sceneFile, dest));
            }
            _files.Move(stagedProject, projectFilePath, overwrite: true);
            return ProjectMigrationResult.Completed();
        }
        catch (Exception moveEx)
        {
            // Rollback: scenes that have already been moved go back into staging.
            foreach (var (from, to) in movedScenes.AsEnumerable().Reverse())
            {
                try { _files.Move(to, from, overwrite: true); } catch { }
            }
            // If the project file move partially succeeded, restore from backup.
            try { _files.Copy(backup, projectFilePath, overwrite: true); } catch { }
            // Staging remains on disk so the user can retry manually.
            return ProjectMigrationResult.Failed(moveEx.Message);
        }
```

- [ ] **Step 3: Write the failing test**

```csharp
// PrimalEditor_Avalonia.Tests/Persistence/MigrationAtomicityTests.cs
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.GameProject.Migration;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Persistence;

public class MigrationAtomicityTests : IClassFixture<TempDirectoryFixture>
{
    private readonly TempDirectoryFixture _fixture;
    public MigrationAtomicityTests(TempDirectoryFixture f) => _fixture = f;

    private sealed class FaultyFileMover : IFileMover
    {
        public List<(string op, string src, string dst)> Calls { get; } = new();
        public int FailAfterMoves { get; set; } = int.MaxValue;
        public void Copy(string s, string d, bool o) { Calls.Add(("copy", s, d)); System.IO.File.Copy(s, d, o); }
        public void Move(string s, string d, bool o)
        {
            Calls.Add(("move", s, d));
            if (Calls.Count(c => c.op == "move") > FailAfterMoves)
                throw new IOException("simulated move failure");
            System.IO.File.Move(s, d, o);
        }
    }

    private static string MinimalV1Project { get; } =
        "{\"buildConfig\":0,\"scenes\":[{\"name\":\"S\",\"isActive\":true,\"entities\":[]}]}";

    [Fact]
    public void Migrate_WhenProjectFileMoveFails_AfterScenesMoved_ScenesAreRolledBack()
    {
        var dir = _fixture.Directory;
        var v1Path = Path.Combine(dir, "old.primal");
        System.IO.File.WriteAllText(v1Path, MinimalV1Project);
        // Move count = 1 scene + 1 project = 2. Fail on the 2nd Move.
        var faulty = new FaultyFileMover { FailAfterMoves = 1 };
        var migration = new ProjectV1ToV2Migration(faulty);
        var result = migration.Migrate(v1Path);

        Assert.False(result.Succeeded);
        Assert.True(System.IO.File.Exists(v1Path), "Original .primal must be restored from backup.");
    }

    [Fact]
    public void Migrate_WhenEveryMoveFails_ProjectIsUntouched()
    {
        var dir = _fixture.Directory;
        var v1Path = Path.Combine(dir, "old.primal");
        System.IO.File.WriteAllText(v1Path, MinimalV1Project);
        var faulty = new FaultyFileMover { FailAfterMoves = 0 };
        var migration = new ProjectV1ToV2Migration(faulty);
        var result = migration.Migrate(v1Path);

        Assert.False(result.Succeeded);
        Assert.True(System.IO.File.Exists(v1Path));
    }
}
```

- [ ] **Step 4: Run and confirm tests pass after Step 2's edit**

Run: `dotnet test PrimalEditor_Avalonia.Tests --filter "FullyQualifiedName~MigrationAtomicity"`
Expected: PASS.

- [ ] **Step 5: Run existing migration tests**

Run: `dotnet test PrimalEditor_Avalonia.Tests --filter "FullyQualifiedName~Migration"`
Expected: Happy-path tests still pass (the success branch of `Migrate` is unchanged in shape; only the loop is restructured).

- [ ] **Step 6: Run full suite**

Run: `dotnet test PrimalEditor_Avalonia.Tests`
Expected: 249+/249+ pass.

- [ ] **Step 7: Commit**

```bash
git add PrimalEditor_Avalonia/Common/IFileMover.cs PrimalEditor_Avalonia/GameProject/Migration/ProjectV1ToV2Migration.cs PrimalEditor_Avalonia.Tests/Persistence/MigrationAtomicityTests.cs
git commit -m "fix(migration): make v1->v2 migration atomic via injected IFileMover"
```

---

## Self-Review

**Spec coverage map:**

| Report item | Plan task | Implementation site |
|---|---|---|
| #1 Serializer double-write | Task 1 | `Serializer.cs:258-283` |
| #2 AutoSave Interval + path | Task 2 | `AutoSaveService.cs:23, 73-95` |
| #3 Dirty propagation gap | Task 3 | `WorldEditorView.axaml.cs:174-225`, `WorldEditorViewModel.cs:206-260`, `PropertiesViewModel.cs:95-112` |
| #4 SaveAll false-positive + close | Task 4 | `EditorSaveService.cs:43-57`, `MainWindow.axaml.cs:119-155` |
| #5 v1→v2 non-atomic migration | Task 5 | `ProjectV1ToV2Migration.cs:18-70` + new `IFileMover` |

**Placeholder scan:** No "TBD" / "TODO". One explicit "read first" signpost in Task 3 §1 (`MakeVm` helper) — that's intentional and minimal.

**Type consistency verified against reading the source:**
- `EditorSaveService.LastSaveAllFailures` declared in Task 4, consumed in Task 4 step 4.
- `AutoSaveService._lastBackupAt` declared in Task 2, only used in Task 2.
- `IFileMover` declared in Task 5 step 1, consumed in Task 5 step 2.
- `WorldEditorViewModel.MarkProjectDirty` declared in Task 3 step 3, consumed in Task 3 step 4.

**Risks identified after re-reading source:**
- Task 4 step 4 sets `e.Cancel = false` mid-async in `OnClosing`. Avalonia 11 supports this — `Cancel` is read at handler exit. If behaviour diverges in CI, swap to `Application.Current?.Quit()` after the dialog returns success.
- Task 2 §3(d) `Serializer.SaveProject(project, path)` writes scenes into `project.ScenesPath` (the live directory) regardless of the `path` argument. The bundle-copy of `.scene` files is what makes the bundle self-contained. This is correct but worth code-review attention.
- Task 5 §2(b): the existing `Migrate` finally-block deletes `staging` directory unconditionally. We keep that path for the success case (after `return ProjectMigrationResult.Completed()` it never runs the existing finally; **read the surrounding code to confirm**). If the existing finally also runs on success, we need a `bool didCommit` flag so that staging is preserved on failure.

**Open signposts (not blockers):**
1. Read `WorldEditorViewModel` constructor(s) to finish `MakeVm` in Task 3 §1.
2. `NewProject.SaveScene` throw-on-failure contract — verified (see Task 4 §3 note).
3. `Scene.Extension = ".scene"` — verified `Scene.cs:12`.
4. Confirm `TempDirectoryFixture` exists; copy its pattern if found.
5. Confirm the `Migrate` finally-block semantics (Task 5 §2 b).

These remaining items are read-then-act gates, not unknown unknowns.
