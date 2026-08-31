# World Editor Scene Manipulation — Design

**Date:** 2026-08-11
**Branch:** feat/avalonia-migration
**Status:** Approved (scope locked)

---

## 1. Background & Goal

The `WorldEditorView` already exists with hierarchy tree, viewport, inspector,
gizmo overlay, picker, undo/redo, scene serialization, and a multi-entity
PropertiesView. **The user does not need a new editor** — they need the
existing editor extended so that placing / moving / rotating / scaling
people, props, lights, and cameras in the scene is a first-class flow that
**persists across save/reload**.

Six concrete gaps block the goal today:

1. Toolbar has only "+ Entity" (blank). No "Add Character / Prop / Light /
   Camera" buttons.
2. **Gizmo Translate clobbers rotation/scale** — confirmed bug in
   `WorldEditorView.OnGizmoDrag` (always writes `rot={0,0,0}` and
   `scl={1,1,1}`).
3. Gizmo Rotate and Scale modes have **no write-back** path at all.
4. Inspector edits to Transform never reach the native engine
   (`PropertiesViewModel.RecordAndApply` does not call
   `SetEntityTransform`).
5. Dropping a `.mesh` into the scene does **not** auto-bind the default
   white PBR material — meshes render as the engine's unbound default
   (likely magenta).
6. No `EntityMetadata` (IsPlayer / Tags / Description) for filtering
   scene objects in later gameplay scripts.

### Success Criteria

- User can click a toolbar button to add a Character, Prop, Light, or
  Camera at the world origin (or the currently selected entity's child
  position).
- User can drag gizmo Translate/Rotate/Scale axes; the engine
  visualization updates immediately, the C# `GameEntity` reflects the
  change, undo/redo round-trips it, and saving+reloading the scene
  preserves it.
- User can edit any property (position, rotation, scale, light params,
  camera params, IsPlayer, Tags, Description) in the inspector and see
  the change both in the viewport and persisted to disk.
- New entity types can be added by **adding one row** to a factory
  registry — no touching of toolbar or view-model code beyond that row.

---

## 2. Architecture

### 2.1 Entity Type Registry (the extensibility spine)

A new `EntityFactory` static class in
`PrimalEditor_Avalonia/Entities/EntityFactory.cs`. Each entry is a
declarative record describing:

```csharp
public sealed record EntityFactoryDescriptor(
    EntityKind Kind,                    // Character, Prop, Light, Camera, ...
    string DisplayName,                 // "Character"
    string IconGlyph,                   // "👤"
    Func<GameEntity> CreateEntity,      // builds the C# entity
    string[] ComponentNames);           // for the inspector hint
```

`EntityKind` is an extensible enum (`Light`, `Camera`, `Character`,
`Prop`, `Empty`). The factory is **the single source of truth** for:

- Which toolbar buttons exist and their order.
- What components each button attaches.
- Default parameter values per entity kind.
- The "is this a light/camera?" classification used by gizmo
  visualization extensions later.

Adding a new entity kind = add one `EntityFactoryDescriptor` row, plus
the corresponding Component subclass if needed. **No edits to
`WorldEditorView.axaml`, `WorldEditorViewModel.cs`, or
`SceneRuntimeSynchronizer.cs`.**

### 2.2 The Toolbar (data-driven)

`WorldEditorView.axaml` adds a new `ItemsControl` populated from
`WorldEditorViewModel.EntityFactoryItems`. Each item is a button
calling a unified `OnAddEntityKindCommand(EntityKind)`. The view-model
exposes the list as an `ObservableCollection<EntityFactoryDescriptor>`
fed by `EntityFactory.All`.

This means adding a new button is purely a code change in
`EntityFactory.cs`. The XAML doesn't need editing.

### 2.3 Default White PBR Material (pre-created, globally shared)

`SceneRuntimeSynchronizer` gains one new field:

```csharp
private ulong _defaultMaterialId;
```

In `Sync(scene)`, after `Teardown()` and before per-entity creation:

