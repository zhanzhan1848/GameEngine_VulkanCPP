# Avalonia Editor Remaining Gaps Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Complete the remaining macOS-relevant Avalonia editor gaps in the approved order while preserving current serialization formats and deferring Windows/Linux viewport hosts.

**Architecture:** Add a focused xUnit test project first, then evolve each feature behind testable services and data models. Texture and Geometry changes preserve existing sidecars with append-only readers; BuildService delegates platform choices to a process-command resolver; Content Browser and Inspector keep filesystem/component logic outside views; Docking is integrated last after panel lifecycles stabilize.

**Tech Stack:** .NET 8, C# 12, Avalonia 11, xUnit, System.Diagnostics.Process, existing native ContentTools/EngineDLL C ABI, JSON and binary sidecars.

---

## Constraints

- Do not implement `WindowsD3D12ViewportHost` or `LinuxVulkanViewportHost`.
- Preserve the verified macOS Metal viewport behavior.
- Preserve existing project/scene JSON and `.meta` sidecars; new binary fields are append-only and optional on read.
- Use TDD for every behavior change: write one failing test, observe the expected failure, implement minimally, rerun focused and full tests.
- Do not suppress type errors or delete failing tests.
- Do not commit unless the user explicitly requests further commits. The current baseline has already been committed.

## Task 0: Add Headless Test Infrastructure

**Files:**
- Create: `PrimalEditor_Avalonia.Tests/PrimalEditor_Avalonia.Tests.csproj`
- Create: `PrimalEditor_Avalonia.Tests/SmokeTests.cs`

**Steps:**
1. Create an xUnit project targeting the same framework as `PrimalEditor_Avalonia` and reference the editor project.
2. Add one smoke test that instantiates a pure model such as `TextureImportSettings`.
3. Run `dotnet test PrimalEditor_Avalonia.Tests/PrimalEditor_Avalonia.Tests.csproj` and verify one passing test.
4. Run `dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor` and preserve zero warnings/errors.

## Task 1: Separate Texture Normal Map and sRGB State

**Files:**
- Modify: `PrimalEditor_Avalonia/Editors/Texture/TextureImportSettings.cs`
- Modify: `PrimalEditor_Avalonia/Editors/Texture/TextureEditorViewModel.cs`
- Test: `PrimalEditor_Avalonia.Tests/Texture/TextureImportSettingsTests.cs`
- Test: `PrimalEditor_Avalonia.Tests/Texture/TextureEditorViewModelTests.cs`

**Steps:**
1. Write a failing binary round-trip test where `IsNormalMap=true` and `IsSRGB=false` remain independent.
2. Write a failing compatibility test that loads a pre-`IsNormalMap` payload and defaults `IsNormalMap` to false.
3. Append `IsNormalMap` to `TextureImportSettings.ToBinary`; read it only if bytes remain.
4. Write a failing VM test showing toggling `IsNormalMap` does not change `IsSRGB`, including Undo/Redo.
5. Change `TextureEditorViewModel.IsNormalMap` and `RebuildPreview` to use `Settings.IsNormalMap`.
6. Ensure `Discard` notifies both `IsNormalMap` and `IsSRGB`.
7. Run focused tests, full tests, build, and serialization verification.

## Task 2: Back Texture Slice Selection with Real Decoded Data

**Files:**
- Create: `PrimalEditor_Avalonia/Editors/Texture/TextureSliceSet.cs`
- Create: `PrimalEditor_Avalonia/Editors/Texture/ITextureSliceDecoder.cs`
- Create: `PrimalEditor_Avalonia/Editors/Texture/AvaloniaTextureSliceDecoder.cs`
- Modify: `PrimalEditor_Avalonia/Editors/Texture/TextureEditorViewModel.cs`
- Modify: `PrimalEditor_Avalonia/Editors/Texture/TextureEditorView.axaml`
- Test: `PrimalEditor_Avalonia.Tests/Texture/TextureSliceSetTests.cs`
- Test: `PrimalEditor_Avalonia.Tests/Texture/TextureEditorViewModelTests.cs`

**Steps:**
1. Define `TextureSliceSet` as immutable decoded RGBA slices indexed by array, mip, and depth, with dimensions per mip.
2. Write failing tests for valid indexing, bounds rejection, and mip dimension reduction.
3. Implement the model without Avalonia UI dependencies.
4. Extract file decoding behind `ITextureSliceDecoder`; the initial decoder exposes actual slices available from the source/container and reports one slice for ordinary PNG/JPEG files instead of trusting sidecar counts.
5. Write failing VM tests showing maxima derive from decoded data, selection clamps, and changing any index rebuilds preview from that slice.
6. Replace `_rawRgba/_rawWidth/_rawHeight` with `TextureSliceSet` selection.
7. Bind controls to actual maxima and disable unavailable dimensions.
8. Verify focused/full tests, build, and a manual macOS texture-editor smoke test.

