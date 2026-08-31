# Autosave Recovery Experience Plan

## Goal

Turn the existing production-loadable `.autosave/<project>_<timestamp>/` bundles into a safe editor recovery flow at startup and when opening a project.

## Product flow

1. Scan only direct, completed bundle directories under `<projectDir>/.autosave/`; ignore hidden `.partial-*` directories.
2. Validate the bundle manifest and every referenced Scene through the production serializers.
3. Offer only the newest valid, non-dismissed bundle that is newer than the authoritative project manifest and its referenced Scene files. If the live project is missing or invalid, a valid backup remains eligible.
4. Show a dedicated dialog with project name, backup time and paths, with three explicit choices:
   - **Recover Autosave**: load the backup into memory, rebase its `Name`/`Path` to the original project, and mark the project plus all scenes dirty. Do not overwrite authoritative files until the user invokes Save.
   - **Open Saved Version**: write a small dismissal marker inside that backup and open the authoritative project normally.
   - **Later**: make no filesystem or project-state change.
5. On application startup, scan recent projects and offer the newest eligible recovery once. When a user opens a project explicitly, check that selected project again.

## Architecture

- Add a UI-independent recovery service and candidate model under `Common/` or `GameProject/`.
- Keep discovery, validation, freshness comparison, dismissal and recovered-project loading independently testable.
- Add a dedicated Avalonia recovery dialog; do not reuse the save/discard `ConfirmDialog` labels.
- Return an explicit project-open result carrying whether the project came from autosave. Centralize assignment so recovered projects are marked dirty after `MainWindowViewModel.Project` completes its normal initialization.

## Safety constraints

- Never restore by directly overwriting the live `.primal` or `Scenes/` files.
- Never delete backup bundles from the recovery prompt.
- Never offer corrupt, incomplete, stale or dismissed bundles.
- Keep backup validation editor-only and independent of `EngineDLL`/GPU availability.
- Preserve existing autosave capture changes and unrelated dirty worktree files.

## Acceptance tests

- Newer valid backup is discovered; newest candidate wins.
- `.partial-*`, invalid, stale and dismissed backups are ignored.
- Recovered project contains unsaved Scene data, is rebased to the original path, and leaves authoritative files byte-for-byte unchanged before Save.
- Recovered project and all recovered Scenes are dirty after assignment to the main view model.
- Open-saved and Later decisions have the documented side effects.
- Existing autosave tests and the complete Avalonia test suite pass.

## Verification

```bash
dotnet test PrimalEditor_Avalonia.Tests/PrimalEditor_Avalonia.Tests.csproj --filter "FullyQualifiedName~AutoSaveRecovery|FullyQualifiedName~AutoSaveBackup" --no-restore
dotnet test PrimalEditor_Avalonia.Tests/PrimalEditor_Avalonia.Tests.csproj --no-restore
git diff --check
```
