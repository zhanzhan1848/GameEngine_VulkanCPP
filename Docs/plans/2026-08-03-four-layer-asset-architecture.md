# Four-Layer Asset Architecture Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a strict four-layer project and asset architecture in which source description files are authoritative, SQLite and cooked artifacts are rebuildable, and all engine/runtime handles remain process-local.

**Architecture:** The Avalonia editor owns source serialization, stable GUIDs, dependency indexing, import scheduling, cooking, migrations, and GUID-to-runtime mappings. `EngineDLL` remains a stable C ABI over core ECS/content/render systems and consumes cooked artifacts without knowing about SQLite or editor source schemas. Project, Scene, Material, Shader, and `.meta` files are the only authoritative persisted state; `.Primal/AssetDatabase.sqlite` and `.Primal/Cache/` may always be deleted and rebuilt.

**Tech Stack:** .NET 8, Avalonia 11, System.Text.Json, xUnit, Microsoft.Data.Sqlite, C++17, CMake/CTest, EngineDLL C ABI, existing ContentTools importers.

---

## 1. Non-negotiable data ownership

| Layer | Data | Authority | May be deleted | IDs allowed |
|---|---|---:|---:|---|
| Source descriptions | `.primal`, `.scene`, `.material`, `.shader`, source files, `.meta` | Yes | No | Stable GUIDs only |
| SQLite asset database | Assets, dependencies, hashes, import state, artifact records, thumbnail records, jobs | No | Yes | Stable GUIDs and relative paths |
| Cooked cache | Platform/configuration-specific mesh, texture, shader and manifest artifacts | No | Yes | Stable GUID in keys/headers; no runtime handles |
| Core runtime | ECS entities, Content IDs, runtime materials, RHI handles | Runtime only | On process exit | Process-local IDs only |

Enforce these invariants in tests:

- `.primal` contains project settings and Scene GUID references, never entity graphs.
- `.scene` is the only authority for scene entities and components.
- `.material` and `.shader` remain source authorities; SQLite never stores their authoritative parameters or source code.
- Core Entity ID, Content ID, RHI handle and Runtime Material ID never appear in source JSON, SQLite records or cooked manifests.
- Every persisted path is normalized and project-relative. Absolute paths are only transient resolver outputs.
- `.Primal/` is the only derived-data directory spelling. Migrate and retire lowercase `.primal/`.

## 2. Current repository findings that drive the plan

- `PrimalEditor_Avalonia/GameProject/Project.cs:101-120` currently serializes complete `Scene` objects into `.primal`.
- `PrimalEditor_Avalonia/GameProject/NewProject.cs:44-56` also writes each Scene to `Scenes/<name>.scene`, creating two authorities.
- `PrimalEditor_Avalonia/Components/GameEntity.cs:57-66` persists an absolute `AssetPath`.
- `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs:301-305` writes `AssetInfo.FullPath` into `GameEntity.AssetPath` and lines 364-408 reload by that path.
- `PrimalEditor_Avalonia/Content/AssetRegistry.cs:17-20,127-170` keeps stable GUIDs in a path-keyed `.primal/asset-guids.json`; moving a resource changes its identity, and the directory casing conflicts with `.Primal/`.
- `PrimalEditor_Avalonia/Common/AssetImportSettingsStore.cs` writes binary `.meta` files containing import settings but no stable GUID/importer envelope.
- `PrimalEditor_Avalonia/Native/AssetImporter.cs:138-163` cooks `.mesh` beside source content and immediately turns it into a runtime Content ID.
- `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs:239-252` already keeps editor Entity-to-Core ID mappings in memory; preserve this concept but key it by Entity GUID.
- `EngineDLL/EntityAPI.cpp`, `LightAPI.cpp`, `AssetAPI.h/.cpp`, `MaterialAPI.cpp`, and `ShaderCompilerAPI.cpp` already expose runtime APIs and process-local IDs. Extend these contracts rather than serializing C++ objects.
- Editor tests use xUnit in `PrimalEditor_Avalonia.Tests`; engine tests use CMake/CTest under `EngineTest`.

## 3. Target source contracts

### Project v2

Create a thin manifest with:

```json
{
  "SchemaVersion": 2,
  "ProjectGuid": "...",
  "Name": "MyGame",
  "BuildConfig": "DebugEditor",
  "DefaultScene": "...",
  "Scenes": ["..."],
  "RenderSettings": {
    "Pipeline": "Standard",
    "ColorSpace": "Linear"
  }
}
```

Do not serialize `Project.Path`; derive the root from the opened `.primal` file. Do not serialize `ActiveScene` or editor UI state.

### Scene v2

Persist `SceneGuid`, name, Entity GUIDs, optional parent Entity GUID, enabled state, and polymorphic component payloads. Persist hierarchy through parent GUIDs or a single canonical children representation, not both mutable parent and children graphs.

### Meta v1 JSON

Use one complete JSON document rather than a JSON header plus binary body:

```json
{
  "MetaVersion": 1,
  "Guid": "...",
  "Importer": "ModelImporter",
  "Settings": { }
}
```

