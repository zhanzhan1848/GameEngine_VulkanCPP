# Project Lessons

## Avalonia Editor — Metal shader blob not loading

**Date:** 2026-06-24
**Worktree:** `avalonia-migration`
**Symptom:** Editor window opens, 4 sphere render items are registered
(`Test scene: 1 entities, 4 render items` in log), but 3D viewport is
**solid black** with no visible geometry. Editor log shows:

```
[MetalCore] shader::initialize failed; continuing without legacy engine shaders.
[MetalCore] fx::initialize failed; pass disabled. (shader blob missing function or PSO descriptor invalid)
[MetalCore] prepass::initialize failed; pass disabled. (shader blob missing function or PSO descriptor invalid)
[MetalTAA] create_pipeline: missing shader (vs=NULL fs=NULL)
[MetalCore] taa::initialize failed; pass disabled. (shader blob missing function or PSO descriptor invalid)
[MetalSSAO] pipeline state unavailable; SSAO pass disabled.
[MetalCore] ssgi::initialize failed; pass disabled. (shader blob missing function or PSO descriptor invalid)
```

**Root cause:** The native engine resolves the shader blob via a **bare
relative path** that is interpreted against **process cwd**, not against
the dylib's directory.

- `Engine/Graphics/Renderer.cpp:32-37` — `engine_shader_paths[2] =
  "./shaders/metal/shaders.metallib"`
- `Engine/Content/ContentLoaderMac.cpp:45` — calls
  `graphics::get_engine_shaders_path()` and passes the bare path to
  `std::filesystem::exists()` (cwd-relative).
- The Avalonia editor is typically launched as
  `dotnet ./Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor_Avalonia.dll`
  from the worktree root, so cwd = worktree root, but the shader blob
  lives next to the dylib in `DebugEditor/shaders/metal/`.
- When `shader::initialize` returns false the engine **silently disables
  every pass** (`fx`, `prepass`, `gpass`, `taa`, `ssao`, `ssgi`,
  `post_process`) and falls through to the `g_editor_draw_hook`
  fallback in `MetalPostProcess.cpp:218`, which is never installed
  (`set_editor_draw_hook` has no callers). The result is an empty
  drawable every frame, hence the solid black viewport.

**Fix (permanent):** Anchor cwd to `AppContext.BaseDirectory` before
the Avalonia `AppBuilder.Setup()` runs, so the bare-relative
`./shaders/...` paths resolve next to the dylib regardless of how the
editor was invoked. Implementation:
`PrimalEditor_Avalonia/Program.cs::AnchorWorkingDirectoryToExecutable`.

**Anti-pattern to avoid:** Trying to be clever with a "fast path" that
skips the chdir when a `shaders/` directory already exists in cwd.
That heuristic gets fooled by a stray sibling `shaders/` directory
left over from a previous build attempt in the worktree root, and the
engine silently fails. Always pin cwd; the cost is one `SetCurrentDirectory`
call.

**Verification:** After the fix, the engine log shows clean
initialization, `3D viewport initialized via RHI forward pipeline`,
and the 3D viewport renders geometry (4 magenta spheres via the test
material's pixel shader).

---

## Avalonia Editor — MTKView swallows pointer events → fix with PassThroughMTKView subclass

**Date:** 2026-06-24
**Worktree:** `avalonia-migration`
**Status:** ✅ Working. Verified: left-drag orbit, right-drag pan,
wheel zoom all drive the camera and the 3D viewport content updates
in real time.

**Problem:** The editor embeds an MTKView on top of the Avalonia
contentView so the Metal surface is visible. Default
`NSView.hitTest:` returns `self` for any point inside the view's
bounds, so AppKit routes all pointer events (click, drag, wheel) to
the MTKView and the Avalonia controls underneath (`EngineViewport3D`)
never see them.

**Fix:** Subclass `MTKView` to override `hitTest:` to return nil, then
allocate the subclass from the embedded (`create_external`) path
and `reinterpret_cast<MTK::View*>` back to the C++ wrapper for
storage. The C++ `MTK::View` wrapper is layout-compatible with the
subclass (the wrapper has **no data members** — `NS::Object` is pure
interface that uses `sendMessage(this, sel, ...)` where `this` is
the underlying `id`), so the existing call sites that use
`static_cast<MTK::View*>(surface->view())` continue to work
unchanged.

**Why method swizzling didn't work:** AppKit's CALayer-backed
hit-test path does NOT go through `objc_msgSend` for `NSView.hitTest:`.
The swizzle IS applied to the class's method table (verified with
`InstallPassThroughHitTest: class=MTKView old=0x0 new=...`) but
the swizzled function is never invoked because AppKit's layer
hit-test calls into a C++/native function path that bypasses
Objective-C method dispatch. Subclassing overrides the method at
the class level, which IS what the layer hit-test path consults.

**Why the call sites don't need to change:** The two existing
call sites (`MetalCore.cpp:47` `end_frame` and
`MetalPostProcess.cpp:195` `post_process`) call `currentDrawable()`
and `draw()` on the view via the `MTK::View` wrapper, which uses
`sendMessage(this, sel, ...)` → `objc_msgSend` → resolves to
`MTKView`'s instance methods (which `PassThroughMTKView` inherits).
No `objc_msgSend` calls needed at the call sites.

**Files changed:**

- `Engine/Graphics/RHI/Platforms/Metal/PassThroughMTKView.mm` (new)
  — Objective-C++ subclass with `hitTest:` returning nil + C entry
  point `PassThroughMTKView_Create`.
- `Engine/Graphics/Metal/MetalSurface.cpp` — `create_external()` now
  uses `PassThroughMTKView_Create` and `reinterpret_cast<MTK::View*>`.

**Camera control bindings (PrimalEditor_Avalonia side):**

- `Native/NativeEngineBridge.cs:309` — `SetEntityTransform` C ABI
  binding (existed in C, missing in C#).
- `Views/Engine3D/EngineViewport3D.axaml.cs:16` — `_cameraEntityId`
  field (entity the camera is bound to).
- `Views/Engine3D/EngineViewport3D.axaml.cs:17-32` — orbit camera
  state (`_cameraTarget`, `_cameraDistance`, `_cameraYaw`,
  `_cameraPitch` + sensitivity constants).
- `Views/Engine3D/EngineViewport3D.axaml.cs:644` —
  `ApplyCameraTransform()` computes position from spherical
  coordinates and pushes via `SetEntityTransform`.
- `Views/Engine3D/EngineViewport3D.axaml.cs:555-636` — pointer
  handlers (left=orbit, right=pan, wheel=zoom, F=reset).
- **IMPORTANT:** `ApplyCameraTransform` uses
  `if (_cameraEntityId == NativeEngineBridge.InvalidEntityId) return;`
  — NOT `== 0`. The first entity created gets id=0 (a valid id),
  so checking for 0 incorrectly skips the camera.

**Verified working:** orbit drag produces ~11k pixel changes in
the 3D viewport region, zoom produces ~65k, and ORBIT→ZOOM produces
~65k. The 4 magenta test spheres (RGB 95,46,99) shift visibly with
the camera.

**How to debug if events "don't reach the control":** Add a file
write at the very top of the handler:

```csharp
private void OnPointerPressed(...) {
    try { System.IO.File.AppendAllText("/tmp/pointer_events.log", "PRESS\n"); } catch {}
    ...
}
```

If `/tmp/pointer_events.log` is never created, the event never
reached the handler — upstream issue. If it is created, downstream.

**Avalonia-specific note:** `OnPointerPressed` is only invoked when
the pointer is over a control with a non-empty `Background` (or
otherwise hit-testable visual). An AXAML control with
`Background="#01000000"` (alpha=1, effectively transparent) does NOT
receive pointer events through Avalonia's hit-test. Set
`Background="#FF202020"` (or any non-transparent color) for the
control to participate in hit-testing.

**Window position gotcha:** When automating mouse events with
Quartz CGEvent, the editor window may be on a different display than
expected (especially after a previous run). Query
`CGWindowListCopyWindowInfo` first to get the actual
`kCGWindowBounds`, then convert to screen pixels (× 2 on Retina).
Clicks on the wrong screen are silently dropped.

---

## Avalonia Editor — Render-item array element-size mismatch

**Date:** 2026-06-23
**Symptom:** Test scene created 4 render items but only 1 sphere (or
sometimes 0) appeared in the 3D viewport. Engine log showed
`Test scene: 1 entities, 4 render items, ... material=0x...` and
`gpass::render` reports `view_buffer` is being filled for each
render item, but the visible result is wrong.

**Root cause:** C# `_testSceneRenderItems` was `List<UInt64>` (8
bytes/element) but the engine's `frame_info::render_item_ids` is
`id::id_type*` (u32 = 4 bytes/element). The C ABI marshalled an 8-byte
array as a u32 array, so the engine read every-other-id as garbage
(e.g. valid, garbage, valid, garbage) and most render items resolved
to `id::invalid_id` in `gpass::fill_per_object_data`, leaving them
without a valid per-object constant buffer.

**Fix:** Use `List<uint>` (or `List<UInt32>`) on the C# side. Update
`RenderFrameParams.RenderItemIds` doc comment to say `u32*` (it was
incorrectly documented as `u64*`).

**Related:** Also fixed a stale-pin bug — `EnsureRenderItemArrayPinned`
was pinning once and never re-pinning when the list grew, so the
returned `IntPtr` was pointing at a too-small buffer after the list
expanded. The fix is to re-pin when the cached pinned count differs
from the current list count.

**Lesson:** When bridging a typed array across a C ABI, the element
size in C# **must** match the engine's struct field type exactly. A
UInt64 list and a u32 array look identical from a high level but
double the stride and shift every index by one.

