# Model Import Pipeline (Phase 2 M13) — Defaults & ABI Contract

**Date:** 2026-08-14
**Status:** Shipped (M13 phase)
**Owner:** Editor team

## What this is

The Avalonia content browser's **Import Model** action now exposes every
ContentTools parameter as user-editable controls. Defaults are
`pipeline::Config`'s own defaults — LOD + Meshlet + Remesh + SDF + UV Atlas
ON, others OFF. Every flag and tuning param is overridable from the import
dialog before the cook runs.

## Engine ABI shape: why flat POD, not nested structs or JSON

`ai_asset_pipeline_params` in `ContentTools/ContentTools.cpp` is a flat POD
that carries **every** legacy geo_settings field plus every
`pipeline::Config` field, in scalar form. Two alternatives were rejected:

- **Nested structs (`repair_params`, `uvatlas_params`, …)**: would force
  the C# P/Invoke marshaling layer to either match nested layouts exactly
  (a constant source of ABI mismatch bugs) or pass them as `IntPtr`
  pointers (manual layout accounting). The codebase already commits to
  `u8` + sentinel values for every C++ enum — staying flat keeps the
  pattern consistent.
- **JSON string in one field**: would invalidate the typed ABI pattern
  the whole `ContentTools.cpp` file uses. Per-field defaulting logic in
  `ConfigurePipelineFromPOD` would also become impossible.

The flat POD cost: ~60 scalar fields, ~208 bytes total. A startup assertion
(`AIAssetPipelineSettings.AssertSize`) in `NativeEngineBridge` catches
field-order drift between C++ and C# — a mismatch crashes the editor
immediately rather than silently corrupting every import.

## The universal-sentinel rule

Every numeric field uses `0` (or `0.0f`) as **"use the pipeline::Config
module default for this field"**. The C# side sends `0` for any field the
user didn't touch, and a positive value to override. `ConfigurePipelineFromPOD`
in `ContentTools.cpp` implements the override-or-default logic.

This means callers don't have to mirror `pipeline::Config`'s internal
defaults. They override what they care about.

### One ABI trap (documented in the POD struct comments)

`repair::Params::max_hole_size = 0` originally meant "unlimited" in the
legacy `repair.h` API. Our POD's universal `0 = use default` sentinel
collides with that. Resolution: callers get the Config default (30) by
sending `0`; a positive value sets the cap; "unlimited" is no longer
reachable through the ABI (use a very large value instead).

### One subtle defaults trap (documented in `ConfigurePipelineFromPOD`)

`derive_normal_mode = 0` casts to `derive::NormalMode::Faceted` (hard-edge
mode that duplicates verts), **NOT** to `AreaWeighted` (which is the
`pipeline::Config` default). Caller who wants `AreaWeighted` must
explicitly send `2`. Same caveat for `derive_tangent_mode = 0` which casts
to `None`.

This is an artifact of C++ enum value 0 being a meaningful mode in both
modules. Documented inline so future maintainers don't "fix" the cast.

## Defaults contract: what users see on a fresh import

`GeometryImportSettings` defaults match `pipeline::Config` defaults plus
ContentTools' legacy SDF behavior:

| Module    | Default | Why |
|-----------|---------|-----|
| Repair    | OFF | Geometric cleanup is opt-in per asset |
| Remesh    | **ON** | Split-only long-edge bisection is nearly-free (no collapse), breaks giant triangles without losing detail |
| Subdivide | OFF | Multiplies triangle count by 4^N; expensive |
| Derive    | OFF | Standalone (independent of subdivide cascade) |
| UV Atlas  | **ON** | Required for texture sampling on lightmap-less assets |
| LOD       | **ON** | Matches `pipeline::Config` default; legacy `CookMesh` produces LODs too |
| Meshlet   | **ON** | Matches `pipeline::Config` default; runtime cap is 256/128 |
| Collision | OFF | Engine has no consumer yet; VHACD is slow |
| SDF       | **ON** | Matches legacy `CookMesh` behavior (`enable_sdf = true` hardcoded before M13) |

**Implication for existing assets:** re-cooking an already-imported `.mesh`
with the new defaults will produce **different bytes** even when the user
didn't touch the dialog. Specifically: split-only remesh will break giant
triangles, LOD chain will re-flow through `pipeline::Run` instead of
`Geometry.cpp::pack_data`, and meshlet packing will use the 256/128 cap
instead of legacy 64/124. This is intentional — it's what ContentTools'
"own defaults" produce — but artists should be told.