This is the recommended decision because GUID and settings changes become Git-reviewable. Keep a legacy binary reader only for one-way migration; all new writes use JSON.

### Asset reference

Persist only the stable GUID in components and descriptors. A relative-path diagnostic hint may exist in editor-only memory but must not participate in identity. Resolution is `GUID -> SQLite row -> project-relative source path -> artifact row -> cooked path`.

### Artifact key

Use this deterministic tuple:

```text
(asset_guid, target_platform, build_configuration, artifact_type,
 source_hash, meta_hash, importer_version, toolchain_version)
```

Do not name artifacts only by GUID; a source/settings/compiler change must produce a distinct cache key or invalidate the existing record.

---

## Phase 0: Add architecture guard tests and test fixtures

### Task 1: Establish serialization fixtures and forbidden-field guards

**Files:**
- Create: `PrimalEditor_Avalonia.Tests/Fixtures/Projects/V1/EmbeddedScenes.primal`
- Create: `PrimalEditor_Avalonia.Tests/Fixtures/Projects/V1/Content/Models/robot.fbx.meta`
- Create: `PrimalEditor_Avalonia.Tests/GameProject/PersistenceBoundaryTests.cs`
- Create: `PrimalEditor_Avalonia.Tests/TestInfrastructure/TemporaryProject.cs`

**Steps:**

1. Add a minimal legacy `.primal` fixture containing an embedded Scene and an entity with `AssetPath`.
2. Add a helper that creates and removes isolated temporary project roots.
3. Write failing tests that serialize Project and Scene and reject `EntityId`, `ContentId`, `RhiHandle`, `RuntimeMaterialId`, `NativeHandle`, and absolute paths.
4. Run:

```bash
dotnet test PrimalEditor_Avalonia.Tests/PrimalEditor_Avalonia.Tests.csproj -c DebugEditor --filter PersistenceBoundaryTests
```

Expected: tests fail because Project embeds Scenes and Scene entities still contain `AssetPath`.

5. Keep these tests as permanent architecture gates.

**Commit:** `test(asset): add four-layer persistence boundary fixtures`

---

## Phase 1: Define stable GUID and source-document primitives

### Task 2: Add strongly typed GUID and project-relative path values

**Files:**
- Create: `PrimalEditor_Avalonia/GameProject/Identifiers/ProjectGuid.cs`
- Create: `PrimalEditor_Avalonia/GameProject/Identifiers/SceneGuid.cs`
- Create: `PrimalEditor_Avalonia/GameProject/Identifiers/EntityGuid.cs`
- Create: `PrimalEditor_Avalonia/Content/AssetGuid.cs`
- Create: `PrimalEditor_Avalonia/Content/ProjectRelativePath.cs`
- Create: `PrimalEditor_Avalonia.Tests/GameProject/IdentifierSerializationTests.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/ProjectRelativePathTests.cs`

**Steps:**

1. Write tests for lowercase canonical GUID serialization, invalid/empty values, equality, and JSON round trips.
2. Write tests that reject rooted paths, `..` traversal, paths outside the project root, and platform separators in persisted output.
3. Implement small immutable value types with explicit parsing and JSON converters.
4. Use `/` as the persisted separator and `Path.GetFullPath` only in the resolver.
5. Run the focused tests and then the whole C# test project.

**Commit:** `feat(asset): add stable GUID and project-relative path primitives`

### Task 3: Replace binary meta writes with versioned JSON meta documents

**Files:**
- Create: `PrimalEditor_Avalonia/Content/Metadata/AssetMetaDocument.cs`
- Create: `PrimalEditor_Avalonia/Content/Metadata/IAssetMetaStore.cs`
- Create: `PrimalEditor_Avalonia/Content/Metadata/JsonAssetMetaStore.cs`
- Create: `PrimalEditor_Avalonia/Content/Metadata/LegacyBinaryMetaReader.cs`
- Modify: `PrimalEditor_Avalonia/Common/AssetImportSettingsStore.cs`
- Modify: `PrimalEditor_Avalonia/Content/AssetImportSettings.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/AssetMetaStoreTests.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/LegacyBinaryMetaMigrationTests.cs`

**Steps:**

1. Write failing tests for JSON round trip, importer discriminator, typed settings, stable GUID, deterministic formatting, unknown settings preservation, and atomic save.
2. Write a failing test that reads the current `PRIM` binary format and emits an equivalent JSON meta with a new stable GUID.
3. Implement `AssetMetaDocument` with `MetaVersion`, `AssetGuid`, `Importer`, and `JsonObject Settings`.
4. Implement atomic writes as sibling `.partial`, flush-to-disk, then replace. Never delete the last valid target before replacement.
5. Preserve the existing binary reader only as migration input. Remove binary write behavior after migration tests pass.
6. Ensure failed migration leaves the old meta untouched and logs a recoverable error.

**Commit:** `feat(asset): introduce JSON meta authority with stable GUIDs`

---

## Phase 2: Split Project and Scene authorities