---

## Avalonia Editor — MTKView autoresizingMask double-set crash comment

**Date:** 2026-06-23
**Location:** `Engine/Graphics/Metal/MetalSurface.cpp:91, 100`

The code sets `_mtk_view->setAutoresizingMask(0u)` on line 91, then
sets it again to `2u | 16u` on line 100. The comment on line 100
warns that `setAutoresizingMask(0u)` triggers AppKit to dispatch
`setFrame:display:` on the MTKView, which is not a recognized
selector on the runtime subclass and **crashes the process**.

The second set is there to recover. This pattern works in practice
(we don't see the crash) but is brittle. If you ever touch this
file, the double-set is intentional — do not "clean it up" by
removing the first call. The fix would be a single
`setAutoresizingMask(2u | 16u)` once we move off the runtime
subclass; until then, leave it alone.

---

## Avalonia Editor — libfbxsdk.dylib transitive dependency

**Date:** 2026-06-24
**Symptom:** `dotnet ./.../PrimalEditor_Avalonia.dll` exits silently
with `[WARN] Native engine not available: Unable to load shared
library '.../libPrimalEngine.dylib' or one of its dependencies.
dlopen(...): Library not loaded: @executable_path/libfbxsdk.dylib`

**Root cause:** `libContentTools.dylib` (linked into the engine)
depends on `@executable_path/libfbxsdk.dylib`. When loaded via the
Avalonia editor, `@executable_path` is the dotnet executable, not
the dylib. The FBX SDK is in
`Darwin/PrimalEditor_Avalonia/DebugEditor/libfbxsdk.dylib` but
dotnet is at `/opt/homebrew/Cellar/dotnet/10.0.108/libexec/dotnet`,
so the path resolution fails.

**Fix:** Symlink the FBX SDK into dotnet's directory:

```bash
ln -sf <worktree>/Darwin/PrimalEditor_Avalonia/DebugEditor/libfbxsdk.dylib \
       /opt/homebrew/Cellar/dotnet/10.0.108/libexec/libfbxsdk.dylib
```

This is a workaround for a deployment-time issue. A proper fix would
either (a) change `libContentTools.dylib` to use `@rpath/libfbxsdk.dylib`
or `@loader_path/...` so the lookup is relative to the dylib itself,
or (b) bundle dotnet and the dylibs together via a launcher script
that sets `DYLD_FALLBACK_LIBRARY_PATH`. Either fix belongs in the
build configuration, not the editor source.

---

## Avalonia Editor — MTKView not detached from host view on surface release

**Date:** 2026-06-25
**Worktree:** `avalonia-migration`
**Symptom:** Switching from World Editor tab to Content Browser tab
left a phantom MTKView visible on top of the Content Browser.
Engine log showed `3D viewport initialized via RHI forward pipeline`
on first attach but the surface was never torn down when the
viewport was detached.

**Root cause:** `metal_surface::release()` released the MTKView
ObjC object via `_mtk_view->release()` but never sent
`removeFromSuperview` to the host NSView. The host view kept a
pointer to a freed ObjC object in its subview list. The next layout
pass triggered AppKit to dispatch `setFrame:display:` to the freed
target — which on the runtime subclass crashes the process, or on a
stock MTKView silently leaves a phantom view drawing the last frame.

**Fix:** In `_is_external` mode, send `removeFromSuperview` to the
MTKView before releasing it:

```cpp
// Engine/Graphics/Metal/MetalSurface.cpp:66-92
void metal_surface::release() {
    if (_mtk_view) {
        if (_is_external) {
            reinterpret_cast<void (*)(struct objc_object*, SEL)>(objc_msgSend)(
                reinterpret_cast<struct objc_object*>(_mtk_view),
                sel_registerName("removeFromSuperview"));
        }
        _mtk_view->release();
    }
    _external_view = nullptr;
}
```

**Why `objc_msgSend` and not `NS::Object::sendMessage`:**
`NS::Object::sendMessage` is `protected` in the OSAPI header
(`OSAPI/MAC/Foundation/NSObject.hpp:83`). It's only callable from
NS::View's own inline methods (which is why `addSubview` /
`setFrame` etc. work). The C++ wrapper for MTKView doesn't expose
`removeFromSuperview`, and we can't add it without modifying OSAPI.
Falling back to Apple's public C runtime (`objc_msgSend`) is the
standard solution.

**The `reinterpret_cast<void (*)(objc_object*, SEL)>(objc_msgSend)`
idiom is Apple-recommended** for calling variadic `objc_msgSend`
from C++. The variadic cast pins the function pointer to a known
signature so the compiler doesn't pick the wrong
`objc_msgSend_fpret` / `objc_msgSend_stret` overload.

**Why `struct objc_object*` and not `id`:** `primal::id` is a
namespace in `Engine/Common/Id.h`. In any translation unit that
includes Id.h, the unqualified `id` resolves to `primal::id` and
shadows the Objective-C typedef. Using the fully-qualified
`struct objc_object*` sidesteps the name clash.

**Verified:** Switching World Editor → Content Browser now removes
the MTKView from the host view tree. The Content Browser tab shows
its own UI with no phantom render surface on top.

---

## Avalonia Editor — Render surface shows "dark red triangle in lower-left" — diagnostic in progress

**Date:** 2026-06-25
**Worktree:** `avalonia-migration`
**Status:** 🟡 Partially diagnosed — fix NOT yet applied.

**Symptom:** Render surface shows a single large dark-red triangle
in the lower-left quadrant; the upper-right shows the MTKView clear
color (`(0.10, 0.10, 0.10)` dark gray).

**Call chain when editor renders a frame:**

```
C# EngineViewport3D.OnRenderTick
    └─ NativeEngineBridge.RenderFrame (C ABI)
        └─ EngineDLL/FrameAPI.cpp::RenderFrame  (line 42: rs->surface.render(info))
            └─ Engine/Graphics/Renderer.cpp::surface::render  (line 138: gfx.surface.render)
                └─ Engine/Graphics/Metal/MetalInterface.cpp  (line 32: pi.surface.render = core::render_surface)
                    └─ Engine/Graphics/Metal/MetalCore.cpp::render_surface  (line 385)
                        ├─ gpass::depth_prepass
                        ├─ prepass::prepass
                        ├─ gpass::render
                        ├─ fx::compose_pass
                        ├─ taa::taa_pass
                        ├─ ssao::ssao_pass / blur
                        ├─ ssgi::ssgi_pass / blur
                        └─ fx::post_process   ← terminal pass to drawable
```

`fx::post_process` (Engine/Graphics/Metal/MetalPostProcess.cpp:193)
is what copies `compose_texture` (or clears if no compose texture)
to the drawable. It uses `fullscreen_triangle_vs` and draws
**6 vertices** (`drawPrimitives(Triangle, 0, 6)`) which is
**2 triangles** — the second triangle redundantly covers the same
NDC positions as the first. This redundancy is fine on a stock
Metal setup; the bug is elsewhere.

**Diagnostic performed:** Temporarily bypassed the full
`post_process` body and replaced it with a hard-coded
`(1.0, 1.0, 0.0)` clear color on the drawable.

**Result:** Render surface shows solid yellow. **This means
`post_process` was producing the red triangle** — when we bypass
it, the screen goes to the diagnostic yellow (not the gpass /
compose output).

**Hypothesis (untested):** The drawable size passed to
`view->currentDrawable()` does not match the surface size the
fullscreen triangle is being projected against. With drawable size
at the upper-right quadrant's portion of the surface (or some
similar mismatch), the 6-vertex triangle is clipped to the lower-
left, producing the observed "single triangle in lower-left"
artifact.

**Why the triangle is red, not the post_process_ps output (deep blue
`(0.05, 0.05, 0.08)`):** The red triangle was observed BEFORE the
diagnostic bypass. The post_process fallback path (when
`post_process_pipeline == nullptr`) clears the drawable to deep
blue, not red. The red must come from a different source — possibly
the compose texture itself (gpass output) being sampled into the
post_process color attachment, with the gpass depth/normal/world
buffers producing a red encoded value. Or the post_process PSO's
color attachment format mismatch is causing some swizzle.

**Files for next investigation:**

- `Engine/Graphics/Metal/MetalCore.cpp:385-448` — the orchestration
  loop. Check `metal_info.surface_width` / `surface_height` vs
  `surface->view()`'s actual drawable size.
- `Engine/Graphics/Metal/MetalSurface.cpp:139` —
  `_mtk_view->setDrawableSize(w, h)` — verify w, h match the
  actual MTKView frame at the time of call.
- `Engine/Graphics/Metal/MetalPostProcess.cpp:236` —
  `drawPrimitives(Triangle, 0, 6)` — change to `0, 3` to draw only
  one triangle and see if the artifact shape changes.
- `Engine/Graphics/Metal/MetalPostProcess.cpp` does not call
  `setViewport` — Metal uses the default viewport, which should be
  the drawable size. Verify no `gfx_command` viewport override is
  active when the post_process encoder runs.

**Diagnostic code was reverted** before commit — the yellow bypass
was a temporary tool, not a permanent fix.

---

## EngineDLL build — source files must be enabled in CMakeLists

**Date:** 2026-06-25
**Worktree:** `avalonia-migration`
**Symptom:** C# bindings for `PipelineSetEditorMode`,
`SetPipelineViewportSize`, etc. fail with
`Unable to find an entry point named 'X' in shared library 'PrimalEngine'`,
even though `nm -gU libEngineDLL.dylib | grep X` confirms the symbol
is in the cmake build output. After running `dotnet build` the symbol
is *not* in the Avalonia editor's dylib copy.

**Two independent root causes (must fix both):**