```csharp
_defaultMaterialId = _bridge.CreateDefaultMaterial();
```

All `AddRenderItem`/`PipelineRegisterMeshEntity` paths receive this
material id. The material id is **scene-scoped** (recreated on every
`Sync`) and lives only in the synchronizer — no C# persistence
required, because the editor only ships one material.

### 2.4 Gizmo Write-Back (correct + complete)

Replace `OnGizmoDrag` in `WorldEditorView.axaml.cs` with a switch on
`GizmoMode` (Translate / Rotate / Scale). Each mode:

1. Reads **current** transform from the C# `Transform` component (not
   the native engine — the C# is source of truth).
2. Mutates only the dragged axis.
3. Records the change through `UndoRedo` so it's undoable.
4. Calls `NativeEngineBridge.SetEntityTransform` so the engine
   reflects it.

```csharp
var oldTransform = SnapshotTransform(entity);
ApplyAxisDelta(transform, e.axis, e.delta, e.mode);
UndoRedo.Add(new TransformAction(oldTransform, transform, e.mode));
NativeEngineBridge.SetEntityTransform(nativeId, pos, rot, scl);
Properties.Refresh(); // re-bind inspector numbers
```

The rotation delta for the Rotate gizmo is converted from pixels to
radians: `delta * (MathF.PI / 180f) * 0.5f` (tweakable scale; matches
the Translate sensitivity's feel).

The Scale gizmo treats `delta` as a multiplicative factor per pixel:
`axis *= 1f + e.delta * 0.01f`. Clamped to `(0.01f, 100f)` to avoid
zero/infinity blowups.

### 2.5 Inspector → Native Push

`PropertiesViewModel.RecordAndApply` calls
`NativeEngineBridge.SetEntityTransform` for any Transform change after
the `applyChange()` runs:

```csharp
private void RecordAndApply(string propertyName, Action applyChange)
{
    var undoRedo = TryGetUndoRedo();
    // ... existing snapshot/apply logic ...
    applyChange();
    // ... existing UndoRedo.Add(new MultiEntityTransformAction(...)) ...
    SyncEntitiesToNative();
}

private void SyncEntitiesToNative()
{
    if (!NativeEngineBridge.IsLoaded) return;
    foreach (var e in SelectedEntities)
    {
        var t = e.GetTransform();
        var nativeId = WorldEditorContext.GetNativeEntityId(e);
        if (nativeId == 0 || t == null) continue;
        NativeEngineBridge.SetEntityTransform(nativeId,
            new[]{t.PositionX, t.PositionY, t.PositionZ},
            new[]{t.RotationX, t.RotationY, t.RotationZ},
            new[]{t.ScaleX, t.ScaleY, t.ScaleZ});
    }
}
```

`SyncEntitiesToNative` is also called from
`WorldEditorView.OnGizmoDrag` (after UndoRedo.Add) so both code paths
share one push helper.

### 2.6 EntityMetadata Component

New file `Components/EntityMetadata.cs`:

```csharp
public sealed class EntityMetadata : Component
{
    public bool IsPlayer { get; set; }
    public ObservableCollection<string> Tags { get; set; } = new();
    public string Description { get; set; } = string.Empty;
    public override IMSComponent GetMultiselectionComponent(MSEntity msEntity)
        => new MSEntityMetadata(msEntity);
}
```

Plus `MSEntityMetadata` mirroring the existing `MSTransform` mixed-value
pattern (null = multiple values, set = unanimous).

Registered as `[JsonDerivedType(typeof(EntityMetadata),
typeDiscriminator: "metadata")]` on `Component`. Added to
`EditorComponentRegistry`. Inspector gets a new collapsible panel.

### 2.7 Persistence

Free — the existing JSON v2 scene format picks up:
- New `EntityMetadata` component automatically via polymorphism.
- New entity kinds automatically because the scene file stores
  components, not "kinds".
- All transforms save with the existing `Transform` component.

No schema migration required (the format already supports arbitrary
components).

---

## 3. Components / Files Touched

### New files

- `PrimalEditor_Avalonia/Entities/EntityFactory.cs` — descriptor table.
- `PrimalEditor_Avalonia/Entities/EntityKind.cs` — extensible enum.
- `PrimalEditor_Avalonia/Components/EntityMetadata.cs` — new component.
- `PrimalEditor_Avalonia/Components/MSEntityMetadata.cs` — multi-select
  wrapper.
- `PrimalEditor_Avalonia.Tests/EntityFactoryTests.cs` — registry sanity.
- `PrimalEditor_Avalonia.Tests/EntityMetadataTests.cs` — JSON round-trip.
- `PrimalEditor_Avalonia.Tests/GizmoWritebackTests.cs` — transform
  write-back + undo/redo.

### Modified files

- `Views/WorldEditorView.axaml` — add `ItemsControl` of factory buttons.
- `Views/WorldEditorView.axaml.cs` — `OnGizmoDrag` becomes a mode-aware
  switch; new `OnAddEntityKindCommand(EntityKind)`.
- `ViewModels/WorldEditorViewModel.cs` — expose
  `EntityFactoryItems`.
- `Components/Component.cs` — add `[JsonDerivedType]` for
  `EntityMetadata`.
- `Components/EditorComponentRegistry.cs` — register `EntityMetadata`.
- `ViewModels/PropertiesViewModel.cs` — add `EntityMetadata` panel;
  call `SyncEntitiesToNative` after `applyChange`.
- `Views/PropertiesView.axaml` — add `EntityMetadata` section.
- `GameProject/Serializer.cs` — extend the discriminator switch (the
  derived-type polymorphism usually handles this; verify).
- `Native/SceneRuntimeSynchronizer.cs` — pre-create default material;
  pass it into every `PipelineRegisterMeshEntity` /
  `AddRenderItem`-equivalent path.
- `Views/Engine3D/GizmoOverlay.cs` — fire `GizmoDrag` event with both
  `axis` and `mode` so the world-editor can switch behavior.

---

## 4. Data Flow

```
User clicks [💡 + Light] in toolbar
  → WorldEditorView.OnAddEntityKindCommand(EntityKind.Light)
  → EntityFactory.Create(Light)
  → GameEntity { Name = "Light_1", Components = [Transform, LightComponent] }
  → WorldEditorViewModel.AddEntity(entity)
  → UndoRedo.Add(AddEntityAction)
  → SceneRuntimeSynchronizer.SyncEntity(entity)  (per-entity push, not full resync)
  → Native: CreateEntity, SetEntityTransform, AddEntityLight

User drags Translate gizmo X arrow
  → GizmoOverlay.OnPointerMoved fires GizmoDrag(axis=X, mode=Translate, delta=pixels)
  → WorldEditorView.OnGizmoDrag
  → Snapshot current Transform (oldT)
  → Apply axis delta to Transform (newT)
  → UndoRedo.Add(new TransformAction(oldT, newT, Translate))
  → SyncEntitiesToNative()
  → Native: SetEntityTransform(nativeId, newT)
  → PropertiesView.Refresh()

User opens scene next session
  → Serializer.LoadSceneDocument reads Scenes/<guid>.scene
  → JSON deserialization auto-creates GameEntity + Transform +
    LightComponent + EntityMetadata via JsonDerivedType
  → SceneRuntimeSynchronizer.Sync(scene)
  → Native entities re-created with the saved transform/light/material
```

---

## 5. Error Handling

| Failure | Behavior |
|---|---|
| User adds Light while engine not loaded | C# entity added; marked as `awaiting native sync`; synced on next engine load or scene change. (Existing pattern — `DropAssetAsync` already does this.) |
| Native `SetEntityTransform` throws | Log via `Logger.Error`; do not corrupt the C# transform; UndoRedo entry is still added (so user can undo the visual desync). |
| `CreateDefaultMaterial` returns 0 | Log error; fall back to existing behavior (meshes render with engine's unbound fallback); do not crash. |
| User tries to Scale to 0 | Clamp to `0.01f`; log warning once (rate-limited). |
| User deletes entity while Light/Camera is selected | Already handled by `RemoveEntityAction`; no extra logic needed. |
| Gizmo event arrives for an entity that no longer exists | Guard with `if (entity.ParentScene == null) return;` at top of `OnGizmoDrag`. |

---

## 6. Testing Strategy

### Unit tests (new)

- **EntityFactoryTests**: each descriptor produces a non-null entity
  with the declared component types; descriptor count == toolbar item
  count.
- **EntityMetadataTests**: serialize / deserialize round-trip preserves
  `IsPlayer`, `Tags[]`, `Description`.
- **GizmoWritebackTests**:
  - Translate X+ delta → only `PositionX` changes.
  - Rotate Y+ delta → only `RotationY` changes; rotation/scale not
    clobbered.
  - Scale Z+ delta → only `ScaleZ` changes.
  - Each produces an UndoRedo entry; `Undo()` restores the snapshot.
- **InspectorSyncTests**: setting `PositionX` in PropertiesViewModel
  triggers `SetEntityTransform` exactly once with the new value.

### Manual smoke tests

- Toolbar buttons create the right entity kinds with the right defaults.
- Gizmo drag updates both the C# hierarchy (numbers in inspector) and
  the native viewport (visual position).
- Save → close project → reopen project → entities appear at saved
  transforms with all properties intact.
- `Ctrl+Z` after a gizmo drag reverses the change in both the C#
  hierarchy and the native viewport.

### Regression guard

- Existing 278/278 test suite must continue to pass. The
  `PropertiesViewModel.RecordAndApply` change adds a native push call;
  tests that don't load the engine should keep passing because
  `SyncEntitiesToNative` early-returns when `NativeEngineBridge.IsLoaded`
  is false.

---

## 7. Out of Scope (deliberately)

- **Free-fly camera** — existing orbit camera only.
- **Snap-to-grid / snap-to-angle** — would be nice but not requested.
- **Multi-entity box-select in viewport** — gizmo only operates on
  first selected entity today; preserved.
- **Per-mesh AABB picker** — still uses the 1m³ fallback. Documented
  as future work.
- **Light gizmo visualization** (light bulb icons / cone overlays) —
  out of scope for this iteration; can be added later by extending
  `GizmoOverlay`.
- **Camera gizmo visualization** (frustum preview) — same.
- **Real character (skinned mesh, animation)** — character is a
  placeholder capsule; documented as such.
- **Undo for "ActiveScene changed"** — pre-existing gap, not in scope.

---

## 8. Future-Proofing Notes

- `EntityFactory.All` is the registry — adding "Trigger Volume",
  "Audio Source", "Particle System", "Nav Mesh Agent" etc. is a
  one-line descriptor per type, no view changes.
- `EntityMetadata.Tags` is `ObservableCollection<string>` not a flag
  enum — supports arbitrary tags without touching this code.
- The default-material slot is per-synchronizer; if later we need
  multiple shared materials (e.g. white PBR, unlit, checker-debug), we
  promote `_defaultMaterialId` to a `Dictionary<string, ulong>` keyed
  by material role — no API surface change.
- `SetEntityTransform` already exists in the C ABI; we don't need new
  native calls for this iteration.

---

## 9. Implementation Order

1. **EntityFactory** + toolbar data binding (gives visible button
   surface for users to play with immediately).
2. **EntityMetadata component** + JsonDerivedType + registry +
   Inspector panel (persistence story complete).
3. **Gizmo fix + Rotate/Scale write-back** (most invasive; isolated
   to `WorldEditorView.axaml.cs` + `GizmoOverlay` event).
4. **Inspector → native push** (small change to PropertiesViewModel).
5. **Default white PBR material** (one new field in
   `SceneRuntimeSynchronizer` + two call-site updates).
6. **Tests** for each layer (unit + manual smoke).

Each step is independently shippable; we stop and verify after each.