### Task 4: Define Project v2 as a thin manifest

**Files:**
- Create: `PrimalEditor_Avalonia/GameProject/Documents/ProjectDocument.cs`
- Create: `PrimalEditor_Avalonia/GameProject/Documents/RenderSettingsDocument.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/Project.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/Serializer.cs`
- Create: `PrimalEditor_Avalonia.Tests/GameProject/ProjectDocumentTests.cs`

**Steps:**

1. Write a failing test that saves a Project with a populated runtime Scene and asserts the `.primal` JSON contains only Project GUID, config, default Scene GUID, Scene GUID list, and render settings.
2. Write a failing test that loads a manifest from a different filesystem location and derives `Project.Path` from the opened file rather than persisted data.
3. Introduce a serializable `ProjectDocument` separate from the mutable UI `Project` model.
4. Map `Project -> ProjectDocument` and `ProjectDocument -> Project`; do not serialize ViewModel classes directly.
5. Replace integer BuildConfig persistence with the stable string enum contract while accepting the v1 integer during migration.
6. Update `Serializer.SaveProject` to write `ProjectDocument` atomically.

**Commit:** `feat(project): make primal file a thin versioned manifest`

### Task 5: Define Scene v2 and component DTOs

**Files:**
- Create: `PrimalEditor_Avalonia/GameProject/Documents/SceneDocument.cs`
- Create: `PrimalEditor_Avalonia/GameProject/Documents/EntityDocument.cs`
- Create: `PrimalEditor_Avalonia/Components/Documents/ComponentDocument.cs`
- Create: `PrimalEditor_Avalonia/Components/Documents/TransformDocument.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/Scene.cs`
- Modify: `PrimalEditor_Avalonia/Components/GameEntity.cs`
- Modify: `PrimalEditor_Avalonia/Components/Component.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/Serializer.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/NewProject.cs`
- Modify: `PrimalEditor_Avalonia/Common/EditorSaveService.cs`
- Create: `PrimalEditor_Avalonia.Tests/GameProject/SceneDocumentTests.cs`
- Create: `PrimalEditor_Avalonia.Tests/GameProject/SceneAtomicSaveTests.cs`

**Steps:**

1. Write failing round-trip tests for Scene GUID, Entity GUID, name, parent GUID, enabled state, Transform, and unknown component rejection with a useful error.
2. Write a failing test proving runtime/editor fields (`ParentScene`, `Parent`, `IsActive`, native maps) are absent from Scene JSON.
3. Introduce DTOs separate from ViewModels and a component discriminator matching the requested `$type` contract.
4. Add stable `EntityGuid` generation at entity creation and preserve it on duplicate/undo semantics: duplicate creates new GUIDs; undo restores the same instance/GUID.
5. Store hierarchy in `Parent` GUID only. Rebuild `Parent`/`Children` runtime links after all entities are instantiated; reject cycles and missing parents.
6. Add `Serializer.SaveScene` and `LoadScene` with atomic replace and schema validation.
7. Make `EditorSaveService.SaveAll` save dirty Scenes first, validate them, and save the Project manifest last so the manifest never points at an unwritten Scene.

**Commit:** `feat(scene): establish scene files as sole entity authority`

### Task 6: Migrate embedded Project v1 Scenes to Project/Scene v2

**Files:**
- Create: `PrimalEditor_Avalonia/GameProject/Migration/IProjectMigration.cs`
- Create: `PrimalEditor_Avalonia/GameProject/Migration/ProjectV1ToV2Migration.cs`
- Create: `PrimalEditor_Avalonia/GameProject/Migration/ProjectMigrationService.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/OpenProject.cs`
- Create: `PrimalEditor_Avalonia.Tests/GameProject/ProjectV1ToV2MigrationTests.cs`

**Steps:**

1. Write a failing migration test using the Phase 0 fixture.
2. Assert migrated entity/component payloads are semantically equal, Scene and Entity GUIDs are generated once, absolute `AssetPath` becomes the corresponding asset GUID, and `.primal` no longer contains entities.
3. Implement migration in a staging directory under `.Primal/Temp/Migration/<transaction-id>`.
4. Validate staged Project and all Scenes by reading them back before replacing originals.
5. Create a timestamped backup of v1 source files and never auto-delete it.
6. Make migration idempotent: opening v2 twice produces no writes or GUID changes.
7. On any failure, retain original files, remove only staging output, and return a user-visible migration error.

**Commit:** `feat(project): migrate embedded scenes to schema v2 safely`

---

## Phase 3: Add serializable rendering components and GUID references

### Task 7: Add MeshRenderer source component

**Files:**
- Create: `PrimalEditor_Avalonia/Components/MeshRendererComponent.cs`
- Create: `PrimalEditor_Avalonia/Components/Documents/MeshRendererDocument.cs`
- Modify: `PrimalEditor_Avalonia/Components/Component.cs`
- Modify: `PrimalEditor_Avalonia/Components/EditorComponentRegistry.cs`
- Modify: `PrimalEditor_Avalonia/Components/GameEntity.cs`
- Modify: `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs`
- Create: `PrimalEditor_Avalonia.Tests/Components/MeshRendererSerializationTests.cs`

