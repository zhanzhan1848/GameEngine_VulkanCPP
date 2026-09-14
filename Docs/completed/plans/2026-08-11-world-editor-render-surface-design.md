# World Editor Scene Manipulation (Round 2) — Design

**Date:** 2026-08-11
**Branch:** feat/avalonia-migration
**Builds on:** [2026-08-11-world-editor-scene-manipulation-design.md](./2026-08-11-world-editor-scene-manipulation-design.md) (Round 1: toolbar buttons + gizmo fix + entity metadata)
**Status:** Approved (scope locked)

---

## 1. Goal

Round 1 added the "place stuff in the scene" half. Round 2 adds the
**"make the scene actually look right and edit fluidly in the Render
Surface"** half:

- Every tool button (Light / Camera / Prop / Character) and every
  Inspector edit must **visually change the Render Surface** immediately.
- Selecting an entity in the hierarchy or by clicking the viewport must
  show a **bounding-box highlight** and place the gizmo at the entity's
  projected screen position (currently the gizmo is hard-locked to the
  viewport center).
- Each scene must own its **environment** (ambient light, fog, skybox
  color, tone mapping, exposure), persist with the scene, and feed
  the Forward renderer so two scenes look different.
- **Script / Animation component stubs** — stored on the entity,
  serialized to JSON, but not executed. Reserves the data path; the
  script engine lands in a future iteration.

Out of scope (deferred): real script runtime, character animation
runtime, per-pixel editor brushes, gizmo plane-drag, snap-to-grid.

---

## 2. Architecture overview

```
Round 1 delivered (kept):                Round 2 adds:
─────────────────────────                ─────────────
GameEntity + Components                  SceneEnvironment (new top-level on Scene)
Component hierarchy with JsonDerivedType  ScriptableComponent (base for Script/Animator stubs)
Toolbar buttons (Light/Camera/Prop/Char)  World↔screen projector in GizmoOverlay
Gizmo Translate/Rotate/Scale (fixed)      Selection bounding box highlight
Inspector Transform/Light/Camera/Mesh    Hover AABB highlight
EntityMetadata (IsPlayer/Tags/Desc)      Forward pipeline reads SceneEnvironment
Default white PBR material               DebugDraw P/Invoke wrappers for selection ring
```

**Critical constraint:** everything stays inside the existing
`PrimalEditor_Avalonia` C# project + `EngineDLL` C ABI surface.
Native-side changes are minimal and additive (no behavior changes
to existing passes).

---

## 3. New data model: SceneEnvironment

### 3.1 Schema (additive, no SchemaVersion bump)

`SceneDocument` gains one new field:

```csharp
public sealed class SceneEnvironmentDocument
{
    // Ambient
    public float AmbientR { get; set; } = 0.05f;
    public float AmbientG { get; set; } = 0.05f;
    public float AmbientB { get; set; } = 0.05f;
    public float AmbientIntensity { get; set; } = 1.0f;

    // Fog
    public bool FogEnabled { get; set; } = false;
    public float FogR { get; set; } = 0.7f;
    public float FogG { get; set; } = 0.75f;
    public float FogB { get; set; } = 0.8f;
    public float FogNear { get; set; } = 10f;
    public float FogFar { get; set; } = 200f;
    public float FogDensity { get; set; } = 0.01f;

    // Skybox (color-only for v1)
    public bool SkyboxEnabled { get; set; } = false;
    public float SkyR { get; set; } = 0.2f;
    public float SkyG { get; set; } = 0.4f;
    public float SkyB { get; set; } = 0.8f;

    // Tone mapping + exposure
    public enum ToneMappingMode { None, Reinhard, ACES, ExposureOnly }
    public ToneMappingMode ToneMapping { get; set; } = ToneMappingMode.ACES;
    public float Exposure { get; set; } = 1.0f;

    // Default main camera (optional — used by run-time spawn)
    public Guid? MainCameraEntityGuid { get; set; } = null;
}
```

JSON wire format lives on `SceneDocument.Environment` as
`SceneEnvironmentDocument`. Missing field in old v2 files → defaults.
`SceneDocument.IsValid` is **unchanged**: `SchemaVersion == 2` still
holds, additive deserialization is `System.Text.Json`'s default behavior.