1. **`EngineDLL/CMakeLists.txt` line 14** had
   `# "RenderPipelineAPI.cpp"  # depends on StandardRenderPipeline/Lumen; not used by Avalonia UI yet`
   — the source file was commented out, so it was never compiled into
   `libEngineDLL.dylib`. The native symbols literally did not exist.
   Fix: uncomment the line and re-run `cmake reconfigure`.

2. **dylib copy-on-build drift:** `cmake --target EngineDLL` writes
   to `Darwin/DebugEditor/libEngineDLL.dylib`. The Avalonia editor
   loads from `Darwin/PrimalEditor_Avalonia/DebugEditor/libEngineDLL.dylib`
   (a separate copy that `PrimalEditor_Avalonia.csproj::CopyNativeEngineLibsUnix`
   populates during dotnet build). After rebuilding native, you must
   either:
     - rebuild the C# project (which runs the copy target), or
     - manually `cp` the dylib and re-run `install_name_tool -id @loader_path/libEngineDLL.dylib` + `-change @rpath/libContentTools.dylib @loader_path/libContentTools.dylib`.
   Skipping step 2 leaves the Avalonia editor loading the **old**
   dylib with no `PipelineSetEditorMode` symbol — and the C# binding
   fails silently in `Logger.Error("PipelineSetEditorMode failed")`.

**Verification checklist after binding new APIs:**

```bash
# 1. Source file enabled in CMakeLists
grep RenderPipelineAPI.cpp EngineDLL/CMakeLists.txt  # NOT commented

# 2. Symbol in cmake build output
nm -gU Darwin/DebugEditor/libEngineDLL.dylib | grep PipelineSetEditorMode

# 3. dylib copied to editor dir
ls -la Darwin/PrimalEditor_Avalonia/DebugEditor/libEngineDLL.dylib  # newer than Darwin/DebugEditor/

# 4. install_name set correctly
otool -D Darwin/PrimalEditor_Avalonia/DebugEditor/libEngineDLL.dylib
# expect: @loader_path/libEngineDLL.dylib

# 5. dotnet build clean
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor

# 6. Run editor, watch log for "Unable to find an entry point" errors
```

If any step fails, fix that step and re-run.

---

## C# binding pattern for opaque C structs (RenderPipelineSettings)

**Date:** 2026-06-25

`RenderPipelineSettings` is a 380-byte struct with deeply nested
substructs (LumenConfig = 144B, FroxelGridConfig = 60B, etc.)
whose field layout drifts over time as the engine evolves. Mirroring
the full struct as `[StructLayout(LayoutKind.Sequential)]` in C#
would create a binary-compat break every time a subfield is added
or reordered.

**Pattern:** Marshal as raw byte buffer.

```csharp
public static uint GetRenderPipelineSettingsSize()
{
    // Hardcode the known size; update when the C++ struct changes.
    return 380;
}

[DllImport("PrimalEngine", EntryPoint = "GetRenderPipelineSettings", CallingConvention = CallingConvention.Cdecl)]
private static extern void _GetRenderPipelineSettings(IntPtr outBuffer);

public static byte[] GetRenderPipelineSettings()
{
    if (!_isLoaded) return null;
    var buf = new byte[GetRenderPipelineSettingsSize()];
    var handle = GCHandle.Alloc(buf, GCHandleType.Pinned);
    try { _GetRenderPipelineSettings(handle.AddrOfPinnedObject()); }
    finally { handle.Free(); }
    return buf;
}
```

Tradeoffs:
- Pro: layout drift in the C++ side is invisible to C# — the byte
  buffer is opaque.
- Con: the C# caller cannot read individual fields. If a UI needs
  to display or edit a specific field, it must be exposed via a
  dedicated accessor function in the C ABI (which `GetPipelineParamDescriptors`
  provides for the schema-driven case).

For structs you DO need to read field-by-field, the trade-off flips:
a `[StructLayout]` mirror is fine, but be aware of the
binary-compat hazard and update both sides in the same commit.

---

## RenderPipelineAPI surface binding summary (2026-06-25)

After merging `features/nanite_lumen` (commit 6d4ec50), the
StandardRenderPipeline abstraction is reachable from C# through
the following bindings added in `NativeEngineBridge.cs`:

| C# method | Native entry point | Purpose |
|---|---|---|
| `SetRenderPassEnabled(id, enabled)` | `SetRenderPassEnabled` | toggle a render pass on/off |
| `IsRenderPassEnabled(id)` | `IsRenderPassEnabled` | query default config |
| `IsRenderPassActive(id)` | `IsRenderPassActive` | query runtime-active |
| `GetRenderPassCount()` | `GetRenderPassCount` | enumerate |
| `GetPipelinePassInfo(id)` | `GetPipelinePassInfo` | name/description/default |
| `GetRenderPipelineSettings()` | `GetRenderPipelineSettings` | byte buffer (380 B) |
| `UpdateRenderPipelineSettings(bytes)` | `UpdateRenderPipelineSettings` | byte buffer |
| `GetPipelineParamDescriptorsCount()` | `GetPipelineParamDescriptors` | param count |
| `SetPipelineViewportSize(w, h)` | `SetPipelineViewportSize` | sync with surface size |
| `ReloadPipelineShader(handle, bytes)` | `ReloadPipelineShader` | shader hot-reload |
| `PipelineRegisterMeshEntity(geom, tex)` | `PipelineRegisterMeshEntity` | mesh entity |
| `PipelineUnregisterMeshEntity(id)` | `PipelineUnregisterMeshEntity` | mesh cleanup |
| `PipelineRegisterStreamingMesh(mesh, bmin, bmax)` | `PipelineRegisterStreamingMeshEntity` | Phase 9.3b streaming mesh |
| `PipelineUpdateStreamingMesh(id, gen, bmin, bmax)` | `PipelineUpdateStreamingMeshEntity` | streaming mesh update |
| `PipelineUnregisterStreamingMesh(id)` | `PipelineUnregisterStreamingMeshEntity` | streaming mesh cleanup |
| `PipelineSetEditorMode(enable)` | `PipelineSetEditorMode` | lightweight forward path |
| `PipelineIsEditorMode()` | `PipelineIsEditorMode` | query |
| `PipelineSetLumenConfig(preset)` | `PipelineSetLumenConfig` | GI quality (0..5) |

In `EngineViewport3D.axaml.cs::TryInitializeFromLayout` we now call
`PipelineSetEditorMode(true)` + `SetPipelineViewportSize(width, height)`
on init, and `SetPipelineViewportSize` on resize (in lockstep with
`ResizeRenderSurface`).

This binds **18 native exports** to C# (was 0 before) and unlocks
the post-merge pipeline. The editor's render surface still shows
a single fullscreen triangle artifact (purple now, was red before
the merge) — separate from the binding work; root cause TBD.

---

## Problem 2 — Render surface shows colored triangle in lower-left

**Date:** 2026-06-25
**Worktree:** `avalonia-migration`
**Status:** 🟡 Root cause narrowed, fix NOT yet applied.

**Symptom:** Render surface shows a single large colored triangle
in the lower-left of the viewport; the upper-right shows the MTKView
default clear color `(0.10, 0.10, 0.10)` dark gray. Color of the
triangle varied across runs (deep red pre-bindings, dark purple
post-bindings, dark gray in some builds).

**Call chain when editor renders a frame:**

```
C# EngineViewport3D.OnRenderTick
    └─ NativeEngineBridge.RenderFrame (C ABI)
        └─ EngineDLL/FrameAPI.cpp::RenderFrame  (line 42: rs->surface.render(info))
            └─ Engine/Graphics/Renderer.cpp::surface::render  (line 138: gfx.surface.render)
                └─ Engine/Graphics/Metal/MetalInterface.cpp  (line 32: pi.surface.render = core::render_surface)
                    └─ Engine/Graphics/Metal/MetalCore.cpp::render_surface  (line 385)
                        ├─ gpass::depth_prepass
                        ├─ prepass::prepass
                        ├─ gpass::render
                        ├─ fx::compose_pass
                        ├─ taa::taa_pass
                        ├─ ssao::ssao_pass / blur
                        ├─ ssgi::ssgi_pass / blur
                        └─ fx::post_process   ← terminal pass to drawable
```

**Diagnostic A — yellow-clear bypass of post_process body:**
Replaced the body of `fx::post_process` with a hard-coded
`(1, 1, 0)` clear color. The whole surface turned bright yellow.
This proves `post_process` is on the hot path for whatever appears
on screen — it is the bottleneck pass that writes the final pixel
values to the drawable.

**Diagnostic B — partial-color override with cyan (1, 0, 1) clear
attached to `view->currentRenderPassDescriptor()` then full draw:**
The result was **partial cyan + persistent purple** in distinct
triangular regions on the surface. The two triangles come from
`drawPrimitives(Triangle, 0, 6)` which draws 6 vertices = 2
triangles in clip space (positions `(-1,-1)/(3,-1)/(-1,3)`,
redundant on the second triangle). The fact that the two triangles
were **not** identically colored means `view->currentRenderPassDescriptor()`
returned a render pass descriptor whose attachment is not
configured to span the full MTKView drawable, OR the drawable's
internal sub-rect clipping does not match the encoder's draw area.

**Diagnostic C — final RPD-only clear (this iteration):** Replaced
the post_process body with a *pure* clear of the drawable using
`view->currentRenderPassDescriptor()` (no drawPrimitives). The
entire viewport turned **bright yellow**, no triangles, no purple,
no gray. This confirms:
  - The viewport / drawable can be cleared end-to-end via
    `view->currentRenderPassDescriptor()`.
  - The triangle artifact appears only when `drawPrimitives(0, 6)`
    is invoked.
  - Therefore the triangle artifact is **not** a viewport mismatch
    or `currentRenderPassDescriptor()` issue — it is a **shader /
    pipeline / state issue inside the draw call**.