**Steps:**

1. Write failing tests for Mesh GUID, ordered Material GUID list, cast shadow, receive shadow, and no absolute path/runtime Content ID.
2. Implement MeshRenderer DTO/model and register its discriminator.
3. Remove persisted `GameEntity.AssetPath`; migration is now the only reader of the old field.
4. Change drag/drop to assign `AssetInfo.Guid` to `MeshRenderer.MeshGuid`.
5. Defer native loading to the scene runtime synchronizer instead of importing inside the source-model mutation path.

**Commit:** `feat(scene): replace entity AssetPath with MeshRenderer GUID references`

### Task 8: Add Light and Camera source components

**Files:**
- Create: `PrimalEditor_Avalonia/Components/LightComponent.cs`
- Create: `PrimalEditor_Avalonia/Components/CameraComponent.cs`
- Create: `PrimalEditor_Avalonia/Components/Documents/LightDocument.cs`
- Create: `PrimalEditor_Avalonia/Components/Documents/CameraDocument.cs`
- Modify: `PrimalEditor_Avalonia/Components/Component.cs`
- Modify: `PrimalEditor_Avalonia/Components/EditorComponentRegistry.cs`
- Modify: `PrimalEditor_Avalonia/Native/NativeEngineBridge.cs`
- Create: `PrimalEditor_Avalonia.Tests/Components/LightSerializationTests.cs`
- Create: `PrimalEditor_Avalonia.Tests/Components/CameraSerializationTests.cs`

**Steps:**

1. Write failing serialization and validation tests for light type, color, intensity, range/cones, enabled and cast-shadow state.
2. Write Camera tests for projection type, FOV/orthographic size, near/far planes, clear state and enabled state.
3. Implement editor source components independent of native IDs.
4. Reuse existing `EngineDLL/LightAPI.cpp` and camera exports through `NativeEngineBridge`; add only missing C ABI setters after a failing ABI integration test demonstrates the gap.
5. Keep all returned Entity IDs in runtime maps.

**Commit:** `feat(scene): add persistent light and camera components`

---

## Phase 4: Material and Shader source resources

### Task 9: Add `.material` documents and asset references

**Files:**
- Create: `PrimalEditor_Avalonia/Content/Materials/MaterialDocument.cs`
- Create: `PrimalEditor_Avalonia/Content/Materials/MaterialSerializer.cs`
- Create: `PrimalEditor_Avalonia/Content/Materials/MaterialParameterValue.cs`
- Modify: `PrimalEditor_Avalonia/Content/ContentHelper.cs`
- Modify: `PrimalEditor_Avalonia/Native/AssetImporter.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/MaterialDocumentTests.cs`

**Steps:**

1. Write failing round-trip tests for Shader GUID, render state, scalar/vector parameters, texture GUID slots and schema version.
2. Reject Content IDs, texture handles and absolute paths.
3. Implement a deterministic serializer with stable property ordering.
4. Register `.material` as an asset type but do not cook or load it during filesystem scan.
5. Add dependency extraction `Material -> Shader/Texture` as a pure function; SQLite integration comes later.
6. Treat shared Material as immutable by scene instances; plan Material Instance or scene override as a later source component rather than mutating the shared resource.

**Commit:** `feat(material): add authoritative material resource documents`

### Task 10: Add `.shader` descriptors and source variants

**Files:**
- Create: `PrimalEditor_Avalonia/Content/Shaders/ShaderDocument.cs`
- Create: `PrimalEditor_Avalonia/Content/Shaders/ShaderSerializer.cs`
- Create: `PrimalEditor_Avalonia/Content/Shaders/ShaderDependencyExtractor.cs`
- Modify: `PrimalEditor_Avalonia/Content/ContentHelper.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/ShaderDocumentTests.cs`

**Steps:**

1. Write failing tests for language variants, entry points, techniques, keywords, source-relative paths and Include dependencies.
2. Implement `.shader` parsing/serialization without embedding `.metal`, `.hlsl` or `.glsl` source.
3. Validate referenced source files remain under the project Content root.
4. Extract `Shader -> source/include` dependencies for later SQLite indexing.

**Commit:** `feat(shader): add authoritative shader descriptor resources`

---

## Phase 5: SQLite Asset Database as a rebuildable editor index

### Task 11: Add the editor-only SQLite package and schema

**Files:**
- Modify: `PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj`
- Create: `PrimalEditor_Avalonia/Content/Database/IAssetDatabase.cs`
- Create: `PrimalEditor_Avalonia/Content/Database/SqliteAssetDatabase.cs`
- Create: `PrimalEditor_Avalonia/Content/Database/AssetDatabaseSchema.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/AssetDatabaseSchemaTests.cs`

**Steps:**