### 3.2 C# runtime model

`PrimalEditor_Avalonia/GameProject/SceneEnvironment.cs`:

```csharp
public sealed class SceneEnvironment : ViewModelBase, IDirtyTrackable
{
    // mirrors SceneEnvironmentDocument 1:1
    public float AmbientR { get; set; } ...
    public ToneMappingMode ToneMapping { get; set; } ...
    public Guid? MainCameraEntityGuid { get; set; } ...
    // + OnPropertyChanged, IsDirty tracking
}
```

`Scene` gains:

```csharp
[JsonPropertyName("Environment")]
public SceneEnvironment Environment { get; set; } = new();
```

`IDirtyTrackable` makes Environment track project-dirty state when
the user edits it from the inspector.

### 3.3 Push to native

New helper on `WorldEditorViewModel`:

```csharp
public void PushEnvironmentToNative()
{
    if (!NativeEngineBridge.IsLoaded || ActiveScene == null) return;
    NativeEngineBridge.UpdateSceneEnvironment(/* 24 floats packed */);
}
```

Called from `PropertiesViewModel.RecordAndApply` whenever any
Environment field changes (single new `RecordAndApply` caller) and
from `SceneRuntimeSynchronizer.Sync` on scene activation.

### 3.4 Native ABI

**Don't add new exports** — reuse `RenderPipelineSettings` blob ABI
(`EngineDLL/RenderPipelineAPI.cpp:332-345`) which already has:

- `GetRenderPipelineSettings(out_settings_blob, in_size)` — read
- `UpdateRenderPipelineSettings(const* blob, in_size)` — write
- `GetRenderPipelineSettingsSize()` — query size

Extend `RenderPipelineSettings` (`Engine/Graphics/RenderPipeline/PipelineQualityConfig.h:115-125`)
with one new member `EnvironmentParams` (32 floats, padded to struct
alignment). C# side mirrors the struct via fixed-layout marshalling.

C# pattern already exists: `NativeEngineBridge.UpdateRenderPipelineSettings`
at `NativeEngineBridge.cs:389-418` — copy pattern for new field.

**ForwardSceneRenderer changes** (`Engine/Graphics/RenderPipeline/Modules/ForwardSceneRenderer.cpp`):

1. **Ambient** — re-add the ambient term to `DeferredLighting.metal:480-488`:
   replace the hardcoded `float3 ambient = float3(0.0f);` with a uniform
   read from `EnvironmentParams.AmbientRGB * AmbientIntensity`.

2. **Skybox** — the existing scaffolding (`ForwardSceneRenderer.h:121`
   declaration, shaders, pipeline, descriptors) is wired up to actually
   render a fullscreen gradient between two sky colors. One new pass
   inserted before Pass 1.

3. **Fog** — add a simple linear/exp fog term to the deferred lighting
   shader. Density/near/far from `EnvironmentParams.Fog*`. Off by
   default (current behavior preserved).

4. **Tone mapping** — `ToneMappingPass` (`Engine/Graphics/RenderPipeline/RenderPasses/ToneMapping/ToneMappingPass.h:13-22`)
   reads the mode enum + exposure from `EnvironmentParams` instead of
   hardcoded.