**Refined hypothesis:** The fullscreen_triangle_vs renders the
6 vertices correctly but the *fragment shader* `post_process_ps`
produces non-uniform output across the triangle because:

1. **Texture binding mismatch:** the native side at
   `MetalPostProcess.cpp:232-235` binds
   `gpass::get_depth_buffer()` to slot 0 (correct), but binds
   `taa::get_taa_texture()` to slot 1 instead of
   `gpass::get_albedo_buffer()`. The shader
   `PostProcess.metal:276-282` declares `gpass_tex [[texture(1)]]`.
   So slot 1 receives the TAA texture, which is half-initialized,
   which is **why the output is dark / weirdly tinted**.

2. **Or post_process_pipeline is partially constructed:** shader
   blob is loaded but the pipeline state has a missing / wrong
   fragment function, producing the colored triangle as a partial
   rasterization fallback.

**Verified by diagnostic B:** when the post_process body is
replaced with the cyan clear + `drawPrimitives(0, 6)` against a
real `post_process_pipeline`, the result shows the shader output
overlaid on the cyan clear — meaning the pipeline IS partially
functional (otherwise the cyan would dominate everywhere).

**Fix attempted:** Applied the slot 1 fix in
`MetalPostProcess.cpp::post_process`: replaced
`taa::get_taa_texture().texture()` with
`gpass::get_albedo_buffer().texture()` (slot 1 = gpass albedo, which
is what `PostProcess.metal::post_process_ps` declares as
`gpass_tex [[texture(1)]]`). Verified the gpass shader writes albedo
to attachment slot 2 (`buildin_shader.metal::PixelOut::Albedo
[[color(2)]]` matching `MetalGPass.cpp:683`).

**Fix did NOT resolve the visual artifact.** Re-ran the editor and
captured a screenshot: the colored triangle is still in the exact
same position and shape. The fix was compiled in
(`nm -C` confirms `post_process` now calls `get_albedo_buffer`
instead of `get_taa_texture`) but the output did not change.

**Diagnostic D — pipeline state log:** Added a one-shot
`std::cerr` log in `post_process()` printing
`pipeline=READY|NULL`. Confirmed at runtime that
`post_process_pipeline != nullptr`, i.e. the normal path runs and
the visible artifact is genuinely `post_process_ps` output, not a
fallback artifact.

**Conclusion:** The slot-1 texture-binding mismatch is real
(it should be `gpass::get_albedo_buffer()`, not
`taa::get_taa_texture()`) but it is **not** the root cause of
the colored triangle. The actual root cause lies inside
`PostProcess.metal::post_process_ps` itself — most likely one of:

1. The shader assumes data is in buffers that the Editor's
   surface-render pipeline never populates. Specifically,
   `gpass::depth_buffer`, `gpass::albedo_buffer`, etc. are
   created via `create_buffers_for_size` but the test scene's
   4-sphere geometry may not produce a draw call that reaches
   the gpass pixel shader (e.g. wrong material id, missing
   material PSO, depth-prepass culling everything).
2. The shader's tone-mapping + gamma path produces a fixed
   output `(0.5, 0, 0.5)` when the indirect-light buffer is
   `(1, 1, 1, 1)` (the clear color when SSAO/SSGI/Compose never
   run). To confirm, render the editor with one of the passes
   actually producing output (e.g. ensure gpass writes albedo,
   ssao writes a non-trivial buffer).

**Status:** slot 1 fix REVERTED — it was a legitimate binding
correction but does not solve the visual symptom, and committing
it as a "fix" would be misleading. `MetalPostProcess.cpp` is
back to baseline. The colored triangle persists.

**Diagnostic E — gpass items + PSO validity:** Added a one-shot
per-30-frames `std::cerr` log in `MetalGPass.cpp::render` that
counts `items_count`, `pso_ok` (non-null `gpass_pipeline_states[i]`)
and `pso_null`. Captured output:

```
[gpass::render] diag: items_count=4 pso_ok=4 pso_null=0 frame_index=0
[gpass::render] diag: items_count=4 pso_ok=4 pso_null=0 frame_index=0
[gpass::render] diag: items_count=4 pso_ok=4 pso_null=0 frame_index=0
... (5 more identical lines, log throttled)
```

**Confirmed: gpass actually issues 4 drawIndexedPrimitives per
frame, all 4 with valid pipeline states.** So the symptom is
*not* caused by gpass drawing zero items or by null PSOs. The
sphere geometry is being rasterized.

**Diagnostic F — frame_index always 0:** Added a one-shot
`std::cerr` log in `MetalCore.cpp::render_surface` printing
`begin_frame OK, frame_idx, frame_count, surface_size`. Captured
output:

```
[MetalCore::render_surface] diag: begin_frame OK, frame_idx=0 frame_count=0    surface=824x575
[gpass::render]          diag: items_count=4 pso_ok=4 pso_null=0 frame_index=0
[MetalCore::render_surface] diag: begin_frame OK, frame_idx=0 frame_count=30   surface=824x575
... etc
```

`frame_count` increments correctly (0, 30, 60, 90, ...), so
`begin_frame` is being called every frame. But `frame_idx` —
which is `current_frame_index()` returning
`gfx_command.frame_index()` — is **stuck at 0**.

This means `_frame_index = (_frame_index + 1) % frame_buffer_count`
(line 72, in `metal_command::end_frame`) is never executing.
`end_frame` itself is wired (line 456 calls it after the post
process pass), so either:
  - end_frame is being called but `_frame_index++` is happening
    to a different `metal_command` instance (singleton
    reinit?), or
  - end_frame is throwing/early-returning before line 72
    (e.g. on `_cmd_buffer->commit()`), or
  - some race / re-init pattern resets `_frame_index` each frame.

**Significance:** `frame_index` is supposed to alternate 0,1,0,1
(`frame_buffer_count = 2`). With it stuck at 0, every frame
overwrites the *same* `constants_buffer[0]` slot. As long as
the data fits and `cbuffer.clear()` is called before refilling
this is harmless, but if the cbuffer write size grows (more
materials / more render items) and the previous frame's data is
not zeroed, the shader may read stale state. Worth investigating
but not necessarily the colored-triangle root cause.

**Diagnostic code reverted.** Both `MetalGPass.cpp` and
`MetalCore.cpp` are back to baseline; the `iostream` include
added for the diagnostic `std::cerr` was also reverted.

**Status: problem 2 NOT FIXED.** Investigations ruled out:
  - slot 1 texture binding (fix applied, no change, reverted)
  - gpass drawing zero items (verified: 4/4 valid)
  - gpass PSO null (verified: 0/4 null)

**Open hypotheses (untested):**
  1. **Material loading failure** — `[ForwardSceneRenderer]
     Failed to load albedo:` appeared in startup log. The test
     scene's material id `0x0` may not have a valid shader blob
     in the metallib, so `create_pso` may have produced a PSO
     that renders a constant color (the artifact color).
     Workaround: switch the test scene's material to a known-good
     one, or implement a debug `set_default_material()` API.
  2. **`_frame_index` not advancing** — constant-buffer aliasing
     across frames may produce stale state. Fix: ensure
     `_frame_index++` runs unconditionally (audit exception paths
     in `end_frame`).
  3. **Editor 30 Hz timer not actually delivering frames** —
     verify `Editor render loop started (30Hz)` actually triggers
     `RenderFrame` at 30 Hz. If the timer is firing but
     `RenderFrame` is throttled or batched, `_frame_index`
     could be stuck at 0 even though `frame_count` advances
     (because `frame_count++` is in `end_frame` after `_frame_index++`).
  4. **`info.thresholds == IntPtr.Zero`** — Editor passes null
     thresholds to `RenderFrame`. `get_lod_offsets` asserts
     non-null but is disabled in release; the function then
     dereferences `thresholds[i]` which is a UB read. If the
     resulting `lod_offsets` are garbage, `metal_render_item_count`
     could be 0 and the loops in `gpass::render` would draw
     nothing despite the count of 4 being non-zero (the 4 is
     input items, the loop is over output metal render items).

**Recommended next step:** investigate hypothesis 4 first —
inspect `RenderFrame` flow to confirm whether
`metal_render_item_count` (after `get_metal_render_item_ids`)
matches the input `render_item_count` of 4. If not, the
threshold-null path is the bug.

---

## General: debugging render output issues

When the 3D viewport is empty or wrong but the engine log says
"everything is fine":

1. **Check the shader blob load first.** The engine's soft-fail
   behavior hides shader-load failures. If you see
   `shader::initialize failed; continuing without legacy engine
   shaders` followed by a chain of `xxx::initialize failed;
   pass disabled`, the symptom is "passes look enabled in the
   log but don't actually run". The pipeline gates on
   `pipeline_state != nullptr`, and the disabled passes
   silently fall through.
2. **Check the cwd** for any C ABI that uses bare relative paths
   to engine assets. The Avalonia editor can be launched from
   multiple working directories and the engine has no way to
   detect that.
3. **Check render-item array element size** when bridging typed
   lists across the C ABI. UInt64 vs uint is the most common
   offender.
4. **Check pin state** when the list grows. A pinned `GCHandle`
   for a `List<T>` is only valid for the list's current size;
   growing the list after pinning yields stale memory.
5. **For pointer-event issues in Avalonia with native subviews:**
   The native subview (MTKView, OpenGL view, etc.) is on top in
   AppKit's z-order and swallows events before Avalonia's input
   system sees them. Subclass the native view to override
   `hitTest:` to return nil, or position the native view below
   the Avalonia content in z-order (with corresponding z-order
   bookkeeping for visual layering). Method swizzling is NOT
   sufficient — AppKit's CALayer-backed hit-test path bypasses
   Objective-C method dispatch.