1. Add `Microsoft.Data.Sqlite` to the Avalonia editor project only.
2. Write failing in-memory schema tests for:
   - `Assets(guid, relative_path, asset_type, source_hash, meta_hash, last_write_time, importer, import_status, error_message)`
   - `Dependencies(owner_guid, dependency_guid, dependency_type)`
   - `Artifacts(asset_guid, platform, configuration, artifact_type, artifact_path, artifact_hash, compiler_version)`
   - `Thumbnails(asset_guid, thumbnail_path, source_hash)`
   - `ImportJobs(asset_guid, status, started_at, finished_at, error_message)`
3. Add foreign keys, unique constraints, indexes by relative path/status/dependency, and a DB schema-version table.
4. Use transactions for scans and jobs; enable foreign keys and WAL mode.
5. Add a static boundary test or script that fails if `Microsoft.Data.Sqlite` appears under `Engine/` or `EngineDLL/`.

**Commit:** `feat(assetdb): add editor-only SQLite asset index schema`

### Task 12: Rebuild the database from source files and `.meta`

**Files:**
- Create: `PrimalEditor_Avalonia/Content/Database/AssetDatabaseRebuilder.cs`
- Create: `PrimalEditor_Avalonia/Content/Hashing/AssetHasher.cs`
- Modify: `PrimalEditor_Avalonia/Content/AssetRegistry.cs`
- Modify: `PrimalEditor_Avalonia/Content/AssetInfo.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/OpenProject.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/AssetDatabaseRebuildTests.cs`

**Steps:**

1. Write tests that scan Project, Scenes and Content, read or create `.meta`, normalize paths, compute separate source/meta hashes, and populate Assets.
2. Write tests for duplicate GUIDs, duplicate normalized paths, missing meta, orphan meta, corrupt source documents and case-only path collisions.
3. Implement deterministic rebuild into a temporary database file, validate it, then replace `AssetDatabase.sqlite` atomically.
4. Make project open recover from missing/corrupt/incompatible DB by rebuilding from source.
5. Refactor `AssetRegistry` into an in-memory projection over the DB. Remove `.primal/asset-guids.json` writes; add a one-time importer only to migrate old GUIDs when no `.meta` exists.
6. Do not remove a resource GUID when a file is temporarily deleted during a rename debounce window; process move/rename as one transaction.

**Commit:** `feat(assetdb): rebuild asset index from authoritative source data`

### Task 13: Index dependencies and incremental watcher changes

**Files:**
- Create: `PrimalEditor_Avalonia/Content/Database/AssetDependencyIndexer.cs`
- Modify: `PrimalEditor_Avalonia/Content/ContentWatcher.cs`
- Modify: `PrimalEditor_Avalonia/Content/AssetRegistry.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/AssetDependencyIndexerTests.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/ContentWatcherDatabaseTests.cs`

**Steps:**

1. Write dependency tests for Scene to Mesh/Material, Material to Shader/Texture, and Shader to source/include files.
2. Write cycle tests: cycles are allowed in the generic graph only when the asset type permits them; cooking must detect and report unresolved cycles rather than recurse forever.
3. Change watcher handling to queue events and reconcile actual filesystem state after debounce, instead of writing the DB from raw events immediately.
4. Preserve GUID through rename/move by moving source and `.meta` together and updating only `relative_path`.
5. Mark reverse dependents dirty when source or meta hashes change.

**Commit:** `feat(assetdb): index dependencies and incremental content changes`

---

## Phase 6: Cook pipeline and platform cache

### Task 14: Define cook requests, artifact keys and cache layout

**Files:**
- Create: `PrimalEditor_Avalonia/Content/Cooking/CookRequest.cs`
- Create: `PrimalEditor_Avalonia/Content/Cooking/ArtifactKey.cs`
- Create: `PrimalEditor_Avalonia/Content/Cooking/CookCacheLayout.cs`
- Create: `PrimalEditor_Avalonia/Content/Cooking/ICooker.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/CookCacheLayoutTests.cs`

**Steps:**

1. Write tests for macOS/Windows/Linux and DebugEditor/ReleaseEditor isolation.
2. Include source hash, meta hash, importer version and toolchain/compiler version in the key.
3. Define paths under `.Primal/Cache/<platform>/<configuration>/<asset-guid>/<artifact-key>.<ext>`.
4. Reject cache paths outside `.Primal/Cache`.
5. Add stale partial cleanup and atomic publication semantics.

**Commit:** `feat(cook): define deterministic artifact keys and cache layout`

### Task 15: Move mesh cooking out of `Content/`

**Files:**
- Create: `PrimalEditor_Avalonia/Content/Cooking/ModelCooker.cs`
- Modify: `PrimalEditor_Avalonia/Native/AssetImporter.cs`
- Modify: `PrimalEditor_Avalonia/Native/NativeEngineBridge.cs`
- Modify: `ContentTools/ContentTools.cpp`
- Modify: `ContentTools/CMakeLists.txt`
- Create: `PrimalEditor_Avalonia.Tests/Content/ModelCookerTests.cs`

**Steps:**

