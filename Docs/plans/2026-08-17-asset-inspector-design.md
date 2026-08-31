# AssetInspector — Model Asset Preview & Editor Window

**Date:** 2026-08-17
**Status:** Design (validated)
**Branch:** avalonia-migration

## Context & Motivation

`Content Browser` is currently docked as a tab sibling to `World Editor` inside a single `DocumentDock`. This forces users to switch tabs to drag assets — Avalonia's `DragDrop.DoDragDropAsync` is synchronous and does not switch tabs mid-drag.

We decided earlier to **move Content Browser into a side `ToolDock`** so it stays resident and is always draggable into the viewport (the standard Unreal/Unity pattern).

Separately, we need a **dedicated Asset Inspector window** that opens when the user double-clicks an engine-format asset (`.model.json`, `.texture`, etc.) in Content Browser. It shows a 3D preview with submesh/LOD navigation and is structured to be extended for future asset-type inspectors and editing actions.

This design covers the new Asset Inspector. The Content Browser → ToolDock relocation is a prerequisite but is split into a separate small change (see "Prerequisites").

## Scope

**In scope:**
- New `AssetInspectorView` / `AssetInspectorViewModel` (Document subtype) for **mesh assets only** in v1
- Three-column layout: TreeView (submesh/LOD) | Preview viewport | Inspector panel
- Independent preview camera (`EngineViewport3D` in `PreviewMode`)
- LOD selection via dropdown, submesh highlight via TreeView selection
- Drag-out support: any tree node can be dragged onto World Editor viewport
- Extension points (virtual methods) for future Texture/Shader inspectors
- Content Browser filter: show only engine-format assets (`.model.json`, `.model`, `.texture`)
- Double-click in Content Browser opens a new AssetInspector tab

**Out of scope (future extensions):**
- Texture asset inspector (channel/mip inspector) — design supports it but v1 doesn't build it
- Shader variant inspector — same
- Asset dependency graph view — extension point exists; UI deferred
- Asset version/diff view — separate `MeshDiffViewModel` future work
- Re-import / edit import settings UI — extension point exists; action deferred to v1.1

## Prerequisites

Before implementing this design, complete:
1. **Content Browser → ToolDock relocation** — `MainWindowDockFactory.cs` change. ToolDock on the left side. World Editor becomes the only Document.

This is a small change (~10 lines) and unblocks drag-and-drop UX.

## Architecture

### Layout Tree (after prerequisites)

```
RootDock
└── ProportionalDock (Horizontal)
    ├── ToolDock (Left, default width 280)
    │   └── ContentBrowserView (the existing VM, just relocated)
    └── DocumentDock (Center, takes remaining width)
        └── WorldEditorView  ← only Document in v1
        └── AssetInspectorView (0..N instances, opened on double-click)
```

`DocumentDock` accepts multiple documents as tabs; World Editor stays active by default.

### File Map

| File | Status | Purpose |
|---|---|---|
| `Common/MainWindowDockFactory.cs` | **modify** | Prerequisites + DocumentDock accepts `AssetInspectorViewModel` instances |
| `ViewModels/AssetInspectorViewModel.cs` | **new** | Base class; v1 subclass `MeshAssetInspectorViewModel` lives here too |
| `Views/AssetInspectorView.axaml` | **new** | Three-column XAML |
| `Views/AssetInspectorView.axaml.cs` | **new** | Drag-out source, LOD dropdown handler |
| `ViewModels/MeshAssetInspectorViewModel.cs` | **new** | Subclass implementing mesh-specific virtual methods |
| `Content/AssetNode.cs` | **new** | Sealed record `AssetNode(Kind, Name, Index, MeshHandle)` |
| `Content/ContentBrowserFilter.cs` | **new** | Static filter for engine-format extensions |
| `Views/ContentBrowserView.axaml.cs` | **modify** | Apply filter + double-click → open inspector |
| `ViewModels/ContentBrowserViewModel.cs` | **modify** | Add `OpenAssetCommand(Guid)` |
| `Views/Engine3D/EngineViewport3D.axaml.cs` | **modify** | Add `PreviewMode` property (disable gizmo, swap camera) |

### Component Diagram