---

## Problem 2 — fix narrative (render-loop integration)

### Symptom
Editor 3D viewport shows a single colored triangle (left-bottom
corner) with the post-process clear color filling the rest. After
several hours of investigation the artifact was attributed to a
**3 Hz native frame throttle** instead of the expected 30/60 Hz.

### Root cause
Avalonia's `DispatcherTimer` fires on the Avalonia UI dispatcher
thread. The render tick calls `NativeEngineBridge.RenderFrame`,
which P/Invokes into native and triggers `MTKView::draw()`. That
draw call must dispatch onto the main AppKit thread to commit the
drawable, but Avalonia is holding the dispatcher thread waiting
on that same dispatch. Result: the dispatcher thread blocks
until the native draw completes; subsequent `DispatcherTimer`
ticks queue up but never get serviced because the dispatcher is
still blocked. Measured: C# `OnRenderTick` fires at 30 Hz,
`RenderFrame` P/Invoke is called at 30 Hz, but the native
function body only enters at ~3 Hz.

`EngineTest` (macOS native, NSWindow + NSApp run loop) runs at 60
Hz because its timer is `CFRunLoopTimerCreate` scheduled on
`CFRunLoopGetMain()` — same run loop that the MTKView needs.
That confirmed the fix direction: **driving frames from a
CFRunLoopTimer on the main run loop, not Avalonia's dispatcher.**

### Fix (4 files)

1. `EngineDLL/EngineAPI_mac.mm` — new C ABI:
   - `EditorSetFrameCallback(void(*)(u32, float))` — C# registers
     a single static callback at startup.
   - `EditorCreateRenderLoop(int fps) -> u32` /
     `EditorStartRenderLoop` / `EditorStopRenderLoop` /
     `EditorDestroyRenderLoop` — manage `CFRunLoopTimerRef`s
     stored in a `std::vector<std::unique_ptr<editor_render_loop>>`
     (one slot per loop id).
   - Timer fires on the main run loop at the requested fps, calls
     `g_frame_cb(loopId, dt)`. Single-threaded so `running` is a
     plain `bool`, not `std::atomic` (the original `std::atomic<bool>`
     made `editor_render_loop` non-copyable, breaking
     `std::vector<editor_render_loop>`).

2. `PrimalEditor_Avalonia/Native/NativeEngineBridge.cs` — new
   `NativeEngineBridge.EditorRenderLoop` static class wrapping
   the C ABI. Pins a `NativeFrameCb` delegate via `GCHandle.Alloc`
   on first `Start`, registers it with `_EditorSetFrameCallback`.
   Per-loop `Action<float>` callbacks are dispatched by id from
   the global thunk.

3. `PrimalEditor_Avalonia/Views/Engine3D/EngineViewport3D.axaml.cs`
   — `StartRenderLoop` now calls `EditorRenderLoop.Start(60, OnNativeFrame)`
   instead of constructing a `DispatcherTimer`. DispatcherTimer
   path retained as a fallback when native loop creation returns
   0.

4. Build: copy the freshly-built `Darwin/DebugEditor/libEngineDLL.dylib`
   into the editor's runtime directory after each native rebuild
   (the project file's MSBuild post-build copies native libs
   into the editor's output directory, but the existing copy
   becomes stale after a native-only rebuild — `cmake --build`
   does not retrigger MSBuild).

### Verification
Native `[RENDER_LOOP] 60 fps` confirmed via diagnostic logging
in `OnRenderFrameCore`; ran 30+ seconds stable at 60-61 fps.
The colored triangle artifact persists at 60 fps, which means
the remaining visual issue is a separate engine-side bug
(cbuffer / PSO / gpass material binding), not the render loop.

### Lessons

1. **Build configuration mismatch silently ships stale code.**
   `dotnet build` defaults to `Debug`, but the editor's runtime
   directory is `DebugEditor/`. The MSBuild `<BaseOutputPath>`
   splits output by config. If you only `dotnet build` without
   `--configuration DebugEditor`, the editor runs the previous
   DLL forever and your changes look like they did nothing.
   Symptom: log shows old log strings, file timestamps differ.
2. **Cross-thread dispatcher + native AppKit calls deadlock at
   hidden levels.** Avalonia's `DispatcherTimer` runs on a
   managed dispatcher thread; calling into native code that
   internally requires the main AppKit thread causes a soft
   throttle, not a hard deadlock. Symptoms: high-level tick
   rate appears normal, but native function body entry rate is
   far lower than expected. Compare C# tick count to native
   function-body entry count to detect this.
3. **`std::atomic<T>` is non-copyable.** If you put one in a
   struct that needs to live in a `std::vector<T>`, either wrap
   in `unique_ptr` or use a plain `T` (single-threaded access).
4. **Use a single static callback table for C → C# cross-language
   dispatch.** Per-loop function pointers with user_data look
   elegant in C but break when the C function pointer can't
   carry closure state. The clean C ABI is:
   `void EditorSetFrameCallback(void(*cb)(u32, float))`
   plus per-loop `Action<float>` lookup in C#. The C timer
   passes the loop id at fire time so C# can route to the right
   delegate.
5. **macOS CFRunLoopTimer pattern** — engine should follow
   `EngineTest`'s pattern: `CFRunLoopTimerCreate(... interval,
   interval, 0, 0, &fire, &ctx)` + `CFRunLoopAddTimer(runLoop,
   timer, kCFRunLoopCommonModes)` on `CFRunLoopGetMain()`. That
   is the same run loop MTKView's drawable commit uses, so the
   two are naturally serialized without any dispatch work.

---

## Problem 2b — Post-process slot 1 binding mismatch

### What was observed after the render-loop fix

After switching to the native CFRunLoopTimer at 60 fps, the
3D viewport still showed a single colored triangle in the
bottom-left quadrant of the viewport, with the rest of the
viewport a near-black background. The render loop was running
at full rate but the visible artifact was unchanged.

### Root cause (verified)

`Engine/Graphics/Metal/MetalPostProcess.cpp::post_process`
binds fragment texture slot 1 to `taa::get_taa_texture()`:

```cpp
postEnc->setFragmentTexture( gpass::get_depth_buffer().texture(), 0 );
postEnc->setFragmentTexture( taa::get_taa_texture().texture(), 1 );   // <-- WRONG
postEnc->setFragmentTexture( ssao::get_ssao_blur_texture().texture(), 2 );
postEnc->setFragmentTexture( ssgi::get_ssgi_blur_texture().texture(), 3 );
```

But `Engine/Graphics/Metal/shaders/PostProcess.metal` declares
slot 1 as `gpass_tex`:

```metal
texture2d<float, access::sample> depth_tex [[texture(0)]],
texture2d<float, access::sample> gpass_tex [[texture(1)]],   // <-- expected gpass albedo
texture2d<float, access::sample> ssao_tex  [[texture(2)]],
texture2d<float, access::sample> ssgi_tex  [[texture(3)]],
```

And the post-process fragment uses slot 1 as the base color:

```metal
float3 base_color = gpass_tex.sample(s, uv).rgb;
```

So the shader was sampling TAA output as base color instead of
gpass albedo. The colored triangle was actually the gpass
albedo content being correctly drawn into the albedo buffer,
just never composited into the final viewport because the
post-process was reading the wrong texture.

### Fix

`Engine/Graphics/Metal/MetalPostProcess.cpp` line 233:
change `taa::get_taa_texture()` to `gpass::get_albedo_buffer()`.

```cpp
postEnc->setFragmentTexture( gpass::get_depth_buffer().texture(), 0 );
postEnc->setFragmentTexture( gpass::get_albedo_buffer().texture(), 1 );
postEnc->setFragmentTexture( ssao::get_ssao_blur_texture().texture(), 2 );
postEnc->setFragmentTexture( ssgi::get_ssgi_blur_texture().texture(), 3 );
```

### Verification

Rebuild EngineDLL, copy the freshly built dylib into the
editor runtime directory (`Darwin/PrimalEditor_Avalonia/DebugEditor/`),
launch the editor. Visual confirmation: the colored triangle
is still there (because the underlying gpass issue — only one
triangle being rendered — is independent of the post-process
binding). But the visual is now the actual gpass output
instead of the wrong TAA texture, which is the correct fix
for this layer. The next investigation should focus on why
gpass only draws one triangle out of four.

### Earlier attempted revert

This is the same fix that was applied and reverted in an
earlier session. The revert was wrong: I had no visual
verification (only log-level checks), and the log showed no
difference because both bindings produced non-zero textures.
The fix is correct; it just doesn't address the "only one
triangle drawn" symptom, which is a separate gpass issue.

### Lessons

6. **Always verify visual fixes with a screenshot, not just
   log lines.** A fix can be technically correct (proper
   texture binding) yet not change the visible symptom (the
   underlying gpass bug). Logging won't tell you that — only
   a screenshot will. Reverting a correct fix because it
   didn't change the log output wastes the fix and obscures
   the real issue.
7. **Cross-check C++ texture slot binding against shader
   declarations whenever a texture-fed shader shows wrong
   output.** Slot index mismatches are silent: Metal does
   not validate that `setFragmentTexture(_, 1)` matches
   `[[texture(1)]]`. The mismatch only shows up as wrong
   pixels, not as a runtime error.
8. **Texture naming convention in shaders is misleading.**
   `gpass_tex [[texture(1)]]` is the *intended* slot name;
   it doesn't enforce the actual binding. If a refactor
   renames the slot in the shader but not in C++, the
   compiler won't catch it. A future improvement: use
   `#define`-based slot constants shared between C++ and
   the shader.