1. Write a failing test proving `.fbx` import produces no sibling `.mesh` and records the artifact under `.Primal/Cache`.
2. Refactor import into two operations: copy source/create meta, then enqueue cook.
3. Make `ModelCooker` call ContentTools with explicit source path, output path and importer settings.
4. Validate the `.partial` cooked mesh before publishing and recording the Artifacts row.
5. Cache hit requires a full ArtifactKey match, not timestamps alone.
6. Keep runtime mesh loading separate: it consumes the recorded cooked path and returns an ephemeral Content ID.

**Commit:** `feat(cook): relocate model artifacts to platform cache`

### Task 16: Add Texture and Shader cookers

**Files:**
- Create: `PrimalEditor_Avalonia/Content/Cooking/TextureCooker.cs`
- Create: `PrimalEditor_Avalonia/Content/Cooking/ShaderCooker.cs`
- Modify: `PrimalEditor_Avalonia/Native/AssetImporter.cs`
- Modify: `EngineDLL/ShaderCompilerAPI.cpp`
- Modify: `EngineDLL/AssetAPI.h`
- Modify: `EngineDLL/AssetAPI.cpp`
- Create: `PrimalEditor_Avalonia.Tests/Content/TextureCookerTests.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/ShaderCookerTests.cs`

**Steps:**

1. Write tests for settings-sensitive texture artifacts and platform shader variants.
2. Make shader cooking emit compiled artifacts and diagnostics into `.Primal/Logs`; never store shader source in SQLite.
3. Record compiler/toolchain versions in Artifacts.
4. Invalidate Material dependents when Shader or Texture artifacts change.
5. Keep existing runtime upload handles local to `NativeEngineBridge`/EngineDLL.

**Commit:** `feat(cook): add texture and shader artifact pipelines`

### Task 17: Add import jobs, cancellation and crash recovery

**Files:**
- Create: `PrimalEditor_Avalonia/Content/Cooking/ImportJobScheduler.cs`
- Create: `PrimalEditor_Avalonia/Content/Cooking/ImportJob.cs`
- Modify: `PrimalEditor_Avalonia/Native/AssetImporter.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/ImportJobSchedulerTests.cs`

**Steps:**

1. Write state-machine tests for Queued, Running, Succeeded, Failed and Cancelled.
2. Persist status in ImportJobs but keep worker/request handles in memory only.
3. On startup, turn stale Running rows into Interrupted and requeue if inputs still exist.
4. Deduplicate requests by ArtifactKey and propagate cancellation without deleting previously valid artifacts.
5. Publish DB artifact rows and files transactionally: file first, DB transaction second; orphan files may be garbage-collected, but DB must never reference a missing artifact.

**Commit:** `feat(cook): add resilient import job scheduling and recovery`

---

## Phase 7: Editor-to-runtime synchronization

### Task 18: Introduce GUID-keyed runtime mapping tables

**Files:**
- Create: `PrimalEditor_Avalonia/Native/RuntimeEntityMap.cs`
- Create: `PrimalEditor_Avalonia/Native/RuntimeAssetMap.cs`
- Create: `PrimalEditor_Avalonia/Native/RuntimeMaterialMap.cs`
- Modify: `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs`
- Create: `PrimalEditor_Avalonia.Tests/Native/RuntimeMapTests.cs`

**Steps:**

1. Write tests for register, replace, lookup, removal, whole-session reset and duplicate registration.
2. Key Entity map by `EntityGuid`, Asset map by `AssetGuid`, and Material map by Material Asset GUID.
3. Replace `Dictionary<GameEntity,uint>` with the typed Entity GUID map.
4. Ensure maps have no JSON attributes and are owned by the open runtime session, not Project/Scene models.
5. Reset mappings on Scene unload, project close and engine shutdown.

**Commit:** `refactor(runtime): key process-local engine mappings by stable GUID`

### Task 19: Create a Scene runtime synchronizer

**Files:**
- Create: `PrimalEditor_Avalonia/Native/SceneRuntimeSynchronizer.cs`
- Modify: `PrimalEditor_Avalonia/Native/EngineSync.cs`
- Modify: `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs`
- Modify: `PrimalEditor_Avalonia/Native/NativeEngineBridge.cs`
- Create: `PrimalEditor_Avalonia.Tests/Native/SceneRuntimeSynchronizerTests.cs`

**Steps:**

1. Introduce an injectable `INativeEngineBridge` façade so tests do not require a loaded dylib.
2. Write an order test: create all Core entities, register Entity GUID mappings, resolve/load cooked assets, add MeshRenderer/Light/Camera, apply Transform, then hierarchy/enabled state.
3. Write rollback tests: if a required cooked artifact is unavailable, keep source Scene loaded, mark the component unresolved, and clean up already-created runtime objects.
4. Move direct import/polling logic out of `WorldEditorViewModel.DropAssetAsync` and `LoadEntitiesAsync`.
5. Reuse existing C ABI operations where possible. Add explicit create/update/destroy functions only when the façade test identifies missing lifecycle support.

**Commit:** `feat(runtime): synchronize source scenes into core ECS by GUID`