The import status message includes pipeline output counts
(`meshes=N, meshlets=N`) and surfaces validation_errors/warnings in the
log so reimports that silently degrade quality are visible.

## UI surface: two-stage layout

`GeometryImportPanel.axaml` is now two clear groups:

1. **FBX / OBJ Import** — legacy geo_settings (smoothing angle, calculate
   normals/tangents, reverse handedness, etc.). These control FBX/OBJ
   parsing BEFORE the pipeline runs.
2. **Asset Pipeline** — per-module Expanders (Repair / Remesh / Subdivide
   / Derive / UV Atlas / LOD / Meshlet / Collision / SDF). Each module
   has an enable checkbox at the top, primary params below, an
   "Advanced" toggle that reveals PMP/VHACD internal knobs (adaptive
   approx error, PCA, downsampling, etc.).

Modules' hidden-by-default advanced knobs (`repair_max_hole_size`,
`remesh_adaptive_approx_error`, `collision_pca`, etc.) are kept out of the
primary view because they're implementation details most artists don't
need to touch. The `ShowAdvanced` checkbox toggles their visibility
panel-wide.

For non-FBX/OBJ files (.gltf, .glb, .dae, .blend), the pipeline section
is greyed out via `PipelineAvailable = false` because ContentTools'
pipeline path only handles .fbx and .obj. The legacy import path still
runs for these formats and ignores the pipeline flags.

## Schema migration: binary frozen, JSON extended

`ToBinary`/`FromBinary` are **frozen at the legacy 15-field schema**.
Existing `.meta` sidecars keep working — the binary reader tolerates
truncation via position-checks (the original `if (r.BaseStream.Position
< r.BaseStream.Length)` pattern).

All new pipeline fields are JSON-only. The `FromJson`/`ToJson` paths
round-trip every field. JSON has been the canonical format since the
sidecar store landed (Format Version 1 + magic `PRIM`), so this is a
backward-compatible extension.

## Files touched (M13)

### C++ engine
- `ContentTools/ContentTools.cpp` — extended `ai_asset_pipeline_params`,
  rewrote `ConfigurePipelineFromPOD`, added `CookMeshWithPipeline` ABI.

### C# bridge
- `PrimalEditor_Avalonia/Native/AIAssetPipelineSettings.cs` — new POD
  mirror struct (208 bytes, sequential layout).
- `PrimalEditor_Avalonia/Native/NativeEngineBridge.cs` — added
  `CookMeshWithPipeline` P/Invoke + wrapper, added `AssertSize()` in
  static constructor.

### C# settings + UI
- `PrimalEditor_Avalonia/Editors/Geometry/GeometryImportSettings.cs` —
  added 47 new fields, JSON-only round-trip, `ToPOD()` flattener.
- `PrimalEditor_Avalonia/Import/Settings/GeometryImportPanel.axaml` —
  rebuilt with two groups + per-module Expanders + advanced toggle.
- `PrimalEditor_Avalonia/Import/Settings/GeometryImportPanelViewModel.cs`
  — added all property bindings, `ShowAdvanced` and `PipelineAvailable`
  visibility flags.
- `PrimalEditor_Avalonia/Import/ImportSettingsDialog.axaml.cs` — detects
  non-FBX/OBJ files and sets `PipelineAvailable = false` on the panel VM.

### Wire-up
- `PrimalEditor_Avalonia/Native/Importers/MeshImporterPlugin.cs` —
  switched from `CookMesh(src, dst, settings)` to
  `CookMeshWithPipeline(src, dst, settings.ToPOD())`, surfaces validation
  errors/warnings in the result log.

### Tests
- `PrimalEditor_Avalonia.Tests/Geometry/GeometryImportSettingsTests.cs`
  — added ABI size assertion, ToPOD field-mapping tests.

## What this intentionally does NOT do

- **`CookMesh(src, dst)` short-form**: preserved byte-identical for
  regression smoke tests and `GeometryReimportService` (which wants
  byte-stable behavior). The reimport path is intentionally conservative.
- **`ProcessAIAsset` direct invocation from C#**: not added. C# goes
  through `CookMeshWithPipeline` which handles FBX parse + pipeline +
  file write + sidecar in one call.
- **SDF regeneration as the default**: matches legacy `CookMesh` behavior.
  Disabling SDF is faster but loses data the runtime relies on (Lumen
  surface cache, Nanite culling). Off by default in `pipeline::Config`
  but **on** for the editor import path so artists get what legacy
  imports produced.