Each change is additive and behind a feature flag (defaults match
today's appearance exactly). Visual regressions are gated by manual
smoke test against the existing 17000-light test scene.

---

## 4. New data model: ScriptableComponent stubs

### 4.1 Why stubs

`Scene` must be runnable from a saved file. That means:

- A `ScriptableComponent` reference (path + params dict + enabled flag)
  on a `GameEntity` survives save/load.
- The runtime player can iterate "scripts attached to this scene" and
  log them — proving the data path is intact.
- No actual script execution. Real runtime lands later.

### 4.2 Component shape

`PrimalEditor_Avalonia/Components/ScriptableComponent.cs`:

```csharp
public abstract class ScriptableComponent : Component
{
    public abstract string ScriptTypeId { get; }  // e.g. "Rotate", "Patrol"
    public Dictionary<string, string> Parameters { get; set; } = new();
}
```

Two concrete subclasses:

- `RotateScriptComponent` — `ScriptTypeId = "Rotate"`, params:
  `SpeedX`, `SpeedY`, `SpeedZ` (degrees/sec).
- `PatrolScriptComponent` — `ScriptTypeId = "Patrol"`, params:
  `Path` (3-floats-per-waypoint, semicolon-separated), `Loop`.

Both registered via `[JsonDerivedType]` discriminators `"scriptRotate"`,
`"scriptPatrol"`. Add to `EditorComponentRegistry` and `Serializer`
switches.

### 4.3 Runtime "execution"

`PrimalEditor_Avalonia/Native/SceneRuntimeSynchronizer.Sync`:

After the per-entity push loop, add:

```csharp
foreach (var entity in _live)
{
    foreach (var script in entity.Components.OfType<ScriptableComponent>())
    {
        Logger.Info($"[Script] {entity.Name}: {script.ScriptTypeId} ({script.Parameters.Count} params)");
    }
}
```

This is a **load-only log**, not execution. Real implementation lands
with the script runtime.

---

## 5. Render Surface interactive editing

### 5.1 Gizmo follows selected entity

`Views/Engine3D/GizmoOverlay.cs` currently hard-codes the gizmo
anchor at viewport center (`:59-95`). Camera state is already passed
in via `UpdateCamera(yaw, pitch, distance, vw, vh)` (`:47-51`).

Port the projector from `Views/Engine3D/ViewportOverlay.cs:74-82`
(`Project(px, py, pz)`). Then read `_target` in `Render` and anchor
the three axes at `(target_sx, target_sy)` instead of `(w/2, h/2)`.

Near-plane rejection: `if (cw < 0.05f) return;` → hide the gizmo
entirely (don't draw at screen center as fallback).

### 5.2 Selection / hover bounding-box highlight

`NativeEngineBridge.cs` — add P/Invoke wrappers for:

```csharp
[DllImport("PrimalEngine", EntryPoint = "DrawDebugBoxWireframe")]
private static extern void _DrawDebugBoxWireframe(
    float cx, float cy, float cz, float ex, float ey, float ez, uint rgb);
public static void DrawDebugBoxWireframe(float cx, float cy, float cz,
    float ex, float ey, float ez, byte r, byte g, byte b)
{
    uint packed = ((uint)r << 16) | ((uint)g << 8) | b;
    _DrawDebugBoxWireframe(cx, cy, cz, ex, ey, ez, packed);
}
```

**Signature is `center + half-extents` (not min/max)** — confirmed in
`EngineDLL/DebugDrawAPI.cpp:27-30`. Caller converts min/max.

**Color caveat:** the existing `LineBatchRenderer` discards color and
draws single-color (`ForwardSceneRenderer.cpp:1833-1851`). To support
selection color (e.g. yellow for selection, cyan for hover), we have
two options:

- **A (cheap):** Two box wireframe calls; renderer always yellow, use
  `ClearDebugLines` between frames and accept the limitation. Selection
  just changes box size or fill style.
- **B (right):** Extend `LineBatchRenderer` to take a per-line color
  attribute. ~30 lines of C++. Selection gets a distinct color.

**Pick B** — it's small and unblocks future debug visualization needs.

### 5.3 Where AABBs come from

Per-entity AABB is not in the engine today (`WorldEditorViewModel.GetPickerCandidates`
uses a 1m³ fallback). For the bounding-box highlight, we accept the
same fallback for now (the box will be at the entity's position with
1m half-extents). Documented as a follow-up.

### 5.4 Hover detection

`EngineViewport3D.axaml.cs` already ray-casts on pointer move (see
`ViewportPicker`). Add a `HoverEntityChanged` event on the viewport
when the picker result changes. `WorldEditorView` subscribes and pushes
the entity into a `_hoverEntity` field that drives the cyan box.

---

## 6. Components / files touched

### New files

- `PrimalEditor_Avalonia/GameProject/SceneEnvironment.cs`
- `PrimalEditor_Avalonia/GameProject/Documents/SceneEnvironmentDocument.cs`
- `PrimalEditor_Avalonia/Components/ScriptableComponent.cs`
- `PrimalEditor_Avalonia/Components/RotateScriptComponent.cs`
- `PrimalEditor_Avalonia/Components/PatrolScriptComponent.cs`
- `PrimalEditor_Avalonia/Views/EnvironmentView.axaml(.cs)` — Inspector
  panel for SceneEnvironment
- `PrimalEditor_Avalonia.Tests/SceneEnvironmentTests.cs`
- `PrimalEditor_Avalonia.Tests/ScriptableComponentTests.cs`
- `PrimalEditor_Avalonia.Tests/ProjectToScreenTests.cs`

### Modified files

- `PrimalEditor_Avalonia/GameProject/Documents/SceneDocument.cs` — add
  `Environment` property
- `PrimalEditor_Avalonia/GameProject/Serializer.cs` — round-trip Environment
- `PrimalEditor_Avalonia/GameProject/Scene.cs` — expose `Environment`
  property
- `PrimalEditor_Avalonia/Components/Component.cs` — add 3
  `[JsonDerivedType]` discriminators
- `PrimalEditor_Avalonia/Components/EditorComponentRegistry.cs` —
  register 2 scripts
- `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs` — add
  `PushEnvironmentToNative()`, `SceneEnvironment` accessor
- `PrimalEditor_Avalonia/ViewModels/MainWindowViewModel.cs` — add
  `EnvironmentViewModel` selection state if needed
- `PrimalEditor_Avalonia/Views/PropertiesView.axaml` — add SceneEnvironment
  section + Script sections
- `PrimalEditor_Avalonia/Views/Engine3D/GizmoOverlay.cs` — port projector
  + anchor at target
- `PrimalEditor_Avalonia/Views/Engine3D/EngineViewport3D.axaml.cs` —
  hover event, push hover/selection to debug-line overlay
- `PrimalEditor_Avalonia/Native/NativeEngineBridge.cs` — P/Invokes for
  debug-draw + scene-environment blob push
- `PrimalEditor_Avalonia/Native/SceneRuntimeSynchronizer.cs` — push
  environment on Sync + log scripts
- `PrimalEditor_Avalonia/Native/INativeEngineBridgeLike.cs` — same

### Native side

- `Engine/Graphics/RenderPipeline/PipelineQualityConfig.h` — add
  `EnvironmentParams` to `RenderPipelineSettings`
- `Engine/Graphics/RenderPipeline/Modules/ForwardSceneRenderer.cpp/.h`
  — re-add ambient term, wire skybox pass, add fog term
- `Engine/Graphics/RenderPipeline/RenderPasses/ToneMapping/ToneMappingPass.cpp/.h`
  — read exposure from EnvironmentParams
- `Engine/Graphics/RenderPipeline/Modules/LineBatchRenderer.cpp/.h` —
  per-line color attribute
- `EngineDLL/DebugDrawAPI.cpp` — no change needed; the existing
  `DrawDebugBoxWireframe` already accepts the packed rgb we need
  (just currently ignored by renderer)

---

## 7. Data flow

```
User clicks [+ Light] in toolbar (Round 1)
  → WorldEditorView.OnAddEntityKindClick
  → WorldEditorViewModel.CreateEntityFromFactory(Light)
  → SceneRuntimeSynchronizer.SyncEntity(entity)
  → Native: CreateEntity, SetEntityTransform, AddEntityLight
  → ForwardSceneRenderer next frame: new light renders              ← R2 wires this

User changes SceneEnvironment.AmbientIntensity in Inspector
  → SceneEnvironment.AmbientIntensity setter (raises INPC)
  → PropertiesViewModel.RecordAndApply wraps it
  → WorldEditorViewModel.PushEnvironmentToNative
  → Native: UpdateRenderPipelineSettings(blob)
  → ForwardSceneRenderer next frame: ambient term reads new value    ← R2 wires this

User selects an entity in the hierarchy
  → WorldEditorViewModel.SelectedEntities setter
  → EngineViewport3D.SetSelectedEntity(nativeId)
  → On next pointer move / idle, draw selection box via DrawDebugBoxWireframe
  → GizmoOverlay.SetTarget(entity.Position) — projects to screen next frame
                                                                ← R2 wires this

User saves scene
  → Serializer.SaveSceneDocument writes Environment + ScriptableComponents
  → JSON v2 file has new fields, IsValid still passes (additive)

User reopens scene
  → Loader reads JSON, Environment defaults if missing
  → SceneRuntimeSynchronizer.Sync → PushEnvironmentToNative
  → ForwardSceneRenderer uses restored environment
                                                                ← R2 wires this
```

---

## 8. Error handling

| Failure | Behavior |
|---|---|
| `UpdateRenderPipelineSettings` returns 0 | Log warning, keep previous environment; don't crash |
| Gizmo target projects to behind-camera | Hide gizmo (`IsVisible = false` on overlay) |
| Selection entity has no AABB | Draw the 1m³ fallback (today's behavior) |
| Hover entity changes every frame | Debounce 50ms before issuing DrawDebugBoxWireframe (avoid flooding) |
| Scene JSON is missing Environment field | Default construct, `IsValid` still passes |
| ScriptableComponent references unknown type | Log warning at sync; skip the entity |

---

## 9. Testing strategy

### New unit tests

- **SceneEnvironmentTests:** round-trip through `Serializer`,
  `IsValid` unchanged when missing field, every field settable,
  `PushEnvironmentToNative` calls bridge correctly when loaded.
- **ScriptableComponentTests:** each subtype round-trips,
  `ScriptTypeId` is stable, parameters survive save/load.
- **ProjectToScreenTests:** pure-math unit tests of the projector
  helper extracted from `ViewportOverlay.Project` — forward of
  camera = identity, behind-camera returns null, 45° rotation matches
  trig.
- **SceneDocumentTests** (extend existing): confirm new Environment
  field doesn't break old-format loading.

### Manual smoke tests

- Click **+ Light** in toolbar → new light appears in scene immediately
- Move new light via gizmo → gizmo follows the light, not screen center
- Select an entity → bounding box visible in viewport, persists while
  moving the camera
- Hover different entities → cyan box follows the cursor
- Change `AmbientIntensity` 0→1 in Inspector → scene darkens
- Save scene → reopen → environment preserved
- Add `RotateScript` to an entity → save → reopen → entity still has it
- Click **Run** → scene log line "Script: Rotate (3 params)" appears

### Regression guard

- 387 passing tests must still pass
- 3 known-failing TextureCook tests must remain failing (unrelated)
- Native engine must build (CMake)

---

## 10. Future-proofing notes

- **Adding a new ScriptableComponent** = new class + new
  `[JsonDerivedType]` line + new registry entry + new log line in
  synchronizer. Runtime execution comes later but the data path is
  ready.
- **Adding a new Environment field** = add field to `SceneEnvironment`
  + `SceneEnvironmentDocument` + `EnvironmentParams` C++ struct +
  one shader uniform read. UI panel updates automatically via the
  binding.
- **LineBatchRenderer color attribute** unlocks future debug
  visualizations (path lines, normals, frustum culling visualization)
  without per-call RHI pipeline changes.
- **Gizmo plane-drag** will reuse the same projector — drop in when
  needed.

---

## 11. Implementation order

1. **SceneEnvironment data model** (no native work) — file + JSON +
   inspector panel + `PushEnvironmentToNative` (stubbed). Tests green.
2. **ScriptableComponent stubs** + JSON + registry + log in synchronizer.
   Tests green.
3. **GizmoOverlay follows target** (pure C# port). Manual smoke.
4. **DebugDraw P/Invokes + per-line color** (native + C#). Tests green.
5. **Selection / hover bounding box** (uses #4). Manual smoke.
6. **Native `EnvironmentParams` plumbing** (struct, blob push, shader
   reads). Manual smoke + visual regression.
7. **Full test suite + commit.**

Each step ships independently. Manual smoke between 3, 5, 6 because
they involve visual changes that test asserts can't capture.