### Task 20: Harden EngineDLL C ABI and ownership

**Files:**
- Create: `EngineDLL/EditorRuntimeAPI.h`
- Create: `EngineDLL/EditorRuntimeAPI.cpp`
- Modify: `EngineDLL/CMakeLists.txt`
- Modify: `PrimalEditor_Avalonia/Native/NativeEngineBridge.cs`
- Create: `EngineTest/IntegrationTests/TestEditorRuntimeAPI.cpp`
- Modify: `EngineTest/CMakeLists.txt` or the nearest test target file

**Steps:**

1. Write C++ integration tests for create/update/destroy Entity, MeshRenderer, Light, Camera and cooked asset registration.
2. Define fixed-width C structs, explicit ABI version query, result codes and caller-owned error buffers.
3. Never pass C++ object layouts, STL objects or editor GUID semantics into core persistence. GUIDs may be accepted only as opaque correlation fields for diagnostics; EngineDLL returns runtime IDs.
4. Specify ownership for every returned ID and make shutdown release all runtime assets/entities.
5. Add C# struct-size and ABI-version tests before invoking APIs.

**Commit:** `feat(enginedll): add versioned editor runtime C ABI`

---

## Phase 8: Complete project-open, scene-open and save flows

### Task 21: Implement project-open orchestration

**Files:**
- Create: `PrimalEditor_Avalonia/GameProject/ProjectOpenService.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/OpenProject.cs`
- Modify: `PrimalEditor_Avalonia/Content/AssetRegistry.cs`
- Modify: `PrimalEditor_Avalonia/Content/ContentWatcher.cs`
- Create: `PrimalEditor_Avalonia.Tests/Integration/ProjectOpenFlowTests.cs`

**Steps:**

1. Write the integration test for: read Project, migrate if needed, scan Scenes/Content, ensure/read meta, validate/rebuild DB, calculate dirty cook set, start watcher, then load Default Scene.
2. Ensure editor-only mode works when EngineDLL is unavailable: source and DB operations still succeed; runtime sync is deferred.
3. Make open cancellation dispose DB connections, watcher and partial runtime state.
4. Verify deleting `.Primal/AssetDatabase.sqlite` before open yields the same assets and GUIDs.

**Commit:** `feat(project): orchestrate migration indexing cooking and scene open`

### Task 22: Implement scene switching and unload

**Files:**
- Create: `PrimalEditor_Avalonia/GameProject/SceneOpenService.cs`
- Modify: `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs`
- Modify: `PrimalEditor_Avalonia/Native/SceneRuntimeSynchronizer.cs`
- Create: `PrimalEditor_Avalonia.Tests/Integration/SceneSwitchingTests.cs`

**Steps:**

1. Write tests that load Scene A, switch to B, and assert A's runtime entities/materials/assets are released and maps contain only B.
2. Handle dirty Scene confirmation independently from Project manifest dirtiness.
3. Load source Scene first and runtime objects second; a runtime failure must not corrupt source data.
4. Cancel obsolete cook/runtime tasks when switching Scenes.

**Commit:** `feat(scene): add deterministic scene open switch and unload lifecycle`

### Task 23: Finish atomic save and autosave behavior

**Files:**
- Modify: `PrimalEditor_Avalonia/Common/EditorSaveService.cs`
- Modify: `PrimalEditor_Avalonia/Common/AutoSaveService.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/Serializer.cs`
- Create: `PrimalEditor_Avalonia.Tests/Integration/SaveRecoveryTests.cs`

**Steps:**

1. Write tests for interrupted Scene save, interrupted Project save, stale `.partial`, and SaveAll ordering.
2. Ensure autosave never writes SQLite/runtime state into source files.
3. Store recoverable autosave snapshots under `.Primal/Temp/Autosave` without making them authoritative.
4. On successful manual save, clear dirty flags only after file replacement and re-read validation.

**Commit:** `feat(save): make project and scene saves atomic and recoverable`

---

## Phase 9: Build Manifest and cache lifecycle

### Task 24: Add release/build manifest generation

**Files:**
- Create: `PrimalEditor_Avalonia/Build/BuildManifest.cs`
- Create: `PrimalEditor_Avalonia/Build/BuildManifestGenerator.cs`
- Create: `PrimalEditor_Avalonia.Tests/Build/BuildManifestTests.cs`

**Steps:**

1. Write tests that start from selected Scene GUIDs and traverse the dependency graph to collect required artifacts.
2. Emit GUID, artifact-relative path, artifact hash, type, target platform and configuration.
3. Exclude source absolute paths, SQLite paths and all runtime IDs.
4. Fail the build for missing or failed required artifacts; ignore unrelated dirty assets.
5. Sort output deterministically for reproducible builds.

**Commit:** `feat(build): generate deterministic cooked asset manifest`

### Task 25: Add cache validation and garbage collection

**Files:**
- Create: `PrimalEditor_Avalonia/Content/Cooking/CookCacheMaintenance.cs`
- Create: `PrimalEditor_Avalonia.Tests/Content/CookCacheMaintenanceTests.cs`