```
┌────────────────────────────────────────────────────────────────────────┐
│ ContentBrowserView                                                    │
│   - Filter: ContentBrowserFilter.IsEngineFormat(path)                 │
│   - Double-click → ContentBrowserVM.OpenAssetCommand(asset.Guid)        │
└────────────────────────────────────────────────────────────────────────┘
                                  ↓ calls DocumentDock.OpenDocument
┌────────────────────────────────────────────────────────────────────────┐
│ DocumentDock                                                          │
│   - VisibleDockables: [WorldEditor, AssetInspector_A, AssetInspector_B]│
└────────────────────────────────────────────────────────────────────────┘
                                  ↓ each instance
┌────────────────────────────────────────────────────────────────────────┐
│ AssetInspectorViewModel : Document    (abstract base)                 │
│   - Asset: AssetInfo                                                  │
│   - SelectedNode: AssetNode?                                          │
│   - TreeRoots: ObservableCollection<AssetNode>                        │
│   - virtual GetTreeRoots()         ─┐                                 │
│   - virtual GetInspectorSections()  │  Extension hooks                │
│   - virtual OnNodeSelected(node)   ─┘                                 │
│   - virtual GetPreviewCommands()                                      │
│   - virtual IsDraggable(node)                                         │
│   - ImportMeshAsync() / DisposeMeshHandle()                          │
└────────────────────────────────────────────────────────────────────────┘
                ↓ inherits (v1)
┌────────────────────────────────────────────────────────────────────────┐
│ MeshAssetInspectorViewModel : AssetInspectorViewModel                  │
│   - SubmeshCount, LODCount                                            │
│   - ActiveLODIndex                                                    │
│   - NativeEngineBridge.ImportMesh(modelJsonPath) → meshHandle         │
│   - NativeEngineBridge.SetActiveLOD(handle, idx)                      │
│   - NativeEngineBridge.SetActiveSubmesh(handle, idx)                  │
│   - GetTreeRoots() → [MeshesNode, LodsNode]                           │
│   - GetInspectorSections() → [General, Submesh, LOD]              │
└────────────────────────────────────────────────────────────────────────┘
```

## Data Flow — Double-click to Preview

```
User double-clicks foo.model.json in ContentBrowser
    │
    ▼
ContentBrowserView.OnAssetDoubleClick(item)
    │
    ▼
ContentBrowserViewModel.OpenAssetCommand.Execute(asset.Guid)
    │
    ├─ if asset.Type == Model:
    │     new MeshAssetInspectorViewModel(asset)
    │     DocumentDock.OpenDocument(inspector)
    │
    ▼
MeshAssetInspectorViewModel constructor
    │
    ├─ NativeEngineBridge.ImportMesh(modelJsonPath)
    │     ↓ async, returns IntPtr meshHandle after .mesh cooked binary loaded
    │     ↓ on success: ReadSidecar() → SubmeshCount, LODCount
    │     ↓             TreeRoots populated
    │     ↓ on failure: TreeRoots empty + StatusMessage = error
    │
    ▼
View binds to TreeRoots; user sees tree populated when import completes
    │
    ▼
User clicks "LOD0" node → SelectedNode = LOD node
    │
    ▼
AssetInspectorViewModel.OnNodeSelected override
    │
    ├─ if kind == LOD → NativeEngineBridge.SetActiveLOD(handle, index)
    ├─ if kind == Submesh → NativeEngineBridge.SetActiveSubmesh(handle, index)
    └─ if kind == Model → reset to full model
    │
    ▼
Preview viewport renders updated mesh
```

## Data Flow — Drag from Inspector to World Editor

```
User starts dragging selected TreeView node
    │
    ▼
AssetInspectorView.OnTreeNodePointerPressed + OnAssetPointerMoved
    │ (mirror of ContentBrowserView pattern, with DragThreshold = 5.0)
    │
    ▼
Build AssetDragPayload:
    - Guid = asset.Guid
    - FullPath = asset.FullPath
    - IsContainer = (node.Kind == Model || node.Kind == LOD)
    - ParentGuid = asset.Guid
    │
    ▼
DragDrop.DoDragDropAsync(...) — same mechanism as ContentBrowser
    │
    ▼
User drops on World Editor viewport
    │
    ▼
WorldEditorView.OnAssetDrop → ResolveFromPayload → vm.DropAssetAsync(...)
    │
    ▼
[existing World Editor drop code unchanged; already supports parent fallback]
```

## Engine Format Filter

Content Browser must only show engine-format files. Filter rule (extension match, case-insensitive):