## Task 3: Implement True Geometry Reimport

**Files:**
- Create: `PrimalEditor_Avalonia/Editors/Geometry/IGeometryReimportService.cs`
- Create: `PrimalEditor_Avalonia/Editors/Geometry/GeometryReimportService.cs`
- Modify: `PrimalEditor_Avalonia/Editors/Geometry/GeometryEditorViewModel.cs`
- Modify: `PrimalEditor_Avalonia/Editors/Geometry/GeometryImportSettings.cs`
- Modify only if required by an existing native API gap: `PrimalEditor_Avalonia/Native/NativeEngineBridge.cs`, `EngineDLL/AssetAPI.h`, `EngineDLL/AssetAPI.cpp`
- Test: `PrimalEditor_Avalonia.Tests/Geometry/GeometryReimportServiceTests.cs`
- Test: `PrimalEditor_Avalonia.Tests/Geometry/GeometryEditorViewModelTests.cs`

**Steps:**
1. Trace the existing OBJ/FBX → `.mesh` cook path and source-path metadata before adding APIs.
2. Write a failing service test that cooks a temporary source mesh to a temporary output and verifies a valid changed `.mesh` is atomically installed.
3. Implement reimport to cook to a sibling temporary file, validate by loading `Geometry`, then atomically replace the destination; preserve the old mesh on failure.
4. Write failing tests for missing source, failed cook, invalid output, and successful replacement.
5. Make source path and relevant import settings persist in the existing append-only geometry sidecar.
6. Convert `Reimport` to an async operation with busy state, errors, sidecar save only on success, and data reload after replacement.
7. Verify focused/full tests, editor build, verifier, and compare destination hash before/after a known source modification.

## Task 4: Refactor BuildService into a Cross-Platform Process Layer

**Files:**
- Create: `PrimalEditor_Avalonia/Common/BuildCommand.cs`
- Create: `PrimalEditor_Avalonia/Common/IBuildCommandResolver.cs`
- Create: `PrimalEditor_Avalonia/Common/PlatformBuildCommandResolver.cs`
- Create: `PrimalEditor_Avalonia/Common/IProcessRunner.cs`
- Create: `PrimalEditor_Avalonia/Common/ProcessRunner.cs`
- Modify: `PrimalEditor_Avalonia/Common/BuildService.cs`
- Test: `PrimalEditor_Avalonia.Tests/Build/PlatformBuildCommandResolverTests.cs`
- Test: `PrimalEditor_Avalonia.Tests/Build/ProcessRunnerTests.cs`
- Test: `PrimalEditor_Avalonia.Tests/Build/BuildErrorParserTests.cs`

**Steps:**
1. Write failing resolver tests for macOS/Linux `.sh` execution and Windows `.cmd`/direct executable resolution without invoking those platforms.
2. Represent commands as executable plus `ArgumentList`, working directory, and environment; never concatenate a shell command string.
3. On macOS use `/bin/zsh` or an executable script selected by the resolver; do not hardcode `/bin/bash` in `BuildService`.
4. Write failing runner tests for output streaming, nonzero exit, cancellation, and process-tree termination using temporary scripts.
5. Move duplicated `Start`, `StartRebuild`, and `StartApp` process setup into one runner path.
6. Keep UI events and public semantics compatible.
7. Extend `BuildErrorParser` tests for clang/gcc and MSVC paths.
8. Verify focused/full tests, build, serialization verifier, and one real macOS editor build invocation.

## Task 5: Complete Content Browser Navigation and Filtering

**Files:**
- Create: `PrimalEditor_Avalonia/ViewModels/BreadcrumbItemViewModel.cs`
- Modify: `PrimalEditor_Avalonia/ViewModels/ContentBrowserViewModel.cs`
- Modify: `PrimalEditor_Avalonia/Views/ContentBrowserView.axaml`
- Modify: `PrimalEditor_Avalonia/Views/ContentBrowserView.axaml.cs`
- Test: `PrimalEditor_Avalonia.Tests/Content/ContentBrowserViewModelTests.cs`

**Steps:**
1. Write failing tests for case-insensitive search, folder/asset filtering, clearing search, and current-folder boundaries.
2. Add a source collection and filtered projection so watcher refresh and search compose predictably.
3. Write failing breadcrumb tests for root, nested folders, and navigation to an ancestor.
4. Add breadcrumb items derived from current path; bind each segment to navigation.
5. Preserve current create/rename/delete/drag-drop behavior during refresh.
6. Run focused/full tests, build, and manual Content Browser smoke tests.

## Task 6: Add Reusable Import Settings Editing