**Steps:**

1. Write tests for orphan artifact removal, current artifact preservation, stale partial cleanup and per-platform isolation.
2. Never infer source deletion from cache deletion.
3. Run maintenance only after a successful DB rebuild or explicit user action.
4. Support a full `Rebuild Derived Data` action that closes DB, deletes `.Primal/AssetDatabase.sqlite` and `.Primal/Cache`, then reopens/rebuilds.

**Commit:** `feat(cook): validate clean and rebuild derived data safely`

---

## Phase 10: Legacy cleanup and final regression gates

### Task 26: Remove legacy authorities and casing conflicts

**Files:**
- Modify: `PrimalEditor_Avalonia/Content/AssetRegistry.cs`
- Modify: `PrimalEditor_Avalonia/Components/GameEntity.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/Project.cs`
- Modify: `PrimalEditor_Avalonia/GameProject/NewProject.cs`
- Modify: `PrimalEditor_Avalonia/Native/AssetImporter.cs`
- Modify: `.gitignore`
- Create: `PrimalEditor_Avalonia.Tests/Architecture/LegacyPersistenceTests.cs`

**Steps:**

1. Remove normal-path reads/writes of embedded Scenes, `AssetPath`, `.primal/asset-guids.json`, binary meta writes and sibling `.mesh` output.
2. Retain legacy input support only inside explicit migration classes.
3. Add tests that grep serialized fixtures and temporary projects for forbidden legacy fields/directories.
4. Ignore `.Primal/AssetDatabase.sqlite`, `.Primal/Cache`, thumbnails, logs and temp; do not ignore source `.meta`, `.scene`, `.material` or `.shader` files.

**Commit:** `chore(asset): retire legacy persistence and unify derived data paths`

### Task 27: Run full verification and deletion/rebuild proof

**Files:**
- Create: `PrimalEditor_Avalonia.Tests/Integration/FourLayerArchitectureTests.cs`
- Create: `EngineTest/IntegrationTests/TestCookedAssetRuntime.cpp`

**Steps:**

1. Create a temporary project with two Scenes, MeshRenderer, Light, Camera, Material, Shader and Texture/Model sources.
2. Save and close it.
3. Reopen and capture normalized Project/Scene/source asset state.
4. Delete `.Primal/AssetDatabase.sqlite` and `.Primal/Cache`.
5. Reopen, rebuild and assert identical stable GUID graph and source documents.
6. If EngineDLL is available, assert new runtime IDs are valid but need not equal IDs from the first run.
7. Run:

```bash
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
dotnet test PrimalEditor_Avalonia.Tests/PrimalEditor_Avalonia.Tests.csproj -c DebugEditor
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target EngineDLL EngineTest -j
ctest --test-dir build --output-on-failure
```

8. Verify boundaries:

```bash
! grep -R "Microsoft.Data.Sqlite" Engine EngineDLL
! grep -R '"\(EntityId\|ContentId\|RhiHandle\|RuntimeMaterialId\)"' PrimalEditor_Avalonia.Tests/Fixtures
```

9. Manually verify moving/renaming an asset with its `.meta` preserves all Scene/Material references.
10. Manually verify the project remains editable in editor-only mode with EngineDLL unavailable.

**Commit:** `test(asset): prove four-layer rebuild and runtime isolation end to end`

---

## Dependency and rollout order

```text
GUID/path primitives
  -> JSON meta authority
  -> Project/Scene v2 + migration
  -> MeshRenderer/Light/Camera GUID components
  -> Material/Shader source descriptors
  -> SQLite schema + rebuild + dependencies
  -> Cook cache and import jobs
  -> Runtime maps and Scene synchronizer
  -> Project/Scene open-save orchestration
  -> Build manifest and cache maintenance
  -> Legacy removal and end-to-end regression
```

Do not implement SQLite before Project/Scene/meta/GUID contracts pass their tests. Do not remove legacy readers until migration has been exercised against real project backups. Do not make Scene opening depend on a working GPU/runtime; source editing and migration must remain available in editor-only mode.

## Acceptance criteria

1. `.primal` has no embedded Scene or Entity payloads.
2. `.scene` round-trips all supported components and stores only stable GUID references.
3. Moving or renaming a source resource with its `.meta` does not break references.
4. Every asset GUID lives in `.meta`; SQLite mirrors it and can be regenerated.
5. No cook artifact is written beside source assets.
6. Cache artifacts are isolated by platform, configuration, inputs and toolchain version.
7. Deleting all of `.Primal/` and reopening reproduces the same source GUID graph and recooks required artifacts.
8. Runtime Entity, Content, Material and RHI IDs are recreated each session and never persisted.
9. Legacy v1 projects migrate transactionally, preserve semantic scene content and retain backups indefinitely.
10. Editor-only mode can open, migrate, edit and save Projects without EngineDLL.
11. The full .NET and C++ verification commands pass, or unrelated pre-existing failures are documented with evidence.