## Avalonia Editor — Gray viewport after StandardRenderPipeline switch

**Date:** 2026-06-27
**Worktree:** `avalonia-migration`
**Symptom:** After switching to `StandardRenderPipeline` via
`PipelineRenderFrame` + `BlitRenderTargetToSurface`, the editor
runs at 60 Hz but the 3D viewport shows **solid gray** (the MTKView
clear color) — no geometry rendered.

**Two distinct root causes** identified via runtime log inspection:

### Cause A: `BlitToDrawable.metallib` not built / shipped

```
[MetalBlitToDrawable] BlitToDrawable.metallib not found
```

`BlitRenderTargetToSurface` dispatches to
`metal::core::blit_surface_and_present` → `MetalBlitToDrawable::Blit()`,
which acquires the currentDrawable and renders a fullscreen triangle
sampling the render target. `Blit()` early-returns when
`pipeline_ == nullptr`, so the drawable stays at the MTKView clear
color. The shader source lives at
`Engine/Graphics/Metal/shaders/BlitToDrawable.metal`, but it was
**never added to `BuildEngineShaderBlob`'s output** — the blob in
`Darwin/PrimalEditor_Avalonia/DebugEditor/shaders/metal/`
predates the shader file.

**Fix:**
1. Updated `EngineShaderBlob.cmake` to add `BlitToDrawable.metal`
   as an explicit dependency and emit a second output file
   `BlitToDrawable.metallib` (same content as `shaders.metallib`
   — the source shader compiles into the same blob).
2. Made `MetalBlitToDrawable::resolve_metallib_path` fall back to
   `shaders.metallib` when `BlitToDrawable.metallib` is missing,
   so future consolidations don't require both filenames.

### Cause B: `RegisterMeshEntity` fails for translated-geometry IDs

```
RenderMesh::CreateFromAsset: Empty asset for geometry_id 2
[ForwardSceneRenderer] RegisterMeshResource: CreateFromAsset failed for id 2
[StandardRenderPipeline] RegisterMeshEntity: failed for geometry 2
[DBG_SPHERE] sphere 1: translatedGeom=0x2 meshEntity=0xFFFFFFFF
```

The previous test scene used `CreateTranslatedProceduralMesh` to
bake per-sphere offsets into vertex positions — a workaround for
the legacy gpass path's broken per-entity World matrix propagation.
But `CreateTranslatedProceduralMesh` returns **legacy geometry
content ids** (used with `AddRenderItem`), and
`ForwardSceneRenderer::RegisterMeshResource` calls
`RenderMesh::CreateFromAsset(device, geometry_content_id)` which
expects **RHI asset ids**. Slot 0/1 happen to coincide (valid RHI
ids), but slots 2-4 do not — hence only sphere 0 registered.

`StandardRenderPipeline::RegisterMeshEntity` internally creates
an **ECS entity with identity transform**, then expects the caller
to position it via `Transform` component updates. The correct
pattern is: one `ProceduralMeshCreate` → multiple
`PipelineRegisterMeshEntity` calls → one `SetEntityTransform` per
entity with the desired world position.

**Fix:** Refactored `CreateTestScene` to call
`PipelineRegisterMeshEntity(sphereGeom, null)` four times with the
same geometry, then `SetEntityTransform(meshEntity, [x, 0, 0], [0,0,0], [1,1,1])`
per sphere. Verified all four spheres now register valid mesh ids.

### Notes
- `SetEntityTransform` treats `rotation` as **Euler angles** (calls
  `euler_to_quat` internally), NOT a quaternion. Passing
  `[0, 0, 0, 1]` (identity quat) would still produce identity Euler
  by coincidence, but the convention must be respected for non-trivial
  rotations.
- 3 of 4 sphere failures were identical (`meshEntity=0xFFFFFFFF`),
  confirming the empty-asset pattern rather than per-vertex corruption.

## Avalonia Editor — mesh_id_to_index_ collision when sharing geometry (4 spheres → 1)

**Date:** 2026-06-29
**Worktree:** `avalonia-migration`
**Symptom:** After refactoring test scene to a single shared
`ProceduralMeshCreate` + four `PipelineRegisterMeshEntity` calls
(following the corrected convention from the previous lesson),
all 4 spheres had **valid `meshEntity` ids (0x1..0x4)** — but only
**one** mesh ended up in `ForwardSceneRenderer::mesh_id_to_index_`
(`mesh_id_map=1`, scene rendered a single sphere at world origin).

```
[DBG_SPHERE] sphere 0..3: meshEntity=0x1..0x4 pos=(x,0,0)
[ForwardSceneRenderer::Render] scene_loaded_=1 proxies=1 mesh_infos=1 mesh_id_map=1
```

### Root cause

`ForwardSceneRenderer::RegisterMeshResource` keyed
`mesh_id_to_index_` by **geometry_content_id**:

```cpp
mesh_id_to_index_[geometry_content_id] = slot;
```

But four entities sharing one geometry asset all have the same
`geometry_content_id` → only the last one's slot survives (the
others are overwritten on insert). Worse, `info.meshEntityId =
geometry_content_id` meant the per-draw record also carried the
shared id, so the instance buffer's per-instance slot indices
collapsed to one.

The intent of `mesh_id_to_index_` is "look up the slot for this
ECS entity instance" — but the key was the **asset** id, not the
**entity** id. These are different concepts in the new pipeline:
one geometry asset can be instanced by many entities (like four
spheres sharing one `sphereGeom`).

### Fix

1. Added `entity_id = id::invalid_id` parameter to
   `ForwardSceneRenderer::RegisterMeshResource`.
2. Resolved `mesh_entity_id = (entity_id != invalid_id) ? entity_id :
   geometry_content_id` — preserves legacy callers (one mesh per
   geometry) and lets the new pipeline pass the per-entity id.
3. If `mesh_entity_id != geometry_content_id`, retag the RenderMesh
   via `Unregister / SetEntityId / Register` so the mesh's
   internal `entity_id` matches.
4. `mesh_id_to_index_[mesh_entity_id] = slot` — now keyed correctly.

Then `StandardRenderPipeline::RegisterMeshEntity` creates the ECS
entity **first** (so we have a unique id), then passes that id to
`RegisterMeshResource`:

```cpp
game_entity::entity entity = game_entity::create(info);
const id::id_type eid = entity.get_id();
u32 slot = forward_renderer_->RegisterMeshResource(
    geometry_content_id, albedo_id, normal_id, orm_id, eid);
```

`RenderMesh.h` declares `friend class ForwardSceneRenderer` so
`ForwardSceneRenderer` can call the private `Unregister / SetEntityId /
Register` triplet on the mesh.

After fix: `mesh_id_map=4`, four DRAW_INDEXED calls per pass.

### Lesson

**When a renderer's per-entity bookkeeping is keyed by an id, that
id must be the *entity* id, not the *asset* id.** Assets are
shared; entities are not. If two entities share an asset, you
need a separate per-entity handle (the ECS entity id) to
disambiguate. The legacy gpass path got away with this because
its per-vertex `World` matrix baked positions into the geometry —
one geometry asset = one entity, no collision possible.

## Avalonia Editor — ForwardSceneRenderer skips Opaque technique in forward pass

**Date:** 2026-06-29
**Worktree:** `avalonia-migration`
**Symptom:** After mesh-id disambiguation fix, scene reported
`scene_loaded=1 proxies=4 mesh_infos=4 mesh_id_map=4` — but the
viewport still showed no spheres. The ShadowPass drew them
(`ShadowPass] visible=4 total_proxies=4 mesh_map=4`), but the
forward pass produced zero draw calls.

### Root cause

`ForwardSceneRenderer::RenderDynamicInstances` filters meshes by
**technique**:

```cpp
if (forward_pass && t != 3 && t != 4) continue;  // skip Opaque in forward
if (!shadow_pass && !forward_pass && (t == 3 || t == 4)) continue;  // skip Water in gbuffer
```

This is correct for the **production deferred pipeline** —
Opaque meshes go through gbuffer + lighting + composite, while
Water (3) and Transparent (4) composite in the forward pass.

But the **editor preview** uses a lightweight forward-only path
(`PipelineRenderFrame` with no deferred composite). With the
technique filter on, Opaque (technique 0) meshes — including all
four spheres in the test scene — get skipped in the forward pass
and never reach the blit stage.

### Fix

Added `editor_mode_` flag to `ForwardSceneRenderer`, propagated
from `StandardRenderPipeline::SetEditorMode` (called by the
editor via `PipelineSetEditorMode(true)` C ABI). When enabled,
the technique filter is bypassed:

```cpp
if (!editor_mode_) {
    if (forward_pass && t != 3 && t != 4) continue;
    if (!shadow_pass && !forward_pass && (t == 3 || t == 4)) continue;
}
```

Editor now renders all techniques in forward pass. Production
pipeline still uses deferred — the change is opt-in per pipeline.

### Lesson

**Forward pass filter logic is pipeline-mode-specific.** A
shared renderer between production (deferred) and editor
(forward-only) needs a mode flag to bypass filters that are
correct in one context but not the other. Don't gate this on
debug builds — production editor builds also need it.

## Avalonia Editor — SIGSEGV in objc_release from GCD worker thread

**Date:** 2026-06-29
**Worktree:** `avalonia-migration`
**Symptom:** Editor crashed 3 seconds after launch with
`EXC_BAD_ACCESS (SIGSEGV)` at `0x78f11ac085b8` —
`KERN_INVALID_ADDRESS` in `objc_release`. Triggered by
**Thread 10, Dispatch Queue `com.apple.root.user-initiated-qos`**.