**Files:**
- Create: `PrimalEditor_Avalonia/Editors/ImportSettings/IImportSettingsEditor.cs`
- Create: `PrimalEditor_Avalonia/Editors/ImportSettings/ImportSettingsEditorRegistry.cs`
- Create: `PrimalEditor_Avalonia/Editors/ImportSettings/ImportSettingsDialog.axaml`
- Create: `PrimalEditor_Avalonia/Editors/ImportSettings/ImportSettingsDialog.axaml.cs`
- Modify: `PrimalEditor_Avalonia/ViewModels/ContentBrowserViewModel.cs`
- Modify: `PrimalEditor_Avalonia/Views/ContentBrowserView.axaml`
- Test: `PrimalEditor_Avalonia.Tests/Content/ImportSettingsEditorRegistryTests.cs`

**Steps:**
1. Write failing registry tests for texture/mesh support, unsupported assets, and homogeneous multi-selection.
2. Register existing Texture and Geometry settings editors rather than duplicating their serialization.
3. Add a Content Browser command enabled only for supported selection.
4. For multi-selection, expose only common editable fields and save each sidecar independently; report partial failures without discarding successful saves.
5. Verify focused/full tests, build, and manual single/batch editing.

## Task 7: Build a Generic Inspector Component Registry

**Files:**
- Create: `PrimalEditor_Avalonia/Components/EditorComponentDescriptor.cs`
- Create: `PrimalEditor_Avalonia/Components/EditorComponentRegistry.cs`
- Create: `PrimalEditor_Avalonia/ViewModels/ComponentEditorViewModel.cs`
- Modify: `PrimalEditor_Avalonia/ViewModels/PropertiesViewModel.cs`
- Modify: `PrimalEditor_Avalonia/Views/PropertiesView.axaml`
- Test: `PrimalEditor_Avalonia.Tests/Inspector/EditorComponentRegistryTests.cs`
- Test: `PrimalEditor_Avalonia.Tests/Inspector/PropertiesViewModelTests.cs`

**Steps:**
1. Write failing registry tests for stable component IDs, factories, display names, and duplicate registration rejection.
2. Register Transform first through the same descriptor path while preserving current multi-select mixed-value behavior.
3. Write failing tests for Add Component across compatible selected entities, preventing duplicates, and one Undo transaction.
4. Write failing tests for Remove Component, prohibiting required Transform removal, and Undo restoration.
5. Add dynamic component editor sections generated from registry descriptors.
6. Add Add/Remove Component UI with mixed-selection rules and clear disabled reasons.
7. Verify focused/full tests, scene serialization round-trip, build, and manual Inspector smoke tests.

## Task 8: Integrate Docking after Panel Lifecycles Stabilize

**Files:**
- Modify: `PrimalEditor_Avalonia/Common/DockingManager.cs`
- Modify: `PrimalEditor_Avalonia/Common/EditorLayoutSettings.cs`
- Modify: `PrimalEditor_Avalonia/Views/MainWindow.axaml`
- Modify: `PrimalEditor_Avalonia/Views/MainWindow.axaml.cs`
- Modify as needed: `PrimalEditor_Avalonia/Editors/AssetEditorHost.cs`
- Test: `PrimalEditor_Avalonia.Tests/Docking/DockingLayoutModelTests.cs`
- Test: `PrimalEditor_Avalonia.Tests/Docking/DockingManagerTests.cs`

**Steps:**
1. Extract a UI-independent docking layout model and write failing tests for register, dock, float, close-to-redock, duplicate IDs, and persistence round-trip.
2. Fix the one-window `_floatHost` design so each floating dockable owns its own host and lifecycle.
3. Write failing tests for multiple dockables per zone; implement tab grouping rather than placing controls on top of each other.
4. Integrate World Editor panels and asset editor documents into the main window without changing editor VM ownership.
5. Persist zones, tab order, floating bounds, and active document in `editor-layout.json`, tolerating unknown/removed IDs.
6. Add drag target feedback and dock/float operations; keep direct commands as a keyboard/accessibility fallback.
7. Verify focused/full tests, build, serialization verifier, and manual macOS lifecycle tests: open, dock, float, close, restart, restore.

## Task 9: Final Regression and Review

**Steps:**
1. Run `dotnet test PrimalEditor_Avalonia.Tests/PrimalEditor_Avalonia.Tests.csproj`.
2. Run `dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor`.
3. Run `Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor_Avalonia --verify-serialization`.
4. Run a macOS GUI smoke pass for Texture, Geometry, Content Browser, Inspector, Build/Run, docking, close confirmation, and layout restore.
5. Run project-wide diagnostics available in the environment and review the final diff for unrelated changes.
6. Request spec, code quality, security, and hands-on QA review; resolve findings before completion.