```csharp
public static class ContentBrowserFilter
{
    private static readonly HashSet<string> EngineFormats = new(StringComparer.OrdinalIgnoreCase)
    {
        ".model.json", ".model", ".mesh", ".texture",
        // future: ".shader", ".material.json"
    };

    public static bool IsEngineFormat(string fullPath)
        => EngineFormats.Contains(Path.GetExtension(fullPath))
           || fullPath.EndsWith(".model.json", StringComparison.OrdinalIgnoreCase);

    public static IEnumerable<AssetInfo> Apply(IEnumerable<AssetInfo> assets)
        => assets.Where(a => a.IsFolder || IsEngineFormat(a.FullPath));
}
```

**Source formats (`.fbx`, `.obj`, `.gltf`, `.glb`, `.dae`, `.blend`) are excluded from Content Browser listing.**
They can still be dropped onto the World Editor viewport from the OS file manager (existing OS drop code path in `WorldEditorView.OnAssetDrop` is preserved). When dropped that way, the existing import-on-demand flow runs.

## Extension Points

| Virtual method | v1 implementation | Future Texture | Future Shader |
|---|---|---|---|
| `GetTreeRoots()` | `MeshesNode + LodsNode` | `MipsNode + ChannelsNode` | `VariantsNode` |
| `GetInspectorSections()` | `General/Submesh/LOD` | `General/Compression/Mips` | `General/Compile` |
| `OnNodeSelected(node)` | switch on kind | switch on channel/mip | switch on variant |
| `GetPreviewCommands()` | `Reimport, Open Folder, Copy GUID` | `Reimport, Convert Format` | `Recompile` |
| `IsDraggable(node)` | always true | usually false | never |

These are public and `virtual` (not `abstract`) so each subclass opts in to what it needs.

## Three-Column View Layout

```
AssetInspectorView.axaml:
┌─ Header (Title + Preview Actions toolbar) ─────────────────────────┐
│ [foo.model.json]            [LOD: LOD0 ▼]   [↻][📂][📋]          │
├─────────────────┬──────────────────────────────┬──────────────────┤
│ Tree (220px)    │ Preview viewport             │ Inspector (260px)│
│                 │                              │                  │
│ ▼ Root          │   [3D Preview - independent  │ ▾ General        │
│  ▼ Meshes       │    camera, no gizmo]         │   Vertices: 4.2k │
│   • submesh_0   │                              │   Triangles: 8k  │
│   • submesh_1   │                              │   Bounds: 1.0m   │
│  ▼ LODs         │                              │ ▾ Submesh[0]     │
│   • LOD0        │                              │   Material: ...  │
│   • LOD1        │                              │   Index Range..  │
│                 │                              │                  │
│                 │                              │                  │
│                 ├──────────────────────────────┤                  │
│                 │ Stats: 4.2k vert, 60 fps     │                  │
└─────────────────┴──────────────────────────────┴──────────────────┘
```

## Asset Node Type

```csharp
public sealed record AssetNode(
    AssetNodeKind Kind,         // Model | RootNode | Submesh | LOD
    string Name,
    int Index,                  // -1 for non-indexed nodes
    IntPtr MeshHandle,          // native handle (0 if not yet loaded)
    IReadOnlyList<AssetNode>? Children = null
)
```

No inheritance. Behavior dispatched by `Kind` switch in the VM overrides.

## Preview Mode for EngineViewport3D

Add `PreviewMode` (bool, default false) to `EngineViewport3D`:
- `PreviewMode == false` → current behavior (gizmo, picking, scene camera)
- `PreviewMode == true` →
  - Hide gizmo
  - Disable picking
  - Use a built-in `OrbitCameraController` (target = mesh AABB center, distance = AABB radius × 2)
  - Disable drag-drop target (only Inspector drag-source uses it)
  - Mesh is shown via `NativeEngineBridge.RenderMesh(handle, ActiveLOD, ActiveSubmesh)` instead of scene traversal

Implement as a branch in the existing `Render()` method, not a full rewrite.

## Error Handling

| Failure | Behavior |
|---|---|
| `NativeEngineBridge.ImportMesh` returns 0/null | TreeRoots empty; StatusMessage shows "Import failed: <reason>"; preview shows last-good frame or empty |
| `.model.json` missing sibling `.mesh` | TreeRoots empty; message: "Cooked .mesh not found — re-import required" |
| Sidecar parse error | TreeRoots shows just the model root; Submesh/LOD counts = 0 |
| LOD index out of range | Clamp to `[0, LODCount-1]`; show warning toast |
| Document closed while import in flight | `DisposeMeshHandle()` after cancellation; `CancellationTokenSource.Cancel()` |