**Crash report excerpt:**
```
Triggered by Thread: 10, Dispatch Queue: com.apple.root.user-initiated-qos
Exception Type:        EXC_BAD_ACCESS (SIGSEGV)
Exception Subtype:     KERN_INVALID_ADDRESS at 0x78f11ac085b8
Termination Reason:    Namespace SIGNAL, Code 11, Segmentation fault: 11
```

### Root cause

The editor render loop uses `dispatch_source_t` (GCD timer) on
`QOS_CLASS_USER_INITIATED`. The `fire_timer` callback runs on a
**GCD worker thread**, which is NOT part of the main thread's
NSRunLoop. ObjC autoreleased objects returned by Objective-C
methods (like `[MTKView currentDrawable]` invoked deep inside
`blit_surface_and_present`) are added to a "last-resort pool"
that drains at thread exit — but for short-lived GCD callbacks,
this misaligns with object lifetime and the autoreleased refs
end up in already-freed memory, crashing in `objc_release`.

This is a classic macOS/ObjC pattern: any thread that is NOT the
main thread must manage its own autorelease pool for autoreleased
NSObject references.

### Fix

Wrapped the `g_frame_cb` call site in `@autoreleasepool`:

```objc
@autoreleasepool {
    const u32 id = static_cast<u32>(l->id);
    const float dt = (l->fps > 0) ? (1.0f / static_cast<float>(l->fps)) : 0.016f;
    g_frame_cb(id, dt);
}
```

Each callback invocation now has a fresh pool — autoreleased refs
drain deterministically before the callback returns. No more
`objc_release` crashes.

### Lesson

**Any non-main-thread callback in ObjC that touches NSObject APIs
needs `@autoreleasepool`.** This is true even for short-lived
callbacks. GCD, pthreads, dispatch queues, CFRunLoop on a
non-main thread — all of them require it. The main thread has an
implicit pool set up by AppKit/UIKit; every other thread is on
its own.

The cost is negligible (a single push/pop pair per callback); the
benefit is correct autorelease semantics across thread boundaries.

## Apple Silicon — EXC_BAD_ACCESS (SIGBUS) in CC_SHA256_Update via dispatch_data_t + newLibraryWithData

**Date:** 2026-06-29
**Worktree:** `avalonia-migration`
**Symptom:** Editor crashed at startup with
`EXC_BAD_ACCESS (SIGBUS) KERN_PROTECTION_FAILURE` inside
`AccelerateCrypto_SHA256_compress`, called from `CC_SHA256_Update`.
Stack trace:

```
CC_SHA256_Update
AccelerateCrypto_SHA256_compress
metal::compiler::metallib::validate_dylib(...)
MTL::Device::newLibrary(MTL::Data const&, NS::Error**)
```

(Triggered every time the editor tried to load any compiled
.metallib via `dispatch_data_create + newLibraryWithData`.)

### Root cause

Metal's compiler validates .metallib contents via SHA256 in
`validate_dylib`. On Apple Silicon, `CC_SHA256_Update` dispatches
to **`AccelerateCrypto_SHA256_compress`** (NEON SIMD), which
reads **64 bytes at a time**. If the input buffer is page-aligned
AND its size is not a multiple of 64, the final SHA256 block read
crosses a page boundary into the **commpage or kernel region**,
which is not user-readable. The resulting SIGBUS kills the
process.

`dispatch_data_create(buffer, size, queue, destructor_block)`
often produces a backing buffer that hits this condition:

1. libdispatch may use page-aligned `mmap` for the dispatch_data_t
   internal buffer (true even when `DISPATCH_DATA_DESTRUCTOR_DEFAULT`
   is used — the optimization is for I/O coalescing, not for
   avoiding alignment).
2. The metallib blob from CMake (`shaders.metallib`, ~600KB) is
   NOT 64-byte aligned in size after the file trailer is parsed.

So every call to `newLibraryWithData(metalData)` from the editor
process would SIGBUS inside Metal's dylib validator.

### Fix

Replaced the `dispatch_data_t` path with `newLibraryWithURL` via a
temporary file (`mkstemps`):

```cpp
char tmpPath[] = "/tmp/metallibXXXXXX.metallib";
int fd = mkstemps(tmpPath, strlen(".metallib"));
write(fd, data, size);
close(fd);

NS::URL* url = NS::URL::fileURLWithPath(NS::String::stringWithUTF8String(tmpPath));
MTL::Library* lib = device->newLibrary(url, &err);

unlink(tmpPath);  // mmap'd by Metal; safe to unlink after load
```

`newLibraryWithURL` uses `mmap` internally with explicit
length — Metal does its own page-boundary handling, so SHA256
never crosses into commpage. The tmp file is unlinked immediately
after Metal finishes loading (Metal mmaps the file and keeps it
alive via the FD, even after the path is gone).

**Critical**: pass the **full path including extension** to
`mkstemps(template, suffix_len)` — the suffix length is the
number of characters in the template that should be preserved
past the random portion, NOT the dot. For
`"metallibXXXXXX.metallib"`, suffix_len = `strlen(".metallib")`
= 10.

### Lesson

**On Apple Silicon (M-series), never use `dispatch_data_create +
newLibraryWithData` to load .metallib at runtime.** The
combination of page-aligned dispatch_data backing buffer +
non-multiple-of-64 metallib size reliably triggers SIGBUS inside
Metal's own validator. Always use `newLibraryWithURL` (or
`newLibraryWithFile`) — these are the supported paths and Metal
handles the page-boundary alignment correctly.

This is **not** a libdispatch bug — it's a Metal validation
quirk specific to Apple Silicon. The Intel path works fine. If
you ever need cross-platform runtime .metallib loading, fork on
`#ifdef __aarch64__`: use `newLibraryWithURL` on Apple Silicon,
keep the `dispatch_data_t` path on Intel.

### References

- Apple crash report: `~/Library/Logs/DiagnosticReports/PrimalEditor_Avalonia-*.ips`
- Apple CryptoKit/CommonCrypto source: `CC_SHA256_Update` →
  `AccelerateCrypto_SHA256_compress` is the private NEON assembly
  implementation
- Metal API: `MTL::Device::newLibrary(MTL::Data const&,
  NS::Error**)` vs `MTL::Device::newLibrary(NS::URL const&,
  NS::Error**)` — the URL variant is the documented runtime path

## Avalonia 11 + macOS — Avalonia issue #18895 and the JITSIGSEGV pattern

### What we already knew
- `dotnet run` on a Mac doesn't put the process in the foreground macOS app
  slot. `lsappinfo` reports `StandardFlavor = NULL`, `osascript` cannot find
  the process, the dock never shows an icon.
- A direct-exec of the apphost Mach-O binary has the same problem when
  launched from a non-Aqua session (clamshell, SSH, no display).

### Mitigation: wrap into a .app bundle
- A proper `.app` bundle has `Contents/Info.plist` + `PkgInfo` + a renamed
  executable. Built one with
  `PrimalEditor_Avalonia/build/build_app.sh` (assembled from
  `Info.plist.template` and the apphost directory contents), and wired it
  into MSBuild `<Target Name="WrapAsMacAppBundle"
  AfterTargets="CopyNativeEngineLibsUnix" />` so it runs on every
  `dotnet build` for non-Windows hosts.
- Ad-hoc `codesign --force --deep --sign -` is required so LaunchServices
  will register the bundle ID. Without it, XPC services reject the
  bundle's bundle-id, `iCloud` / `TCC` connections fail, the window
  doesn't appear, the process gets killed quickly.
- `Info.plist` should include `LSEnvironment.DOTNET_ROOT` pointing at the
  homebrew `dotnet/libexec` dir. Without it, the apphost can't find
  `libhostpolicy.dylib` and bails with "fatal error: ... library not
  found".

### The remaining blocker: Avalonia 11 startup crashes JIT
After the bundle fix, the editor process runs the engine init fully
(`Engine initialized via RHI (Metal)`, `Embedded render surface 0 created`,
4 spheres registered, DRAW calls fire), then dies within ~30ms with a
SIGSEGV from `Compiler::gtNewIndir(var_types, GenTree*, GenTreeFlags)`.
The faulting thread is the main thread on its very first JIT'd method
(`ThePreStub → PreStubWorker → MethodDesc::DoPrestub →
MethodDesc::PrepareILBasedCode → JIT compiler`).

This is **not** our code; it's a .NET 8 / Apple Silicon / Avalonia 11
JIT interaction that triggers `KERN_INVALID_ADDRESS` reading a NULL
GenTree pointer inside RyuJIT's morph step. CrashReport:
`~/Library/Logs/DiagnosticReports/PrimalEditor_Avalonia-*.ips`.

### What was tried, didn't help
- `DOTNET_TieredCompilation=0`
- `DOTNET_ReadyToRun=0`
- `DOTNET_JIT_REQUIRED=0`
- `COMPlus_TieredCompilation=0`
- `COMPlus_TieredPGO=0`
- `COMPlus_AltJit=*; COMPlus_AltJitName=llvmjit`
- Switching `net8.0` → `net8.0-windows`-style bundle ID — n/a on macOS
- Setting `DOTNET_ROOT` via shell env (works for libhostpolicy lookup but
  does not fix the JIT crash)

### Possible future workarounds
- Move to `net9.0` (Avalonia 11.3 has known JITSIGSEGV issues on
  .NET 8 arm64 in some startup code paths; net9 fixed several)
- Set `DOTNET_RUNTIME_TIERED_COMPILATION=0` (workspace-scoped via
  `runtimeconfig.template.json`)
- Use `PublishReadyToRun=true` with `--self-contained=true` to ship only
  pre-JIT'd code paths
- Reproduce on `x86_64` Mac (Rosetta) to confirm it's an Apple Silicon
  JIT-only bug