## Testing Strategy

| Layer | Test type | File |
|---|---|---|
| VM logic | Unit test (xUnit) | `PrimalEditor_Avalonia.Tests/ViewModels/MeshAssetInspectorViewModelTests.cs` |
| Filter | Unit test | `PrimalEditor_Avalonia.Tests/Content/ContentBrowserFilterTests.cs` |
| Drag payload | Unit test (round-trip) | extend existing `AssetDragPayloadTests.cs` |
| View XAML | Manual smoke test | N/A (XAML loads on app start) |
| Layout | Manual smoke test | Run editor, open inspector, resize |

We have a `PrimalEditor_Avalonia.Tests` project (xUnit) — extend it. No new test infra needed.

## Implementation Order

1. **Pre: Content Browser → ToolDock** (prerequisite, ~30 min)
   - `MainWindowDockFactory.cs`: move ContentBrowser from DocumentDock to a new ToolDock sibling
   - Verify drag from Content Browser to viewport
2. **Filter** (small)
   - `Content/ContentBrowserFilter.cs` + unit tests
   - Apply in `ContentBrowserViewModel.FilteredFiles` getter
3. **AssetNode record** (trivial)
4. **EngineViewport3D.PreviewMode** (medium)
   - Add property, branch in Render(), implement OrbitCameraController
5. **AssetInspectorViewModel base** (medium)
   - Virtual extension points, ImportMeshAsync, DisposeMeshHandle
6. **MeshAssetInspectorViewModel** (medium)
   - Override GetTreeRoots / GetInspectorSections / OnNodeSelected / IsDraggable
   - Native calls to SetActiveLOD / SetActiveSubmesh
7. **AssetInspectorView** (medium)
   - Three-column XAML
   - TreeView binding + selection handler
   - LOD dropdown binding
   - Inspector section bindings
8. **Double-click open** (small)
   - ContentBrowserView: `OnAssetDoubleClick` → `vm.OpenAssetCommand`
   - ContentBrowserViewModel: `OpenAssetCommand` → `DocumentDock.OpenDocument(new MeshAssetInspectorViewModel(info))`
9. **Drag-out** (small)
   - Reuse AssetDragPayload
   - OnTreeNodePointerPressed + OnAssetPointerMoved (mirror ContentBrowser pattern)
10. **Tests** (medium)
    - Filter tests, VM tests, payload round-trip with parent GUID
11. **Verification**
    - `dotnet build` clean
    - Manual smoke test: open editor, double-click mesh, drag submesh to World Editor, place entity, verify mesh renders
    - `lsp_diagnostics` clean on all new files
    - `graphify update .` per CLAUDE.md

## Acceptance Criteria

- [ ] Content Browser only shows engine-format assets (`.model.json`, `.model`, `.mesh`, `.texture`)
- [ ] Double-clicking a mesh asset opens a new Asset Inspector tab
- [ ] Asset Inspector shows a 3D preview of the mesh
- [ ] TreeView lists submeshes and LODs; selecting one highlights in preview
- [ ] LOD dropdown switches the previewed LOD
- [ ] Inspector panel shows General / Submesh / LOD sections with live data
- [ ] Dragging any TreeView node onto World Editor viewport creates an entity
- [ ] Multiple Asset Inspector tabs can be open simultaneously
- [ ] Closing an Asset Inspector tab disposes its native mesh handle
- [ ] All tests pass; build is clean
- [ ] Drag from Content Browser still works (regression check after ToolDock move)

## Risks & Open Questions

1. **Native `SetActiveLOD`/`SetActiveSubmesh` may not exist yet.** If absent, we need to add them to `NativeEngineBridge.cs`. Flag this during implementation step 6.
2. **OrbitCameraController** doesn't exist yet — need to add or stub it. Lower priority: can use simple Euler-orbit until v1.1.
3. **Tab proliferation.** Users could open dozens of inspectors. Consider DocumentDock "Close Others" / "Close All" — defer to user feedback.

## Notes

- The existing `ContentBrowserPreviewViewModel.cs` was discovered during research. Its preview pipeline logic may be reusable as a starting point. Investigate during step 5.
- Per CLAUDE.md, run `graphify update .` after the implementation lands.