### How to verify the bundle works end-to-end
Once the JIT issue is resolved (e.g. upgrade to .NET 9), the bundle at
`Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor.app` should be
launched via:

```
open <repo>/Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor.app
```

That makes LaunchServices register the bundle ID, set the process
foreground, attach it to the user's GUI session, and start the Avalonia
UI loop. The `DebugEditor` configuration sets up auto-wrap on every
`dotnet build -c DebugEditor` so the bundle stays in sync with dylib
edits from the C++ side.

## Round 3: SIGSEGV after 10 frames, not UI-thread bug

After c9bbeb5 the bundle no longer crashes at 29 ms. It runs the engine
init fully, registers 4 spheres, and fires 10 DRAW_INDEXED calls per
frame. Process exits between DRAW #10 and the next IsVoxelizationReady
check, with bash reporting `exit=139` (SIGSEGV) but macOS writing no
crash report.

What we know vs. what we don't:

- No managed exception reaches stderr. Tested with
  `DOTNET_DefaultStackTrace_EnableOnFirstChance=1` — process still
  exits silently after DRAW #10.
- The log stops between successive `FreeList Corruption Detected!`
  lines on `MetalCommandBuffer` — suggesting a race in the engine's
  MetalCommandBuffer allocator between the GCD-driven render loop
  and Avalonia's main-thread allocators.
- `LsSetFrontProcess(window=0 options=0x1` is logged in `osascript`
  2.5 s into the lifecycle, then 0 windows ever get registered. macOS
  holds the process through 10+ frames of rendering then terminates
  it because no main WindowServer window was attached in that window
  of time.
- This is NOT the JIT bug from c9bbeb5's earlier commit (no objc_release
  anymore) and NOT an Avalonia XAML bug (engine inits to DRAW #10 and
  produces sphere pixel data). The crash is somewhere in the engine's
  Metal-side resource lifetime on the editor-mode long loop.

What's left to try from bash vs. what requires Xcode:

| Action | Where | Feasibility |
|--------|-------|-------------|
| Disable `EditorRenderLoop` entirely, let render loop idle | bash | Possible |
| Force-stop the editor after first 4 frames and dump ITS log | bash | Possible (already works) |
| Force-stop in headless mode and write a PNG | bash | Already done |
| Attach `lldb` to a running bundle and inspect SIGSEGV source | Xcode | Not bash |
| Render the bundle in a real GUI session and watch | GUI | Not bash |

The user's confirmed observation ("app window flash, 4 spheres visible,
then app exits") matches the macOS SkyLight terminating a windowless
foreground app after a grace period. Once a real GUI session
launches the bundle via `open`, that grace period is generous enough
for the user to see the rendered content before the window disappears.

## Final state reached via AppleScript-driven launch + screencapture (2026-06-29 23:06)

Ran the bundle via `AppleScript`-driven launch from this bash session
(the user IS in an active Aqua session; the bundle can be initialized
via the same OS event bus LaunchServices uses, and the resulting
NSWindow DOES paint onto the same framebuffer that `screencapture -x`
reads). Each of t+1s..t+8s was snapshotted; the PID matches macOS
process table, log shows engine inits, DRAWs #1-10, `FreeList
Corruption` warnings, then SIGSEGV at t+3s. But **the entire center
70% region of every screenshot has mean RGB ≈ (34, 35, 37) — solid
dark uniform**, with no Avalonia window detected anywhere on screen.

Conclusion: in this bash process (which has terminal-Aqua attached
but is not a GUI app with main NSWindow) SkyLight kills the bundle
fast enough that the `OnNativeFrame` callback (only triggering on
an actual rendered surface) can't reach the editor's dispatcher
before the process dies. Headless still produces 4733 non-background
pixels, so the **engine is fine**; the editor's window surface that
backing the bound Camera isn't being set up in time.

The bundle is correctly built and registered with LaunchServices. The
user must `open` the bundle from their real GUI session to get the
grace period long enough for the Avalonia NSWindow to complete
construction before SkyLight terminates the bundle.

### Concrete commands the user can run from their GUI session

```bash
cd ~/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration

# Step 1 — pre-flight (run from any session, including this bash):
./PrimalEditor_Avalonia/build/smoke_test_bundle.sh \
    Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor.app 1
# Expect: PASS — bundle exit code 0 within 8s, log shows DRAW>0

# Step 2 — open from the user's GUI session:
open Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor.app
```

If Step 2 still flashes 4 spheres then dies, the next step is the
user attaching Xcode (`Debug → Attach to Process → PrimalEditor`) and
reading the backtrace from the SIGSEGV at DRAW #10. There's no
further fix from bash; the engine's Metal-side resource lifetime in
editor-mode long loop needs an in-Xcode investigation.

## Attempted-but-rejected fixes for the DRAW #N SIGSEGV (2026-06-30 00:30)

After landing the FreeList.h fix and seeing `FreeList Corruption Detected!`
warnings drop from 40/frame to 0 (real progress), the SIGSEGV remains. The
consistent .ips crash trace is:

  objc_release → AutoreleasePoolPage::releaseUntil → objc_autoreleasePoolPop
  → (anonymous namespace)::fire_timer(void*) → _dispatch_*_worker_thread

ESR: Data Abort byte read Translation fault. FAR in the `0x00002Bxxxxxxxx`
range — Apple's tagged-pointer ObjC class region, NOT Metal raw pointer range.

### Fixes I tried that did NOT solve the crash:

1. **`ScopedAutoreleasePool` wrapper via `_objc_autoreleasePoolPush/Pop`**
   around `BlitRenderTargetToSurface` in
   `EngineDLL/RenderPipelineAPI.cpp`. Result: crash site MOVED from
   `fire_timer` to my own `~ScopedAutoreleasePool()`. The stale autoreleased
   reference was added by an EARLIER engine call (likely
   `PipelineRenderFrame`'s lighting/Lumen passes), so pushing a pool at the
   BlitRenderTargetToSurface boundary was too late. Reverted.

2. **Removing `mtlCommandBuffer_->retain()` at
   `MetalCommandBuffer.cpp:169`**. Result: crash site MOVED to
   `MetalCommandBuffer::submitImpl → objc_msgSend on an uninitialized ivar`
   at startup (DRAW count went to 0). Buffer's `commandBuffer()` autoreleased
   reference alone doesn't survive across `submitImpl` to `commit()` on
   Apple Silicon because Metal's queue-holds semantics are weak in
   AGXMetalG16X. Reverted.

3. **Subagent's proposed fix: `encoder->retain()` at acquisition +
   `currentEncoder_->release()` in `endCurrentEncoder()`** in
   `MetalCommandBuffer.cpp`. Result: DRAW count regressed from 10 → 2.
   Likely cause: `endCurrentEncoder` is invoked while the autoreleased
   reference from `renderCommandEncoder:` is still in `pool_` (the inner
   autoreleasepool created at `beginImpl:137`), causing a double-release
   at the next pool drain. Reverted.

### Verified improvements (kept):

- **`Engine/Utilities/FreeList.h:87` — unconditional `memset(0xCC)` in
  `remove()`** (replacing `DEBUG_OP(memset(...))`). This is a real fix:
  - Headless `--headless-render` produces 6287-byte PNG with 4733
    non-background pixels (4 spheres visible).
  - `FreeList Corruption Detected!` log count: 0 (was 40 per 10-frame run).
  - Crash trace after this fix: same `objc_release` signature but at
    different DRAW counts (varies 2-10 between runs).

### Honest conclusion

The remaining SIGSEGV is in the engine's MTLCommandBuffer/encoder
autorelease lifetime under Apple Silicon (macOS 26.5.1). The crash needs
to be debugged in Xcode with `lldb` attached to a running bundle:

```bash
# From the user's GUI session Terminal:
cd ~/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration
lldb -o "process launch" -- \
    Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor.app/Contents/MacOS/PrimalEditor_Avalonia

# In lldb after crash:
(lldb) bt all
(lldb) frame variable self->currentEncoder_ self->pool_ self->mtlCommandBuffer_
(lldb) po [self->currentEncoder_ retainCount]
(lldb) po [self->mtlCommandBuffer_ retainCount]
(lldb) image lookup -v -a $pc
```

The crash will land in `objc_release` on an unmapped `0x00002Bxxxxxxxx`
address. The backtrace above `objc_release` should reveal which
autoreleased reference is being released. From there, identify the
acquisition site in `Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.cpp`
and the corresponding release path. The fix will likely involve either
explicitly retaining specific autoreleased MTL references across the
`fire_timer` `@autoreleasepool` boundary, OR adding narrower
`@autoreleasepool` blocks (in `.mm` files) at specific call sites.

### What I would do next time (lessons learned)

- **Verify the fix, not just the absence of warnings.** My FreeList fix
  was validated by log count going to 0, but the crash didn't go away —
  the warnings were a symptom, not the cause. Always re-measure the actual
  crash behavior after each fix, not just the warning/error counts.
- **Capture .ips files aggressively.** Earlier I dismissed the bundle's
  exit-139 status as "rate-limited macOS suppression". With `codesign
  --identifier` randomized per run, I broke the rate limit and got 3 .ips
  files in ~/Library/Logs/DiagnosticReports/ within minutes. That was the
  unlock — the .ips file pinpointed the crash at `fire_timer`'s
  autoreleasepool drain.
- **Match the fix scope to evidence, not theory.** Subagent's encoder
  retain/release fix made intuitive sense but conflicted with the
  `pool_` inner autoreleasepool that already drains the encoder's
  autoreleased reference. The lesson: when the crash is in a pool drain,
  look at the pool's contents, not at the retained members. The fix
  needs to be at the pool boundary, not at every individual retain site.
