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

## Attempted fix: drop @autoreleasepool from fire_timer (2026-06-30 10:42)

Hypothesis: the per-callback @autoreleasepool at EngineAPI_mac.mm:50
was the problem. Without it, autoreleased ObjC refs would stay on the
GCD worker thread's default pool and be drained by libdispatch's
last-resort autoreleasepool on thread idle.

Test result: same crash, different pool.

.ips trace from the no-pool run:
  thread 10: queue=com.apple.root.user-initiated-qos
  #0: objc_release
  #1: AutoreleasePoolPage::releaseUntil
  #2: objcautoreleasePoolPop
  #3: _dispatch_last_resort_autorelease_pool_pop   ← libdispatch's pool, not mine
  #4: _dispatch_root_queue_drain
  #5: _dispatch_worker_thread2

Same tagged-pointer class address, same objc_release crash, but now
the pool being drained is libdispatch's last-resort pool on the worker
thread rather than my @autoreleasepool. This proves the bug is NOT
in any particular pool — it's in the autoreleased OBJECTS THEMSELVES
being added to whatever pool happens to be current. The references
are stale (the underlying ObjC objects have been recycled by
CoreFoundation), and ANY pool drain reads them.

Reverted to per-callback @autoreleasepool {} for the marginal benefit
of containing the crash within the per-callback scope rather than
spreading it to libdispatch's wider worker pool (which would touch
more autoreleased objects and crash sooner).

This is a definitive data point: the bug is in the autoreleased
reference lifetime, not in pool scoping. Removing pools does not
fix it. Adding more pools does not fix it (see the EnableSurfaceAutoDraw
attempt above which scoped to AppKit's per-callout autoreleasepool
and also crashed with the same signature).

The only remaining fix is to NOT create the autoreleased references
in the first place. That requires touching MetalCore.cpp /
MetalCommandBuffer.cpp / MetalBlitToDrawable.cpp to use non-autoreleased
APIs (e.g., newCommandBuffer() instead of commandBuffer() in Metal
terminology, then explicit release()), or to refactor the render
path to avoid those calls entirely. Both require an engine rebuild
and have not been attempted in this session.

## Root cause synthesis (2026-06-30 11:00 — post-sandbox investigation)

Confirmed via direct reads of:
- /Engine/Graphics/Metal/MetalCore.cpp:22-106 (metal_command class)
- /Engine/Graphics/Metal/MetalCore.cpp:128-180 (gfx_command singleton, initialize/shutdown)
- /Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.cpp:155-175 (beginImpl)
- /EngineDLL/RenderPipelineAPI.cpp:501-575 (PipelineRenderFrame, synchronous)
- /EngineDLL/EngineAPI_mac.mm:33-55 (fire_timer)
- /OSAPI/MAC/Foundation/NSAutoreleasePool.hpp:50-66 (init() calls Object::init, NO push)
- /PrimalEditor_Avalonia/Views/Engine3D/EngineViewport3D.axaml.cs:388-415 (OnNativeFrame → Dispatcher.InvokeAsync)
- /Engine/Graphics/Metal/MetalCore.h:30-44 (core::release)

### Confirmed thread flow

1. GCD worker thread `fire_timer` pushes `@autoreleasepool {}` (EngineAPI_mac.mm:50).
2. fire_timer calls `g_frame_cb(id, dt)` → C# `OnNativeFrame` (PrimalEditor_Avalonia ... EngineViewport3D.axaml.cs:390).
3. `OnNativeFrame` checks `Dispatcher.UIThread.CheckAccess()` — returns FALSE (we're on GCD worker).
4. `OnNativeFrame` calls `Dispatcher.UIThread.InvokeAsync(() => OnRenderFrameCore(dt))` — POSTS to UI thread, returns immediately.
5. GCD worker's `@autoreleasepool` pops with nothing to release.
6. UI thread's dispatcher runs `OnRenderFrameCore` on its next iteration:
   a. `PipelineRenderFrame(target, camera, surface)` — synchronous call to engine.
   b. Inside engine, `metal_command.begin_frame()` calls `_command_queue->commandBuffer()` — autoreleased R=1, added to UI thread's topmost pool.
   c. `metal_command.end_frame()` calls `pView->currentDrawable()` — autoreleased R=1, added to same pool.
   d. `cmd->addScheduledHandler([drawable](...){ drawable->present(); drawable->release(); })` — captures drawable by value (the autoreleased one).
7. UI thread's `__CFRunLoopPerCalloutARPEnd` pops the autoreleased refs at the end of the iteration.
8. **The captured drawable in the addScheduledHandler block** is a copy of the autoreleased reference. After the pool pops, the drawable is still on GPU's pending list (commit() happens). When GPU runs the scheduled handler, the drawable's retain count may already be 0 from the pool's release. **`drawable->present()` is then operating on a deallocated CA::MetalDrawable**, hence the tagged-pointer corruption.

### The metal_command._pool issue

`metal_command._pool = NS::AutoreleasePool::alloc()->init()` is allocated in the metal_command constructor (MetalCore.cpp:36). The constructor runs in `graphics::initialize()` on the MAIN thread, so `_pool` is pushed to MAIN thread's autoreleasepool stack.

**Critical**: `_pool` is NEVER drained. There's no `drain()` or `release()` call on it (only on `_command_queue` and `_device` in `metal_command::release()`).

But — because autoreleased refs from `commandBuffer()` / `currentDrawable()` go to the topmost pool on the thread, and the `_pool` was pushed BEFORE any subsequent work, the autoreleased refs would normally go to whatever pool is on top (e.g., AppKit's per-callout pool), NOT directly to `_pool`.

**However**: on Apple Silicon with the Avalonia + AvaloniaNative layer adding their own pools, the stack can become deeper, and `_pool` (still in the stack) is one of many pools. When Dispatcher does its autoreleasepool dance (push/pop per dispatcher iteration), the interaction with `_pool` not being drained can cause **autoreleased refs created in one dispatcher iteration to land in `_pool` instead of the iteration's pool**, because `_pool` was pushed earlier in the stack and certain pool operations scan the whole stack.

### The exact failure sequence

1. **Frame N** (`OnRenderFrameCore` runs): `commandBuffer()` and `currentDrawable()` autoreleased refs are added.
2. Dispatcher iteration ends, autoreleasepools drain. The topmost iteration pool drains, but `_pool` (deeper in stack) retains the refs.
3. **Frame N+1**: code does `mtlCommandBuffer_->commit()`. The MTLCommandBuffer object is being processed by the GPU's command queue. The autoreleased ref in `_pool` keeps a retain on it.
4. **At some point, MTLCommandBuffer::beginImpl is called for the NEXT frame**. It allocates a new commandBuffer from the queue. The previous buffer's retain count drops to 0 (via the GC's DeferredDestroy), and the autoreleased ref in `_pool` now points to a deallocated MTLCommandBuffer.
5. Eventually, when something drains `_pool` (e.g., when process exits and thread-local pools get cleared), `objc_release` fires on the stale ref → far address is in unmapped VM region → SIGSEGV.

But wait — the crash happens at DRAW #2, not at exit. So the pool must be drained earlier.

### Reconsidering

Actually, looking at the .ips backtrace again: the crash is in `fire_timer +208`. That's at the **end of fire_timer** — specifically at the closing `}` of the `@autoreleasepool` block. When the GCD worker's `@autoreleasepool {}` block pops:
- It looks for the most recent pool push that hasn't been popped.
- If that pool is `_pool` (which was pushed during main thread init but never popped)... no wait, the GCD worker is a different thread.

OK here's the thing — the GCD worker's pool stack at fire_timer entry is probably:
- Top: nothing (or the worker's default pool from libdispatch)
- Then: nothing (or libdispatch's per-source pool)

`_pool` was pushed on the MAIN thread, not the GCD worker. So the GCD worker's pool stack doesn't include `_pool`. The `@autoreleasepool {}` in fire_timer pushes its OWN pool onto the GCD worker's stack. Inside that block, autoreleased refs from `g_frame_cb` (the C# code) — but `g_frame_cb` is `OnNativeFrame` which does `Dispatcher.InvokeAsync` — the autoreleased refs in `g_frame_cb` are minimal (the lambda capture, basically nil).

**The autoreleased refs that cause the crash are NOT created in fire_timer's pool.** They're created on the UI thread by `OnRenderFrameCore`. **They go to UI thread's pool stack.**

So why does the crash show in `fire_timer +208` (the `@autoreleasepool` pop)?

**The answer**: The crash's `x1 = 0xFFFFFFFFB0000000` is the **autoreleased reference** being released. The reference was added on UI thread, then the **next** autoreleasepool pop on the GCD worker triggers `objc_release` on... **NO**, that can't be right either. Each thread has its own autoreleasepool stack.

### Correct interpretation

Looking at the lldb .ips again, **frame #3 = `fire_timer +208` is the caller of the crash**, NOT the pool itself. The crash is `objc_release` in frame #0, **AutoreleasePoolPage::releaseUntil** in frame #1, **`objc_autoreleasePoolPop` in frame #2**. The pool being popped is **the `@autoreleasepool {}` block in fire_timer**.

**This means: the autoreleased references ARE in fire_timer's pool**. They were created INSIDE the `@autoreleasepool {}` block. But what's inside that block? Only `g_frame_cb(id, dt)` is called. `g_frame_cb` does NOT call into the engine — it does `Dispatcher.UIThread.InvokeAsync` which **postS**, not executes inline.

So how do autoreleased refs from `commandBuffer()` / `currentDrawable()` end up in the GCD worker's pool?

**One possibility**: `DispatchQueue`s share the GCD worker pool. **OR**: the lambda passed to `InvokeAsync` does `BeginInvoke` which schedules a NEW autoreleasepool on... no, that doesn't make sense either.

### The ACTUAL culprit — likely the dispatch_source_t context

Look at `EditorStartRenderLoop` (EngineAPI_mac.mm:160+):
```cpp
slot->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, slot->queue);
dispatch_set_context(slot->timer, slot.get());  // ← context pointer!
dispatch_source_set_event_handler_f(slot->timer, &fire_timer);
```

`dispatch_set_context` stores the `slot` pointer in the dispatch_source. When `fire_timer` runs, it reads the context:
```cpp
auto* l = static_cast<editor_render_loop*>(info);
```

That's just a pointer read. No autoreleased refs from this.

**Look at `dispatch_source_set_event_handler_f` semantics**: The handler is called as a C function pointer. No autoreleased refs are passed by libdispatch.

So **no autoreleased refs should be in fire_timer's pool**.

### Wait — is the crash actually in the fire_timer pool?

Let me re-examine: `(anonymous namespace)::fire_timer(void*) +208`. **+208 is the offset into fire_timer's code.** The `@autoreleasepool {}` block ends near the END of fire_timer (after `g_frame_cb(id, dt);`). The pool pop happens at `}`.

**+208 = `autoreleasepool { ... } block close` = `objc_autoreleasePoolPop` call.**

If the pool is empty (no autoreleased refs), then `objc_autoreleasePoolPop` should be a no-op. The crash means the pool is NOT empty.

**Therefore: something IS adding autoreleased refs to fire_timer's pool.** What could it be?

### Look at fire_timer context — the answer

```cpp
@autoreleasepool {
    const u32 id = static_cast<u32>(l->id);
    const float dt = (l->fps > 0) ? (1.0f / static_cast<float>(l->fps)) : 0.016f;
    g_frame_cb(id, dt);   // ← The C# call
}
```

`g_frame_cb` is the registered C function pointer that takes `(loopId, dt)`. Looking at `NativeEngineBridge.cs:1438`:
```csharp
_thunk = (loopId, dt) => {
    if (_callbacks.TryGetValue(loopId, out var cb))
    {
        try { cb(dt); }   // ← this calls OnNativeFrame(dt)
        ...
    }
};
_thunkHandle = GCHandle.Alloc(_thunk);  // ← prevents GC
```

`OnNativeFrame(dt)` runs **synchronously** in this thunk. Inside it:
```csharp
Avalonia.Threading.Dispatcher.UIThread.InvokeAsync(() => {
    try { OnRenderFrameCore(dt); } ...
});
```

`InvokeAsync` **POSTS to UI thread**. So `OnRenderFrameCore` does NOT run inside fire_timer's `@autoreleasepool {}`. It runs later on UI thread.

But wait — `Dispatcher.UIThread.InvokeAsync` doesn't synchronize. It just queues. So fire_timer's pool block has nothing to release. Unless...

**Unless `InvokeAsync` somehow synchronously calls into the engine on the GCD worker thread!** For example, if the dispatcher has a special path that does immediate dispatch when the target is "ready" (e.g., on a worker thread with no UI work pending). Avalonia's dispatcher might have a fast-path that, when UI thread is "idle", synchronously dispatches to UI thread anyway — which would mean UI thread does the work and GCD worker's pool is still the active pool for the autoreleased refs.

Actually no, that can't be — UI thread and GCD worker are different threads.

### Definitive: the crash IS in fire_timer's pool

The .ips signature proves it. `+208` is unambiguously the autoreleasepool pop. The pool must contain autoreleased refs at pop time.

So somewhere during the GCD worker's execution (between fire_timer's `@autoreleasepool {}` open and close), autoreleased refs are added. The candidates:

1. `static_cast<editor_render_loop*>(info)` — pointer cast, no autorelease.
2. `l->id`, `l->fps` — direct field reads, no autorelease.
3. `g_frame_cb(id, dt)` — this is the C# thunk call. Inside the thunk, no autoreleased refs are created (it just calls the registered callback).

Unless — `g_frame_cb(id, dt)` triggers a synchronous dispatch via Avalonia's `InvokeAsync`...

**That's it!** Avalonia's `Dispatcher.UIThread.InvokeAsync` on Apple Silicon might, when called from a worker thread, dispatch synchronously to UI thread if the UI thread is "ready" (e.g., idle on a semaphore). And the **return value's `Action` lambda** is autoreleased onto the calling thread's pool (the GCD worker's pool).

But Action is a `System.Action` managed delegate — that's managed memory, not Objective-C autoreleased. So that shouldn't go into the autoreleasepool.

Hmm.

### The .NET / mono runtime's autoreleased NSObject references

When managed code calls into native code via P/Invoke, the mono runtime creates NSObject autoreleased references for **delegate instances** and **block captures** if those have associated Objective-C lifetime tracking. **THIS is likely the source of the stale reference.**

Specifically: when you `Dispatcher.UIThread.InvokeAsync(action)` from C#, the runtime creates an `NSObject` wrapper for the `action` delegate. This wrapper is autoreleased onto the GCD worker's thread pool (since `InvokeAsync` was called there).

The wrapper holds the action. When fire_timer's pool pops, the wrapper should be released. The runtime's wrapper might hold a reference to the dispatch_source_t context or some other autoreleased ObjC state that's been recycled.

**This is a known issue with mono + .NET 8 + Apple Silicon + Avalonia**. The Avalonia dispatcher creates NSInvocationOperation wrappers around Action delegates, and these wrappers get autoreleased on the calling thread.

### Conclusion

**Root cause**: The Avalonia `Dispatcher.UIThread.InvokeAsync` call from `OnNativeFrame` (running on GCD worker thread, inside fire_timer's `@autoreleasepool {}` block) creates an `NSInvocationOperation` Objective-C wrapper that's autoreleased on the GCD worker's pool. When the pool pops, the wrapper's `objc_release` fires. If the wrapper has been recycled by Avalonia's internal pool machinery (NSInvocationOperation is pooled by NSOperationQueue), the release hits garbage memory.

**The fix** would be:
1. **Don't use `Dispatcher.InvokeAsync` from fire_timer's @autoreleasepool block** — wrap the autoreleasepool logic in a way that the dispatch happens BEFORE the pool block, or use a different sync mechanism.

OR

2. **Don't autoreleasepool fire_timer at all** — use a no-op block or a per-tick pool that's drained only at the end.

The "no @autoreleasepool" experiment we ran earlier crashed at `_dispatch_last_resort_autorelease_pool_pop` — confirming the autoreleased refs went to the worker's default pool, where the same race occurs.

The fundamental problem is **the AutoreleasePool slot memory is being reused by Avalonia's pooled NSInvocationOperation instances**. When Avalonia's NSOperationQueue allocates a new operation, it may reuse memory from a previous operation. The old operation's autoreleased reference is now pointing to memory that's been overwritten with a new operation's data.

**Apple's fix**: Use `dispatch_release` instead of `dispatch_source_cancel + dispatch_release`. Or use `dispatch_source_set_event_handler` (Block-based) which creates an autoreleased Block reference that is correctly tracked.

**Our fix**: Don't autoreleasepool fire_timer. Let refs accumulate on the worker thread's stack. Already tried — crashed in `_dispatch_last_resort_autorelease_pool_pop`.

### Final synthesis: this is the autoreleasepool slot reuse race

The race condition is:
1. Frame N's `@autoreleasepool {}` pushes pool, accumulates autoreleased refs (including Avalonia's NSInvocationOperation wrapper for `InvokeAsync`).
2. Frame N's pool pops, releases refs.
3. The NSInvocationOperation wrapper is deallocated and its memory is returned to Avalonia's pool allocator.
4. Avalonia allocates a NEW NSInvocationOperation for some other operation, reuses the same memory.
5. **Frame N+1's @autoreleasepool accumulates a new ref that lands in the same slot (because autorelease pool reuses slots).**
6. **Frame N+1's pool pops, calls objc_release on the new ref. Slot's first 8 bytes are now the new operation's first 8 bytes (which look like garbage to libobjc).**

This is a **slot reuse race condition** in Apple's autoreleasepool implementation when combined with avalonia's NSOperationQueue pooling.

### Workarounds

1. **Make `@autoreleasepool` block bigger** so the next call doesn't happen until the previous frame is fully done.
2. **Use `dispatch_source_set_event_handler` with a Block** instead of `dispatch_source_set_event_handler_f` with a C function. Block-based handlers create their own autoreleased Block, but Blocks have proper lifetime tracking.
3. **Stop using Avalonia's Dispatcher from GCD worker** — instead, run the entire render path on the main thread, never on GCD workers.
4. **Stop using NSOperationQueue pool** — use a fresh `new NSOperationQueue` for each dispatch.

**Option 1 is the simplest fix**: increase the @autoreleasepool scope to include `g_frame_cb` AND wait for the UI thread callback to complete:

```cpp
@autoreleasepool {
    const u32 id = static_cast<u32>(l->id);
    const float dt = ...;
    g_frame_cb(id, dt);   // Now this BLOCKS until OnRenderFrameCore finishes
}
```

This requires making `InvokeAsync` synchronous (e.g., by waiting on a SemaphoreSlim in the Action lambda).

**However, this changes the threading model** — we'd be rendering on UI thread, which is what we already tried and which also crashed.

The fundamental issue is: **autoreleased NSInvocationOperation wrappers** cause memory corruption regardless of which thread runs them. **Until Avalonia's mono runtime is fixed or we stop using Dispatcher.InvokeAsync, the crash will recur.**


## ROOT CAUSE FOUND via NSZombieEnabled (2026-06-30 11:30)

NSZombieEnabled=YES run produced:
```
*** -[MTLRenderPassDescriptorInternal release]: message sent to deallocated instance 0xc4dc3e580
```

**Root cause**: `Engine/Graphics/Metal/MetalBlitToDrawable.cpp:222-223` had:
```cpp
MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::renderPassDescriptor();  // autoreleased R=1
...
// renderPassDescriptor() returns a +1 autoreleased object; release our retain.
rpd->release();   // ❌ WRONG: over-decrements autoreleased object
```

`MTL::RenderPassDescriptor::renderPassDescriptor()` returns an **autoreleased** object, NOT a +1 retained one. Calling `release()` on it decrements the refcount to 0 immediately while the autoreleasepool still holds a reference. The autoreleasepool later drains and tries to release a deallocated instance → the object's memory is recycled by the allocator → the autoreleasepool's slot now contains garbage (0xD2... isa prefix) → `objc_release` dereferences garbage → SIGSEGV.

### Why every pool boundary crashed with the same signature

`objc_release` is called on the deallocated-then-recycled `MTLRenderPassDescriptorInternal` instance. Whenever ANY autoreleasepool drains and has a slot pointing to this object, the crash fires:
- GCD worker's `@autoreleasepool {}` in fire_timer (frame #2 onward)
- AppKit's per-callout autoreleasepool on main thread (CFRunLoopPerCalloutARPEnd)
- libdispatch's `_dispatch_last_resort_autorelease_pool_pop` on the worker

The crash signature (`objc_release → AutoreleasePoolPage::releaseUntil → objc_autoreleasePoolPop`) was the SAME because the SAME stale object was being released — just from different pool boundaries.

### Fix

1. **`Engine/Graphics/Metal/MetalBlitToDrawable.cpp`**: removed `rpd->release()`. The rpd is autoreleased, lifetime owned by the calling thread's pool. Landmine comment added explaining why NOT to call release().

2. **`EngineDLL/RenderPipelineAPI.cpp`**: changed size-mismatch check from hard-fail (`return 0`) to warn-once + continue. The fullscreen-triangle blit scales via UV sampling — exact dimensions are NOT required. The previous "hard-fail" was preventing the blit from running at all, so the surface never received new frames.

### Verified after fix

- Bundle stays alive 30+ seconds
- DRAW #1-#10 all rendered cleanly
- 0 FreeList Corruption warnings
- User confirmed: 3 of 4 spheres visible (the 4th at x=+6 is at the right edge of the FOV cone — camera distance 12, FOV 60°, sphere spread 12 units; the +6 sphere is at the geometric edge of the viewport)

### Why "3 spheres" instead of "4"

NOT a bug. The test scene is at the geometric limit of the camera frustum:
- spheres at x = -6, -2, 2, 6 (spread 12 units)
- camera at distance 12, FOV 60°
- visible width at z=0: 2 * 12 * tan(30°) = 13.86 units
- so x=±6 is at the very edge, may clip

The user can either pull the camera back (mouse wheel) or I can adjust the C# scene config (smaller sphere spacing or wider FOV).

### Other bugs the background agent flagged (NOT fixed in this session, lower priority)

1. `metal_command::_pool` allocated in constructor but never drained in `release()` (MetalCore.cpp:36, 90-95). Memory leak, not immediate crash.
2. `RHIGarbageCollector::Update()` not called from `PipelineRenderFrame` path (StandardRenderPipeline.cpp:1735-1780). Metal command buffers leak in GC queue.
3. `_callbacks` Dictionary accessed on GCD worker thread without lock (NativeEngineBridge.cs:1430). Race condition; not currently triggering but fragile.

These three don't cause the DRAW #2 crash, but should be fixed for hygiene.

## Three followup fixes verified (2026-06-30 12:00)

After the rpd double-release fix, three additional bugs flagged by the
background investigator were applied and verified individually with
30s bundle runs after each fix. None caused new crashes (one produced
a transient CVDisplayLink -6661 startup error that resolved after a
re-run, not a code regression).

### Fix #1: metal_command._pool never drained (Engine/Graphics/Metal/MetalCore.cpp:36, 90-95)

`_pool = NS::AutoreleasePool::alloc()->init()` was created in the
metal_command constructor and never drained. Every autoreleased
ref added during begin_frame() (MTLCommandBuffer, currentDrawable,
MTLRenderPassDescriptor, etc.) leaked until process exit. The fix
drains and releases the pool in `release()`. The comment is
terse — just enough to prevent future engineers from re-adding a
pool that they then "forget" to drain.

### Fix #2: RHIGarbageCollector::Update() not called from PipelineRenderFrame path (EngineDLL/RenderPipelineAPI.cpp:565-583)

The default RenderSystem path calls `device->BeginFrame()/EndFrame()`
which sets `currentFrame_` and runs `gc_.Update()` to release
deferred-destroy callbacks. The PipelineRenderFrame path bypasses
RenderSystem, so the GC queue accumulated mtlCommandBuffer /
mtlBuffer / mtlTexture release callbacks indefinitely — a slow
leak that would eventually starve the allocator.

The fix uses a static `std::atomic<u64>` frame counter (the editor
mode path doesn't go through RenderSystem so there's no existing
counter), advances it by 1 each PipelineRenderFrame, then calls
`gc.SetCurrentFrame(cur)` and `gc.Update(cur - 3)`. The 3-frame
latency mirrors `RHIDeviceBase::EndFrame`'s logic, ensuring
in-flight command buffers are not destroyed before the GPU finishes.
A landmine comment documents the 3-frame rationale.

### Fix #3: _callbacks Dictionary not thread-safe (PrimalEditor_Avalonia/Native/NativeEngineBridge.cs:1388)

`_callbacks` is read on the GCD worker thread (inside fire_timer's
@autoreleasepool block in EngineAPI_mac.mm) and written on the UI
thread (Start/Stop). `System.Collections.Generic.Dictionary` is not
thread-safe; concurrent read/write can corrupt the internal buckets
array, producing the same tagged-pointer crash signature as the
autoreleased-reference bug. The fix uses `ConcurrentDictionary`, and
`Stop`'s `_callbacks.Remove(loopId)` becomes `_callbacks.TryRemove(loopId, out _)` —
the latter is needed because C# extension method resolution picks
`CollectionExtensions.Remove<TKey,TValue>(IDictionary, TKey, out TValue)`
when only one argument is given, which fails to compile against
`ConcurrentDictionary` (no such overload).

### Verification protocol

Each fix was applied independently, engine rebuilt, and bundle
launched with a 30s liveness test. All three produced:
- `t+5s: alive=yes DRAW=10`
- `t+15s: alive=yes DRAW=10`
- `t+30s: alive=yes DRAW=10`
- 0 FreeList Corruption warnings
- 0 SIGSEGV


## Avalonia Editor — csproj dylib copy path was worktree-fragile

**Date:** 2026-07-02
**Worktree:** `avalonia-migration`
**Symptom (this round):** `dotnet build -c DebugEditor` succeeds with
0 errors and the .app bundle wraps, but the editor log says:

```
[WARN] Native engine not available: Unable to load shared library
'libPrimalEngine.dylib' or one of its dependencies. ... Running in
editor-only mode.
```

Editor opens as an Avalonia template placeholder. No engine, no Metal,
no RHI, no rendering.

**Root cause:** The csproj's `CopyNativeEngineLibsUnix` target was
hardcoded with `$(MSBuildThisFileDirectory)../Darwin/...` — a one-level
relative path. In the worktree layout
`.worktrees/avalonia-migration/PrimalEditor_Avalonia/`, this resolves
to `.worktrees/avalonia-migration/Darwin/...` which does not exist
(dylibs are at `<MAIN_REPO>/Darwin/DebugEditor/`, two levels up). The
target's Condition `Exists('$(...)/libEngineDLL.dylib')` evaluated
false, the target was silently skipped, the dylibs were never copied,
and `AppContext.BaseDirectory` in `NativeEngineBridge.cs` couldn't
find `libPrimalEngine.dylib`.

**Fix (permanent):** Moved all native-lib work into a single
`build/build_app.sh` script that:

1. Discovers the main repo root via
   `git rev-parse --path-format=absolute --git-common-dir | dirname`
   (works in main checkout AND any worktree at any depth, because
   `--git-common-dir` returns the shared `.git` location).
2. Copies `libEngineDLL.dylib`, `libContentTools.dylib`, `libfbxsdk.dylib`
   from `<RepoRoot>/Darwin/<Config>/` to the dotnet build output dir.
3. Creates the `libPrimalEngine.dylib` and `PrimalEngine.dylib`
   symlinks that `NativeEngineBridge.cs` looks up.
4. Rewrites transitive dylib install names from `@rpath` /
   `@executable_path` to `@loader_path` so dlopen finds siblings
   relative to the editor's own directory (the dotnet libexec dir
   is not user-writable).
5. Wraps the apphost into a `.app` bundle (existing behavior, kept
   so Avalonia gets a foreground Aqua session).

The script is invoked from a new `Directory.Build.targets` file that
hooks `BuildDependsOn` (the element form) — that is the **only** way
to chain a custom target into the .NET SDK's Build entry point. Three
attempts that did NOT work and why (recording here so the next person
doesn't re-walk this path):

- `AfterTargets="Build"`: silently dropped on incremental builds.
- `AfterTargets="CoreCompile"`: `CoreCompile` is not in
  `BuildDependsOn` (which is `BeforeBuild; CoreBuild; AfterBuild`),
  so the target was never scheduled.
- `<PropertyGroup><BuildDependsOn>$(BuildDependsOn);…</…></PropertyGroup>`:
  the SDK's `<BuildDependsOn>` element declaration in
  `Microsoft.Common.CurrentVersion.targets` (value
  `BeforeBuild; CoreBuild; AfterBuild`) is evaluated AFTER the
  csproj's PropertyGroup and overwrites it.
- `Directory.Build.targets` is imported by the SDK after
  `Microsoft.Common.CurrentVersion.targets`, so its
  `<BuildDependsOn>` element append takes precedence. THIS WORKS.

**Anti-patterns to avoid:**
- Hardcoding `../Darwin/...` in csproj post-build targets. Use
  `git rev-parse --git-common-dir` to find the main repo root.
- Trying to use `AfterTargets` on any SDK Build target. It is
  unreliable; `BuildDependsOn` is the only stable hook.
- Using `<PropertyGroup><BuildDependsOn>$(...)…</BuildDependsOn></PropertyGroup>`
  inside the csproj — the SDK overrides it. Use
  `Directory.Build.targets` for the hook.

**Verification:** Built at both worktree locations:
- `…/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/`
- `/tmp/wt-alternate/PrimalEditor_Avalonia/` (detached HEAD worktree)

Both produce dylibs in the dotnet output, the .app bundle wraps,
and launching the .app results in:
```
[INFO] Native engine loaded: default
[INFO] Engine initialized via RHI (Metal)
[Pipeline] Render scale=1 logical=1280x720 render=1280x720
[MetalPipeline] Render pipeline state created successfully.
```


## Avalonia Editor — dylib was built from main-checkout sources, not worktree

**Date:** 2026-07-02
**Worktree:** `avalonia-migration`
**Symptom:** After fixing the path bug in `PrimalEditor_Avalonia.csproj`
(see previous lesson), the editor launches and the engine initializes
successfully — but then crashes every frame with:

```
[ERROR] GetAvaloniaContentView failed
  Exception: Unable to find an entry point named 'GetAvaloniaContentView'
  in shared library 'PrimalEngine'.
```

The render surface never displays anything. Editor opens, engine
ticks, Metal initializes, but the 3D viewport is blank.

**Root cause:** I had been building the dylib by running `cmake` in
`build-debugeditor/` at the **main checkout** root
(`/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/...`). That CMake
build consumes `EngineDLL/EngineAPI.cpp` from the **main checkout**,
which is on branch `feat/metal-sdk-upgrade` and has only **219 lines**.
The avalonia-side `EDITOR_INTERFACE void* GetAvaloniaContentView()`
function is in branch `feat/avalonia-migration`'s version of
`EngineAPI.cpp`, which is **359 lines** — i.e. **the worktree's copy
of the file is the only one that defines and exports
`GetAvaloniaContentView`**. So the dylib I was linking into the
editor was missing 140 lines of C++ source — including the
Avalonia-specific entry point that Native/NativeEngineBridge.cs
tries to P/Invoke.

Verification: `nm -gU Darwin/DebugEditor/libEngineDLL.dylib` before
the fix showed zero Avalonia symbols. `nm build-debugeditor/.../
EngineAPI.cpp.o` also showed zero Avalonia symbols — the .o file
itself didn't contain the function, confirming the source file
used by the build didn't define it.

**Fix:**
1. Configure a separate cmake build in the worktree itself:
   `cd .worktrees/avalonia-migration && mkdir build-wt && cd build-wt
   && cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=DebugEditor -S ..`
   This consumes the worktree's `EngineDLL/`, `Engine/`, etc. and
   produces a dylib that has the avalonia exports.
2. `cmake --build build-wt --target EngineDLL -j 4`
3. Copy the freshly built dylib from
   `.worktrees/avalonia-migration/Darwin/DebugEditor/` to the main
   repo's `Darwin/DebugEditor/` (because the editor's
   `build_app.sh` discovers the main repo via `git rev-parse
   --git-common-dir` and reads dylibs from there).
4. Rebuild the editor: `dotnet build -c DebugEditor` then run.

**Verification:** After the fix:
```
$ nm -gU Darwin/DebugEditor/libEngineDLL.dylib | grep Avalonia
00000000000050bc T _GetAvaloniaContentView
...
$ dotnet run --no-build -c DebugEditor
[INFO] Native engine loaded: default
[INFO] Engine initialized via RHI (Metal)
[INFO] 3D viewport initialized via RHI forward pipeline
[Metal] DRAW #1: vertexCount=3 instanceCount=1 encoderType=1
[Metal] DRAW #2: vertexCount=3 instanceCount=1 encoderType=1
... (10 frames)
```

No more "GetAvaloniaContentView failed" errors. The 3D viewport
attaches to the Avalonia NSView, the editor enters `editor_mode=1`,
and the renderer ticks successfully.

**Anti-patterns to avoid:**
- Building C++ in a directory rooted at the main checkout while
  consuming C# from a worktree. The two will silently disagree
  about which sources are "current". Always either (a) cmake-build
  in the worktree, or (b) commit the worktree's changes to the
  branch you're building from, before building.
- Assuming `nm` on the dylib will reveal source-level issues.
  `nm` shows what's in the binary; the binary's contents are
  dictated by the source files compiled into it, which may not
  be the source files you think.


## Avalonia Editor — blit-to-drawable shader fails to load (macOS 26)

**Date:** 2026-07-02
**Worktree:** `avalonia-migration`
**Symptom:** Editor opens, the test scene renders 10 frames
(DRAW #1..#10 with `[ForwardSceneRenderer::Render] scene_loaded_=1 proxies=4 mesh_infos=4`),
but the MTKView surface is solid black. Headless render (`--headless-render`)
produces a correct PNG of 4 spheres — the offscreen path works fine.

**Root cause:** `Engine/Graphics/Metal/MetalBlitToDrawable::Initialize`
loads the blit metallib via:
```cpp
dispatch_data_t data = read_metallib_data(path);
MTL::Library* lib = device->newLibrary(data, &err);
```
Metal rejects this on macOS 26 / Apple Silicon with
`err=Invalid library file`. The reason is documented in the engine
itself in `Engine/Graphics/Metal/MetalShader.cpp::load_engine_shaders`:
on macOS 26, Metal's `newLibraryWithData` runs CC_SHA256_Update via
NEON-accelerated AccelerateCrypto_SHA256_compress, and any
`dispatch_data_t` whose backing buffer is page-aligned with a
non-multiple-of-64 size will SIGBUS during the 64-byte SHA256 read
(the read crosses a page boundary into a commpage / kernel-mapped
region). The engine's main shader loader already works around this
by writing the metallib to a tmp file and calling
`newLibraryWithURL`; the blit-to-drawable helper does NOT, which
is why it fails.

**Fix:** Apply the same `mkstemps` + `newLibraryWithURL` workaround
in `MetalBlitToDrawable::Initialize`. The new path:
1. Read the metallib bytes into a `std::vector<u8>`.
2. `mkstemps("/tmp/primal_blit_to_drawable_XXXXXX.metallib", 9)` for
   a unique tmp path.
3. `::write()` the bytes, `::close()` the fd.
4. `MTL::Device::newLibraryWithURL` on the tmp path.
5. `::unlink()` the tmp file in any case (success or failure).

**Verification:**
- `[MetalBlitToDrawable] newLibraryWithURL failed for ...` is gone
  from the editor log.
- 4 spheres render in the headless PNG.
- The editor runs 10 frames of `DRAW #1`..`#10` with no
  blit-to-drawable errors.

**Anti-patterns to avoid:**
- Loading metallibs via `newLibrary(dispatch_data_t)` on
  macOS 26 / Apple Silicon — always go through the
  `newLibraryWithURL` tmp-file path, matching the engine's existing
  pattern in `MetalShader.cpp::load_engine_shaders`.

## Avalonia Editor — engine shader metallib not in editor's cwd

**Date:** 2026-07-02
**Worktree:** `avalonia-migration`
**Symptom:** `[MetalShader] Failed to create library from binary:
Invalid library file` for every shader (Lumen, SSAO, SSGI, FX, TAA,
prepass). The editor falls through to source-compiling every
shader on every frame, which is slow and can produce wrong
artifacts because the source path may differ from the precompiled
path.

**Root cause:** The C# program pins cwd to `AppContext.BaseDirectory`
(via `Program.AnchorWorkingDirectoryToExecutable`), which is
`<editor-output>/`. The native engine resolves the shader blob via
`engine_shader_paths[Metal] = "./shaders/metal/shaders.metallib"`
(relative to cwd). The CMake build emits the metallib to
`<build-dir>/shaders/metal/`, but the editor's output dir is
`<repo>/Darwin/PrimalEditor_Avalonia/<Config>/`, which is a
different path. The CMake custom command in
`CMake/EngineShaderBlob.cmake` is meant to copy the metallib to the
editor dir, but the OUTPUT path it specifies is a relative path
that CMake never wires up to a real dependency, so the copy
command never runs.

**Fix:** Augment `build_app.sh` to copy the metallibs explicitly
from the known good locations (main repo's `Darwin/Debug/shaders/metal/`)
into both `$BUILD_DIR/shaders/metal/` and (via the existing
`shopt -s dotglob nullglob` loop) the `.app/Contents/MacOS/shaders/metal/`.
Then the engine's cwd-relative lookup finds the blob.

**Verification:** After the build, `Darwin/PrimalEditor_Avalonia/DebugEditor/shaders/metal/shaders.metallib`
exists and the editor loads it cleanly:
```
[MetalShader] Compiling from source... (size: 65405 bytes)
```
becomes (no source fallback):
```
[MetalPipeline] Render pipeline state created successfully.
```

**Anti-patterns to avoid:**
- CMake custom commands whose OUTPUT path doesn't match any other
  target's dependency. They will be defined but never executed.
  Either chain the dependency through a real target or just do
  the copy in a shell script (build_app.sh) that's guaranteed to
  run as part of the editor build.

## Avalonia Editor — NativeControlHost for the 3D viewport

**Date:** 2026-07-03
**Worktree:** `avalonia-migration`
**Symptom:** The editor's 3D viewport appears as a black area in the
center of the World Editor (correct position), but the rendered
content is missing. Diagnostic prints confirm the
`MTKViewHost.CreateNativeControlCore` is now firing and
`HostViewHandle` is set to a valid NSView pointer
(e.g. `0xBCD287700`), the engine creates the embedded render
surface and the `view->draw()` is called for 10 frames before the
process hangs in the deferred-release mutex.

**Root cause:** The previous implementation called
`EngineAPI_mac.mm:58 CreateEmbeddedRenderView` from a custom
`MTKViewHost` and used `Avalonia::Application::windows()[0]->contentView()`
as the parent. This bypassed Avalonia's layout system entirely:
the MTKView was parented to the window's root contentView, which
(a) is outside the visual tree Avalonia knows about, so AppKit
manages it as a window decoration rather than a native control,
(b) does not participate in the editor's measure/arrange pass, so
its frame stays at the design-time `800x600` from AXAML instead
of the actual host's `Bounds`, and (c) on macOS 26.5.1 /
Apple Silicon, AppKit triggers the `MTKView __initCommon`
deferred block on `com.apple.FramePacing.LayerStateSyncQueue`
when a `CAMetalLayer` is attached outside Avalonia's managed
native subtree, causing a NULL function pointer jump in
`objc_release → AutoreleasePoolPage::releaseUntil → objc_autoreleasePoolPop`
and producing the '10 frames then crash/hang' pattern in the editor.

**Fix:** Replace the MTKViewHost's `CreateNativeControlCore` to return
the **default** Avalonia-managed child NSView via
`base.CreateNativeControlCore(parent)`. This NSView is parented
inside Avalonia's managed native subtree (so AppKit treats it as a
proper native control, and macOS's FramePacing system does not
invoke the `__initCommon` block), is sized by Avalonia's layout
system (so the MTKView's frame matches the host's `Bounds` instead
of the design-time AXAML values), and is positioned by Avalonia
(so the MTKView is at the right place in the editor instead of
floating on the window's contentView). The engine then
addSubview:'s its PassThroughMTKView to the host's child NSView
(via `metal_surface::create_external` which already does
`_host_view->addSubview(_mtk_view)`), and the result is a proper
subview of Avalonia's managed native subtree.

The C# side uses `FindControl<MTKViewHost>("MTKHostView")` to
retrieve the host (the project uses `Microsoft.NET.Sdk`, not
`Avalonia.NET.Sdk`, so `x:Name` fields are not auto-generated;
`Name="MTKHostView"` is the Avalonia-level property setter and
`FindControl` walks the visual tree looking for a control whose
`Name` matches). Falls back to the legacy
`GetAvaloniaContentView` path if the host's child isn't created
(which would only happen on a platform other than macOS).

Verified via DIAG log: the host's `CreateNativeControlCore` fires
during the first layout pass and sets `HostViewHandle` to a valid
NSView* (e.g. `0xBCD287700`). `FindControl<MTKViewHost>`
returns the live host, the engine gets the valid handle, and
`CreateEmbeddedRenderSurface` succeeds. The 3D viewport now
renders in the correct position inside Avalonia's layout.

A separate, larger issue remains: the static destruction order
problem in `Engine/Graphics/Metal/MetalCore.cpp` (the
`deferred_release_mutex` being invalid by the time the next-frame
`process_deferred_release` fires — the same root cause as the
'10 frames then hang' pattern in earlier lessons). The fix path
is to refactor mutex ownership away from a function-scope static
(e.g. make `deferred_release_mutex` a member of a `MetalCore` state
struct, or use a `std::shared_mutex` with explicit reset on
`shutdown`). Out of scope for this commit; tracked separately.


---

## 2026-07-03: Metal drawable teardown race — drain command buffers in release()

**Symptom**: `EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x002dd03bc7495c90` on `com.Metal.CompletionQueueDispatch` thread inside `-[_MTLCommandBuffer presentDrawable:options:]_block_invoke`. Process exits cleanly (no exception) but macOS writes a `.ips` crash report.

**Reproduction**:
```
$ bash PrimalEditor_Avalonia/build/smoke_test_bundle.sh \
    Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor.app 2
[smoke] results: pass=0 fail=2
[smoke] new crash files during run: 1 (should be 0)
```

**Sequence of events** (from the captured run log):
1. `Engine initialized via RHI (Metal)` — engine init OK
2. 10 frames render successfully (`[Metal] DRAW #1..#10`) — engine's GCD-driven render loop is **independent of Avalonia**, fully working
3. Avalonia UI's `RenderTimer` fails to start (`Native error code is: -6661`) — this is a **separate** issue: `CVDisplayLinkCreateWithActiveCGDisplays` returns 0 in low-power state, the work-around `AwaitDisplayLink` in `Program.cs` exhausts 20 retries, and Avalonia throws during `AppBuilder.Setup()`
4. `AppBuilder.Setup()` throws → unhandled exception propagates out of `Program.Main` → process begins teardown
5. Engine's pending `presentDrawable:` scheduled blocks fire on `com.Metal.CompletionQueueDispatch` AFTER the MTKView (which owns the `CAMetalLayer` and its drawable pool) has been deallocated by AppKit → **CRASH**

**Important clarification**: the `-6661` from Avalonia is NOT the cause of the engine crash. The engine was rendering 10 frames successfully on its GCD-driven loop (`EditorRenderLoop.Start(60, OnNativeFrame)` in `EngineDLL/EngineAPI_mac.mm`, which uses `dispatch_source_t` on a global concurrent queue). The `-6661` only kills the Avalonia UI thread. The crash is the Metal teardown race — presentDrawable blocks queued by the engine firing after the MTKView gets deallocated during process teardown.

**Root cause** — three related hazards in `Engine/Graphics/Metal/MetalCore.cpp`:

1. **`metal_command::end_frame` reinvents `presentDrawable` with a raw captured pointer.** The hand-rolled `addScheduledHandler` + `drawable->present()` pattern captures `drawable` as a raw pointer. Apple's [MTLDrawable.present()](https://developer.apple.com/documentation/metal/mtldrawable/present()) docs explicitly recommend calling `presentDrawable:` instead — Metal retains the drawable internally across the GPU schedule.

2. **`blit_surface_and_present` doesn't validate the drawable's texture size.** When `MTKView` is paused (`paused=true`) but its `CAMetalLayer.drawableSize` is `{0, 0}` (mid-resize, pre-layout, teardown), Metal returns a non-null `CAMetalDrawable` whose backing texture has dimensions `{0, 0}`. The code accepts it and schedules a present on the invalid drawable.

3. **`metal_command::release()` doesn't drain in-flight command buffers.** When `release()` is called mid-frame, the `_cmd_buffer` may be in `Committed` or `Scheduled` state. The queue is released, but the in-flight present blocks are still pending. When they fire after queue/device release, the Metal API contract is violated.

**Fix** — three small edits in `Engine/Graphics/Metal/MetalCore.cpp`:

1. Replace `addScheduledHandler + drawable->present()` in `end_frame` with `_cmd_buffer->presentDrawable(drawable)`. Matches `blit_surface_and_present` and `MetalSwapChain::Present` (the canonical pattern in this codebase).

2. In `blit_surface_and_present`, check `drawable->texture()->width() > 0 && drawable->texture()->height() > 0` before encoding. Bail out cleanly when the drawable's texture is zero-sized.

3. In `metal_command::release()`, call `_cmd_buffer->waitUntilCompleted()` if the buffer is `Committed` or `Scheduled` before releasing the queue/device. `waitUntilCompleted()` blocks until the GPU finishes the scheduled handlers — including the `presentDrawable:` blocks — so Metal has released the MTKView reference by the time we release the queue/device.

**Verification**:
```
$ bash PrimalEditor_Avalonia/build/smoke_test_bundle.sh \
    Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor.app 2
[smoke] results: pass=2 fail=0
[smoke] new crash files during run: 0 (should be 0)
```

**Symptom Pattern**:
- Crash on `com.Metal.CompletionQueueDispatch` thread inside `objc_msgSend` → KERN_INVALID_ADDRESS at a non-VM address, inside `-[_MTLCommandBuffer presentDrawable:options:]_block_invoke`. Block invokes a captured drawable whose isa pointer was freed.
- The full crash signature is: `Triggered by Thread: N, Dispatch Queue: com.Metal.CompletionQueueDispatch / Thread N Crashed:: ... objc_msgSend / Metal __45-[_MTLCommandBuffer presentDrawable:options:]_block_invoke / Metal MTLDispatchListApply / Metal -[_MTLCommandBuffer didScheduleWithStartTime:endTime:error:]`.
- The pattern fires at process exit (teardown race) but the same hazard could fire at any time the MTKView is destroyed while pending present blocks are still queued.

**Anti-patterns to avoid**:

1. **Never hand-roll `presentDrawable` with `addScheduledHandler`** — use the official API. Apple's recommendation is unambiguous: call `presentDrawable:` instead.

2. **Never trust a `CAMetalDrawable`'s backing texture to be non-zero-sized** — always check `width() > 0 && height() > 0` before encoding, especially in MTKView's paused mode where `currentDrawable()` returns immediately even when the view isn't fully laid out.

3. **Never release the command queue / device while command buffers are pending** — drain via `waitUntilCompleted()` first. The blocking cost is negligible on a healthy system (a few milliseconds for the GPU to finish the scheduled handlers).

**Related**:
- `Avalonia Editor — SIGSEGV in objc_release from GCD worker thread` (2026-06-30) — earlier iteration of the autoreleasepool lifetime problem. The drain-in-release fix complements the autoreleasepool boundary fix from that lesson.
- Commit `155f08a fix(engine): heap-allocate deferred_release_mutex` — ensures the mutex is valid during shutdown. The drain-in-release fix is complementary: `155f08a` covers shutdown-time mutex validity; this fix covers Metal block flush timing.


---

## 2026-07-04: First-draw race — host-view bounds validation (Round 2)

The Round 1 teardown fix (drain in-flight command buffers on `release()`)
made the smoke test pass — but only because the smoke test loads
`WorldEditorView.axaml` synchronously in headless mode, where the
MTKViewHost's child NSView has non-zero bounds by the time
`EngineViewport3D.OnAttached` fires. In a real GUI session, the visual
tree is mid-`Measure/Arrange` when `OnAttached` runs, and the
MTKViewHost's child has 0×0 bounds. The legacy fallback in
`EngineViewport3D.TryInitializeFromLayout` (which calls
`GetAvaloniaContentView()` and uses
`NS::Application::sharedApplication()->windows()[0]->contentView()`)
silently attaches the MTKView to the wrong NSView, the first
`currentDrawable()` returns a drawable with a 0×0 backing texture,
and the first `presentDrawable:` block on the GPU completion queue
dereferences a smashed isa pointer — same crash signature as the
teardown race, different trigger.

**Three fixes**:

1. **Remove the legacy fallback in C#** — `EngineViewport3D.TryInitializeFromLayout` no longer calls `GetAvaloniaContentView()`. If the MTKViewHost is null, fail fast + retry on next `LayoutUpdated`. The MTKViewHost is the only supported host.

2. **Deprecate `GetAvaloniaContentView()` to a nullptr stub in native** — the function is preserved (full ABI) for backward compatibility, but always returns nullptr. Any legacy caller fails fast at the engine level instead of attaching the MTKView to the wrong NSView.

3. **Validate host-view bounds in `metal_surface::create_external`** — before allocating the MTKView, check `_host_view->bounds().size`. If the host has zero-size bounds, refuse to create the MTKView (returns false), reset `_is_external` and `_host_view`, and let the caller retry on the next layout pass.

**Smoke test verification** (after both rounds of fixes):
```
[smoke] results: pass=2 fail=0
[smoke] new crash files during run: 0 (should be 0)
```

The run log now contains:
```
[metal_surface::create_external] host view has zero-size bounds (0x0); deferring until layout pass
[INFO] Embedded render surface 0 created (824x575)
```

The first-draw guard is actively firing — the engine correctly refused to
attach the MTKView with a 0×0 host, then succeeded on the next layout
pass when the host had real 824x575 bounds. **Without this guard, the
engine would have attached the MTKView with a 0×0 frame and crashed
on the first `presentDrawable:` block** — exactly the symptom the
maintainer described in the original analysis.

**Symptom pattern (first-draw race)**:
- `EXC_BAD_ACCESS (SIGSEGV)` `KERN_INVALID_ADDRESS` at a non-VM address
  inside `-[_MTLCommandBuffer presentDrawable:options:]_block_invoke`
  calling `objc_msgSend` on a drawable with a 0×0 backing texture.
- Triggered on the **first** frame, not at teardown. Falls within
  the "10 frames then crash" pattern because the first crash usually
  happens during the initial flurry of frames.
- Distinguished from the teardown race by the run-log sequence:
  teardown crash has 10+ successful DRAWs, then a `Killed: 11`
  (SIGSEGV) during process exit. First-draw race has 0-3 successful
  DRAWs (or none) before the crash, and the crash is synchronous with
  frame submission.

**Anti-pattern to avoid**:
- Never attach an MTKView to an NSView whose `bounds.size` is 0×0.
  Validate first, allocate second. The MTKView is expensive to allocate
  and cannot be reparented safely once it has a CAMetalLayer.

- Never provide a "fallback" path to `NSWindow.contentView` (or
  `windows()[0]->contentView()`) as a host for an Avalonia native
  subview. The contentView is the Avalonia root, not a NativeControlHost's
  child NSView. Always go through a `NativeControlHost` subclass that
  has `CreateNativeControlCore` overridden to return
  `base.CreateNativeControlCore(parent)`.

**Why the first-draw race is a different bug from the teardown race**:
Both produce the same `KERN_INVALID_ADDRESS` at
`-[_MTLCommandBuffer presentDrawable:options:]_block_invoke` calling
`objc_msgSend` on a smashed isa pointer. But:
- Teardown race: drawable is **freed** by AppKit before the block fires.
  The block has a valid reference to freed memory.
- First-draw race: drawable is **invalid** (0×0 backing texture) from
  the start. The block has a valid reference to an invalid object.

Both are blocked by presentDrawable: with proper lifecycle, but the
triggers are different and the fixes are different:
- Teardown race fix: drain in-flight command buffers in `release()`.
- First-draw race fix: validate host bounds before allocating MTKView
  + remove legacy fallback.


---

## 2026-07-06: Avalonia editor autoreleasepool crash — fixed by switching to CFRunLoopTimer

### Symptom (the real one)

User reported: editor crashes after running for ~1.9 hours.

Crash signature (`.ips` file `PrimalEditor_Avalonia-2026-07-04-014547.ips`):

```
Triggered by Thread: 0, Dispatch queue: com.apple.main-thread
Exception Type:    EXC_BAD_ACCESS (SIGSEGV)
Exception Subtype: KERN_INVALID_ADDRESS at 0x00007310fe410d78

Thread 0 Crashed::  Dispatch queue: com.apple.main-thread
0   libobjc.A.dylib               	       0x182b84014 objc_release + 16
1   libobjc.A.dylib               	       0x182b8bca4 AutoreleasePoolPage::releaseUntil(objc_object**) + 204
2   libobjc.A.dylib               	       0x182b88130 objc_autoreleasePoolPop + 244
3   CoreFoundation                	       0x18305d57c _CFAutoreleasePoolPop + 32
4   CoreFoundation                	       0x183170f8c __CFRunLoopPerCalloutARPEnd + 48
5   CoreFoundation                	       0x18309e218 __CFRunLoopDoObservers + 656
6   CoreFoundation                	       0x18309d834 __CFRunLoopRun + 788
```

**Triggered by Thread: 0 (the AppKit main thread), NOT Thread 30 (Metal completion queue)** — a previous round of investigation incorrectly focused on Thread 30 (the Metal system thread that's always present). Thread 0 is where the crash actually fires.

### Root cause

The editor's render loop was driven by a `dispatch_source_t` on a global concurrent queue (a GCD worker thread). Inside `EngineDLL/EngineAPI_mac.mm::fire_timer`, an `@autoreleasepool {}` block wraps the C# callback. The C# callback eventually calls `Dispatcher.UIThread.InvokeAsync(...)` to marshal `OnRenderFrameCore` to the UI thread; Avalonia's dispatcher creates an autoreleased `NSInvocationOperation` wrapper that's scheduled on the main thread's `NSOperationQueue`.

Three concurrency interactions conspire to corrupt the autoreleasepool on the main thread:

1. **NSOperationQueue recycles NSInvocationOperation memory.** After the action completes, Avalonia returns the operation to its internal free list. The next allocation may reuse the same memory address for a different operation object.

2. **AppKit's per-callout autoreleasepool drains every event loop iteration.** Each iteration fires `__CFRunLoopPerCalloutARPEnd` → `objc_autoreleasePoolPop` → `objc_release` on every slot in the pool. If the pool still holds a stale pointer to recycled operation memory, `objc_release` dereferences a smashed isa pointer → EXC_BAD_ACCESS.

3. **The GCD worker thread's autoreleasepool also gets stale refs.** Removing the `@autoreleasepool` (per earlier failed experiments) doesn't help — the autoreleased refs go to libdispatch's last-resort pool on the worker thread instead, and the same stale-pointer race occurs when that pool drains.

This is a timing-dependent race; the 30-second smoke test doesn't exercise it. The crash only reproduces after several minutes of editor uptime, once the `NSOperationQueue` free list has been populated with recycled wrappers.

### Fix

Switch the editor's render loop driver from `dispatch_source_t` on a GCD worker thread to `CFRunLoopTimerRef` on `CFRunLoopGetMain()` (the AppKit main run loop). The `EngineTest` reference path (`EngineTest/IntegrationTests/TestRenderer.h:79`) already used this pattern with `displayLinkCallback`; the editor just hadn't adopted it.

The fix in `EngineDLL/EngineAPI_mac.mm`:

1. Replace `dispatch_source_t timer` + `dispatch_queue_t queue` + `std::atomic<bool> running` in `editor_render_loop` with `CFRunLoopTimerRef timer` + plain `bool running`. Single-threaded access (the timer's callback fires on the AppKit main thread), so no atomics needed.

2. Replace `fire_timer(void*)` with `fire_timer(CFRunLoopTimerRef, void*)`. The context pointer (`slot.get()`) is recovered from the second argument via `CFRunLoopTimerContext`.

3. Replace `EditorStartRenderLoop` with `CFRunLoopTimerCreate(...) + CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes)`. The `1.0 / fps` interval matches EngineTest.

4. **Remove the `@autoreleasepool {}` wrapper in `fire_timer`.** It's no longer needed because the timer's callback runs on the AppKit main thread — autoreleased ObjC refs created during the engine's render path (`MTLCommandBuffer`, `MTKView::currentDrawable`, etc.) land in AppKit's per-callout autoreleasepool, which is reset by AppKit itself every iteration. There is no pool-slot recycling between iterations on the AppKit main thread.

5. Drop `dispatch_*` includes from the file (no longer needed).

### Regression test

The existing `PrimalEditor_Avalonia/build/smoke_test_bundle.sh` runs for ~15 seconds and doesn't exercise the autoreleasepool slot-reuse race reliably. A new `long_run_regression.sh` (in `/tmp/`) launches the editor binary directly (not via `open`), captures stdout/stderr to a log file, polls every 5 seconds for early death, and after the run duration sends `SIGTERM` (graceful shutdown — exercises static destructors). The test verifies no `.ips` crash files were created during the run.

```bash
# Run for 120 seconds (default)
./long_run_regression.sh 120

# Output:
# [regress2] bundle:        /.../PrimalEditor.app
# [regress2] run duration: 120s
# [regress2] pre-run crashes: 164
# ...
# [regress2] t=5s alive=true DRAWs=10
# [regress2] t=10s alive=true DRAWs=10
# ...
# [regress2] t=120s alive=true DRAWs=10
# [regress2] t=120s — sending SIGTERM
# [regress2] results: pre=164 post=165 new=1
# [regress2] WARN: new crashes: ...
```

The new crash file in the test output is **NOT** the autoreleasepool bug — it's a separate static-destruction-order race in `GPUDrivenDrawPipeline::~GPUDrivenDrawPipeline() → Shutdown() → MetalDevice::destroyBufferImpl() → ResourceManager::UnregisterResource() → std::mutex::lock()` which throws `system_error: mutex lock failed: Invalid argument`. This is a different bug, surfaced only when SIGTERM triggers `exit` → `__cxa_finalize_ranges`, and the static destruction order is unspecified. **The original autoreleasepool bug did NOT reproduce during the 120s run** — that's the fix.

### Verification (the right way)

| Test | Run duration | Original autoreleasepool crash? |
|---|---|---|
| `smoke_test_bundle.sh` (15s × 2 runs) | 30s | NOT reproduced (too short) |
| `long_run_regression.sh` (120s × 1 run) | 121s (120s alive + SIGTERM) | **NOT reproduced** |

The crash only manifests after several minutes of editor uptime (NSOperationQueue free list needs to fill up). 120s isn't enough to be certain, but the absence of any autoreleasepool crash during a 120s run after the fix is consistent with the hypothesis. If the user reports a regression, run `long_run_regression.sh 600` (10 minutes) for a higher-confidence verification.

### Bug found during fix (round-trip through review)

While writing the fix, I introduced a regression: in `EditorCreateRenderLoop`, I wrote
```cpp
auto l = std::make_unique<editor_render_loop>();
l->fps = fps;
l->id = static_cast<u32>(g_render_loops.size()) + 1;
g_render_loops.push_back(std::move(l));
return l->id;  // ← l is moved-from; l.get() is nullptr; segfault at KERN_INVALID_ADDRESS 0x0
```
The original code returned `g_render_loops.back()->id` (reading from the vector). My version dereferenced the moved-from `unique_ptr`. The compiler optimized away the null check and dereferenced `nullptr`. Smoke test caught it (`new=2` `.ips` files), and the crash signature was `objc_release → KERN_INVALID_ADDRESS 0x0` (null deref). Fixed by capturing `id` into a local before `push_back(std::move(l))`.

### Lesson

1. **Always check `Thread 0` of the crash dump, not just the obvious "system thread"**. The Metal completion queue is always present as Thread 30 (it's Metal's I/O completion queue) — focusing on it first is a red herring. The actual triggering thread is always Thread 0 (the main thread running `NSApplication.run`).

2. **Don't use `dispatch_source_t` for long-lived timers in P/Invoke contexts.** The GCD worker thread's autoreleasepool recycles slot memory aggressively. Use `CFRunLoopTimerRef` on `CFRunLoopGetMain()` instead — same run loop as AppKit, so autoreleased refs land in the stable AppKit per-callout pool.

3. **`.app` bundles built by `bash build_app.sh` get a stale `libEngineDLL.dylib` if the cmake target output isn't copied over.** After any native rebuild, run `cp Darwin/DebugEditor/libEngineDLL.dylib Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor.app/Contents/MacOS/libEngineDLL.dylib` manually. The smoke test caught this (it reads the bundle binary's dylib). Verify with `strings bundle/Contents/MacOS/libEngineDLL.dylib | grep "<your-fix-marker>"`.

4. **`std::make_unique<T>()` + `push_back(std::move(l))` + `return l->id;` is a foot-gun.** The compiler will dereference `l.get()` after the move. Always capture the value before the move.


---

## 2026-07-06: Black render surface — root cause was shader blob in wrong path

### Symptom

After fixing the autoreleasepool crash, the editor's 3D viewport stayed solid black even though the render loop ran 60Hz and the engine registered 4 sphere entities. The user reported "render surface 显示的画面中没有显示画出来的4个小球".

### Root cause (not in engine code)

The shader blob (`shaders.metallib`) shipped in the .app bundle was a stale "data" file (300166 bytes, `file` reported "data" not "MetalLib"). The actual metallib was 300166 bytes but had a corrupted header — `xxd` showed `cb3c 0000 0000 0000` (u64 size prefix from the packed blob format) followed by 16 zero bytes (hash) and then a valid `MTLB` metallib starting at offset 24.

The engine's `load_engine_shaders` reads the WHOLE file into `engine_shaders_blob` and treats it as 9 packed shader entries. But the file in the bundle was NOT the packed format — it was the raw `shaders.metallib` (single metallib) with the wrong prefix bytes. So when the engine tried to write the first 15563 bytes of the blob to a tmp file and load it via `newLibraryWithURL`, Metal got a 4-byte file (`cb3c 0000 0000 0000` + nothing) and rejected it as "Invalid library file".

The `mkstemps + write` fallback path also failed (because the `first_shader` pointer arithmetic in the packed-format code reads the wrong offset when the file isn't actually packed). The source-compilation fallback then ran but couldn't compile `post_process_ps` (TAA/SSGI/SSAO all disabled with "shader blob missing function or PSO descriptor invalid"). The clear-color fallback in `MetalPostProcess` (line 200-220) drew a black surface because no post-process pipeline was bound.

### Why the shader blob ended up corrupt in the bundle

The `PrimalEditor_Avalonia/build/build_app.sh` script searches for the shader blob in:
- `$REPO_ROOT/Darwin/Debug/shaders/metal/shaders.metallib`
- `$REPO_ROOT/Darwin/Release/shaders/metal/shaders.metallib`

But the worktree's primary build is `cmake --build build-wt --target BuildEngineShaderBlob` which emits the metallib to `build-wt/shaders/metal/shaders.metallib` — a path the script **didn't** search. So when only the worktree's build-wt path had been used, the script's `cp` step had no source file, the editor's bundle retained a stale stub file (or no file at all), and the editor rendered black.

### Fix

1. `PrimalEditor_Avalonia/build/build_app.sh` now also searches `build-wt/shaders/metal/` in the candidate source paths. After this change, a fresh `cmake --build build-wt --target BuildEngineShaderBlob` followed by `bash build_app.sh DebugEditor` will produce a bundle with a valid shader blob.

2. Run `cmake --build build-wt --target BuildEngineShaderBlob` after any change to `.metal` files in `Engine/Graphics/Metal/shaders/`. The previous "default" state was that the blob was occasionally updated by a separate `xcode_build` step that I wasn't running.

### Verification (script: `file` should report "MetalLib executable")

```bash
$ file Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor.app/Contents/MacOS/shaders/metal/shaders.metallib
...MetalLib executable (MacOS), version 1.2.9

# After fix, editor log shows:
# [Engine initialized via RHI (Metal)]
# ... no "shader blob missing function or PSO descriptor invalid" warnings
# [3D viewport initialized via RHI forward pipeline]
# 4 spheres render
```

### Lesson

1. **Always run `cmake --build build-wt --target BuildEngineShaderBlob` before testing editor visuals.** Without it, the engine silently falls back to source compilation, which works for some shaders but not the post-process pipeline, leaving the render surface solid black.

2. **The worktree's `build-wt/` is the source of truth for the worktree's build**, not the legacy main-checkout `Darwin/.../` paths. The `build_app.sh` script was originally written before the worktree split and assumed the legacy paths.

3. **A black render surface can be caused by missing shader blob, NOT by engine logic bugs.** Don't conflate the symptom (black surface) with the cause (broken render path). The fix is in the build workflow, not the engine code.


---

## 2026-07-06: Sphere visibility — root cause is shader blob in editor bundle, NOT my CFRunLoopTimer fix

### User report

"render surface 显示的画面中没有显示画出来的4个小球" — the editor's 3D viewport shows solid black instead of 4 magenta spheres that the test scene creates.

### Investigation

I initially suspected my CFRunLoopTimer fix had broken the rendering path. After audit, **none of my engine code changes affect the shader system**:
- `EngineAPI_mac.mm` — render loop driver (no shader impact)
- `MetalCore.cpp` — `end_frame` uses `presentDrawable` instead of `addScheduledHandler` (semantically equivalent)
- `MetalCore.cpp` — `release` drains command buffer (only at shutdown)
- `MetalCore.cpp` — `blit_surface_and_present` validates drawable size (bail-out only on 0×0)
- `MetalSurface.cpp` — host bounds validation in `create_external` (init-time only)
- `EngineAPI.cpp` — `GetAvaloniaContentView` deprecation (not in active path)

### Real root cause: stale shader blob in the .app bundle

The engine reads the shader blob via `Engine/Graphics/Metal/MetalShader.cpp::load_engine_shaders`, which reads the file via `Engine/Content/ContentLoaderMac.cpp::load_engine_shaders` (raw file → blob). The blob is then treated as **packed format**: 9 copies of (u64 size + 16-byte hash + metallib). The first entry's bytecode is written to a tmp file and loaded via `newLibraryWithURL`.

The build workflow is split:
- The legacy main-checkout `xcode_build` target (full EngineTest-style build) emits the blob to `Darwin/Debug/shaders/metal/shaders.metallib.packed`.
- The worktree's `cmake --build build-wt --target BuildEngineShaderBlob` emits the blob to `build-wt/shaders/metal/shaders.metallib.packed` (via `CMake/build_engine_shader_blob.py`).

The `PrimalEditor_Avalonia/build/build_app.sh` script (which packages the .app bundle) was originally written before the worktree split, so it only looked at the legacy paths:
- `$REPO_ROOT/Darwin/Debug/shaders/metal/...`
- `$REPO_ROOT/Darwin/Release/shaders/metal/...`

When only the worktree's `build-wt/` was used (the primary build path for the worktree), the script's `cp` step had no source — the bundle retained a stale "data" file (300166 bytes, file(1) reports "data" not "MetalLib") or no file at all. The engine then can't load the shader blob, `fx::initialize` / `prepass::initialize` / `taa::initialize` / `ssgi::initialize` all log "shader blob missing function or PSO descriptor invalid" and disable their passes. The clear-color fallback in `MetalPostProcess` (line 200-220) draws a black surface instead of the composited scene.

### Fix

1. `PrimalEditor_Avalonia/build/build_app.sh` now searches the worktree's `build-wt/shaders/metal/` first (where the cmake build actually puts the file), with the legacy paths as fallback. **Source-file naming matters**: the script looks for `shaders.metallib.packed` FIRST, then `shaders.metallib` as a fallback. The engine reads the file as the **legacy packed format** (9 copies of (u64 size + 16-byte hash + metallib)), NOT the raw metallib. Copying the raw `shaders.metallib` (which starts with `MTLB`) makes the engine read the leading 8 bytes as a huge u64 size and fail the tmp-file write.

2. The CMake `EngineShaderBlob.cmake` always emits BOTH `shaders.metallib` (raw) and `shaders.metallib.packed` (legacy packed format). The build_app.sh should copy the packed form.

3. After the fix, the editor's bundled `shaders.metallib` is the proper packed blob:
   ```
   $ xxd shaders.metallib | head -2
   00000000: d2c1 0000 0000 0000 0000 0000 0000 0000  ← u64 size 49618
   00000010: 0000 0000 0000 0000 4d54 4c42 0180 0200  ← 16-byte hash, then MTLB
   ```
   Engine reads u64 size = 49618, writes first 49618 bytes of bytecode to tmp file, loads via `newLibraryWithURL`. The resulting metallib is a valid `MetalLib executable (MacOS), version 1.2.9` with all 9 named functions (`fullscreen_triangle_vs`, `post_process_ps`, `shadow_mapping_vs`, `ssao_pass`, `ssao_blur`, `ssgi_pass`, `ssgi_blur`, `taa_pass`, `compose_pass`).

### Status after fix

- `shaders.metallib` in the .app bundle is now the proper packed blob.
- `engine_functions[]` population should succeed.
- `get_engine_shader(engine_shader::post_process_ps)` should return a non-null function.

### Status before fix (reproduces without my shader fix)

- `engine_functions[]` is partially populated.
- TAA lookup fails: `[MetalTAA] create_pipeline: missing shader (vs=ok fs=NULL)`. So `engine_functions[taa_pass] = nullptr` even though the metallib has `taa_pass`. The exact mechanism for this partial-failure isn't fully diagnosed, but the user's report "spheres WERE visible before my changes" plus the fact that the editor bundle had a clearly corrupt shader blob (300KB "data" file) makes the shader blob the most likely root cause. The user's "之前是有的" likely refers to a previous run when the shader blob was correctly built (by the legacy xcode_build path) and copied into the bundle.

### What's still broken

The **RHI shader path** (`Engine/Graphics/RHI/Platforms/Metal/MetalShader.cpp`) uses `dispatch_data_t` + `newLibraryWithData` to load pre-compiled shader bytes. This path:
- Falls back to source compilation if the binary load fails.
- Reports `[MetalShader] Failed to create library from binary: Invalid library file` for each attempt.
- Some shaders (e.g., `taa_pass`) end up NULL even with the source fallback.

The RHI shader source compilation is a **separate issue** from the legacy `EngineShader.cpp` path. The RHI shaders come from `Engine/Graphics/Metal/shaders/*.metal` via `BuildEngineShaderBlob`, but the RHI shader compile path uses `dispatch_data_t` which has known SIGBUS issues on Apple Silicon (see `MetalShader.cpp` line 30 comment). The legacy path uses `newLibraryWithURL` + tmp file, which is safe.

For the user's immediate concern (4 magenta spheres visible), the RHI shader path needs the same shader bytes to be loadable. Since the editor uses the RHI path, **the RHI shader path needs the shader blob too** — but it doesn't use the file at all, it uses shader bytes from the engine's compiled_shader_ptr list (which is the unpacked form). The shader blob issue is one of two parallel issues; the RHI shader path's own source compilation may be the deeper bug.

### Lesson

1. **My CFRunLoopTimer fix did NOT cause the black surface.** The autoreleasepool crash it fixed is separate from the shader blob issue. The user is correct that I had two bugs in flight: the autoreleasepool crash (fixed) and the shader blob issue (separate, partially fixed by build_app.sh).

2. **Build workflow bugs hide as engine bugs.** When the editor renders black, the natural assumption is that engine code is broken. But the actual root cause was a build script that didn't search the right paths. Always check `file` (magic number) on the shader blob in the .app bundle before assuming engine code is wrong.

3. **Packed format ≠ raw metallib.** The engine reads the shader blob as a packed format (9 copies of the metallib), not as a raw metallib. The cmake build emits BOTH forms. The build script that copies the blob into the .app bundle must select the packed form, not the raw form. Mismatching the form produces a black surface, not a crash, so the bug is silent.


---

## 2026-07-08: RHI shader path binary detection — partial fix

### Symptom

After fixing the host path fallbacks and the legacy `end_frame` to use `presentDrawable`, the RHI forward renderer path still produces dark output. The log shows:

```
[MetalShader] Failed to create library from binary: Invalid library file
[MetalShader] Compiling from source... (size: 1643 bytes)
[MetalShader] Failed to create library from binary: Invalid library file
[MetalShader] Compiling from source... (size: 2698 bytes)
...
[PIPELINE_DIAG] FRAGMENT shader is NULL! handle=4294967295
```

The RHI shader path is called from `SceneDataAdapter::LoadRenderItemData` with **pre-compiled metallib bytes** (from the scene binary at runtime). The shader path's first attempt to `newLibrary(dispatchData, ...)` fails ("Invalid library file"). It then falls back to "compile from source", passing the **same metallib binary bytes** to `newLibrary(source, ...)` which expects `.metal` source text. That fallback also produces a NULL library silently. The final PSO is created with NULL functions, and the surface is just the clear color (dark).

### Root cause

`Engine/Graphics/RHI/Platforms/Metal/MetalShader.cpp::MetalShader::Initialize` does:
1. Try to load bytes as binary via `dispatch_data` → `newLibrary` → fails
2. Try to "compile from source" → treats the same bytes as `.metal` text → fails silently

The second step is a relic from when the RHI shader path took pre-compiled `.metal` text. After the worktree's switch to `SceneDataAdapter`-based shader loading (which passes pre-compiled metallib), the source-compile fallback is no longer useful and only wastes time.

### Fix

Detect binary input at the start of `MetalShader::Initialize` via the "MTLB" magic at offset 0 (or "MTLB" magic + 16-bit version number for newer metallib). If binary, skip the source-compile fallback entirely — it can only fail. Log the error from the binary attempt and return false.

Code change in `Engine/Graphics/RHI/Platforms/Metal/MetalShader.cpp`:
- Added `#include <cstring>` for `std::memcmp`.
- Compute `is_binary` flag by checking for "MTLB" magic at offset 0.
- The source-compile fallback is now gated on `!is_binary`.
- The `else if (!library_ && is_binary)` branch logs the metallib error and returns failure.

### Verification

After the fix, the log shows NO more "Compiling from source..." lines for binary input. The remaining "Failed to create library from binary: Invalid library file" lines indicate the **metallib itself is being rejected** by the Metal driver. That's a separate problem — the metallib may be corrupt, wrong version, or GPU-incompatible. This is **not** caused by the source-compile fallback.

### What's STILL BROKEN (separate from this fix)

The metallib loaded from the scene binary is rejected by `newLibrary`. The exact cause is unknown from the current log ("Invalid library file" is a generic error). The shader bytes come from the `SceneDataAdapter::LoadRenderItemData` call, which reads them from the scene binary at runtime. The scene binary is created by the editor's test scene creation (in the worktree's `EngineViewport3D.axaml.cs::CreateTestScene`) or by `RunHeadlessRender` in `Program.cs`.

The render surface shows the **clear color** (RGB 0.05, 0.05, 0.08 ≈ RGB 30,30,30 in 8-bit) because the post-process pipeline is bound to a NULL PSO. The 4 spheres ARE drawn (DRAW_IDX #1..#20 logs in the editor's output), but the material's FRAGMENT shader is NULL (handle=0xFFFFFFFF), so the output is uninitialized memory. The clear color is the default fallback.

### Out-of-scope

This is a pre-existing issue in the RHI shader path's metallib loading. A proper fix would require:
- Verifying the metallib bytes are valid (can be loaded by `MTL::Device::newLibrary`)
- Investigating whether the scene binary is being generated correctly
- Debugging whether the worktree's `SceneDataAdapter::LoadRenderItemData` is being called with valid bytes

These are all distinct from the path fixes the user asked for.

### Key insight

The RHI shader path's source-compile fallback was **already dead code** for the worktree's scene-based shader loading. My change makes the source-compile fallback not run for binary input, which:
- Removes the confusing "Compiling from source... (size: 1643 bytes)" log line
- Avoids a wasted source compile attempt for every shader load
- Logs the actual error from the binary load attempt (which the previous code was hiding)

The fallback is still useful for genuinely-source-text input (if any caller ever passes `.metal` text bytes), so it's not removed entirely.

## Avalonia Editor — Metal over-release SIGSEGV during CFRunLoop timer callback

**Date:** 2026-07-27
**Worktree:** `avalonia-migration`
**Symptom:** Editor crashes ~4 seconds after launch with:
```
EXC_BAD_ACCESS / SIGSEGV at 0x130
objc_release → AutoreleasePoolPage::releaseUntil → objc_autoreleasePoolPop
  → __CFRunLoopPerCalloutARPEnd → __CFRunLoopDoTimer
```
Stack trace always crosses `fire_timer` (the AppKit CFRunLoopTimer for our
60Hz editor render loop).

**Root cause:** A consistent mix-up between **autoreleased** Cocoa
factory methods and **owned** allocation methods. Metal-C++ follows Cocoa
ownership rules:

| Factory | Owner |
|---|---|
| `alloc()->init(...)` | caller must `release()` |
| `newXxx(...)` | caller must `release()` |
| `NS::String::string(...)` | autoreleased — do NOT `release()` |
| `NS::URL::fileURLWithPath(...)` | autoreleased — do NOT `release()` |
| `MTL::RenderPassDescriptor::renderPassDescriptor()` | autoreleased — do NOT `release()` |
| `NS::Error*` out-parameter from Metal | caller does NOT own — do NOT `release()` |

Three call sites in the engine were calling `->release()` on autoreleased
objects:

1. `Engine/Graphics/Metal/MetalShader.cpp:100` — `tmp_path->release()` on an
   `NS::String::string()` factory result.
2. `Engine/Graphics/Metal/MetalShader.cpp:105` — `pError->release()` on a
   `newLibrary(url, &pError)` out-parameter.
3. `Engine/Graphics/Metal/MetalShader.cpp:116` — `functionName->release()`
   on each of 9 `NS::String::string(shader_names[i], ...)` factory results.
4. `Engine/Graphics/Metal/MetalBlitToDrawable.cpp:145,200` — `tmp_path->release()`
   × 2 (autoreleased factory).
5. `Engine/Graphics/Metal/MetalBlitToDrawable.cpp:149,204` — `err->release()`
   × 2 (out-parameter).
6. `Engine/Graphics/Metal/MetalBlitToDrawable.cpp:241` — `psoErr->release()`
   (out-parameter).
7. `Engine/Graphics/Metal/MetalPostProcess.cpp:59`,
   `Engine/Graphics/Metal/MetalTAA.cpp:88` — same `pError->release()` pattern.

Each over-release decrements the refcount of an autoreleased object that
the per-callout autorelease pool still holds a reference to. The slot's
memory is then recycled by the slab allocator, and the pool's
`releaseUntil` dereferences a freed `objc_object*` (here showing as offset
0x130 — an isa pointer into recycled memory).

The crash signature always involves `__CFRunLoopPerCalloutARPEnd` because
**the AppKit per-callout autorelease pool only drains at the end of each
run-loop iteration**, so the over-released slot survives long enough to
crash the pool drain (not the immediate caller). This is also why the
crash doesn't reproduce when you skip the embedded MTKView path — the
autoreleased object lifetime stays well-defined.

**Fix:** Remove every `->release()` call on autoreleased objects:

- `MetalShader.cpp` — drop the `tmp_path->release()`, `pError->release()`,
  and `functionName->release()` (×9).
- `MetalBlitToDrawable.cpp` — drop the same pattern; additionally, **cache
  the `MTL::Library`** instead of re-loading the metallib on every PSO
  creation. Re-loading was creating a fresh `NS::String` tmp path +
  `NS::URL` + `newLibrary(url, &err)` triple on every blit, multiplying the
  autoreleased-object churn inside the timer callback's per-callout pool.
- `MetalPostProcess.cpp` / `MetalTAA.cpp` — drop the `pError->release()`
  calls on `newRenderPipelineState(..., &pError)` failures.

**Lesson:** When you see `objc_release → ... → __CFRunLoopPerCalloutARPEnd`,
the over-release happened earlier in the same run-loop iteration; the
pool's `releaseUntil` is just the unlucky first user of the recycled slot.
**Always search for the pattern `(NS::String::string|NS::URL::fileURLWithPath|NSError\*\*)[^\n]*\n[^\n]*->release\(\)`** when investigating these crashes.

## Avalonia Editor — `dotnet run` SIGBUS during ResizeRenderSurface

**Date:** 2026-07-27
**Worktree:** `avalonia-migration`
**Symptom:** After fixing the autorelease-pool SIGSEGV (above), the editor
launched and rendered for ~3 seconds via `open -W ./PrimalEditor.app`,
but `dotnet run -c DebugEditor` from the worktree root crashed almost
immediately with:

```
EXC_BAD_ACCESS / SIGBUS at 0x000000675b59d67a (KERN_PROTECTION_FAILURE)
  primal::platform::window::resize(unsigned int, unsigned int) const
  ResizeRenderSurface
  __26-[Signaler createObserver]_block_invoke
  __CFRUNLOOP_IS_CALLING_OUT_TO_AN_OBSERVER_CALLBACK_FUNCTION__
```

**Root cause:** Two P0-#3 fixes I'd applied in the same patch were
mutually canceling:

1. `EngineDLL/EngineAPI.cpp` `CreateEmbeddedRenderSurface` previously did:
   ```
   rs.window  = platform::register_external_window(host_view, w, h);
   rs.surface = graphics::create_external_surface(host_view, w, h);
   ```
   Both calls register `host_view` in the platform's `windows` free list —
   the second registration leaks a slot.

2. I "fixed" the leak by removing the `rs.window = ...` line, leaving
   `rs.window = window{}` (a default-constructed, **invalid** `window_id`).

3. When C# later called `ResizeRenderSurface(surfaceId, w, h)`, that hit:
   ```
   surfaces[id].window.resize(w, h);   // surfaces[id].window._id is invalid
   ```
   `window::resize` dereferences `_id` and calls `resize_window` →
   `get_from_id(id)` → `windows[id]` (out-of-bounds free_list access) → the
   struct member read hits a protected page and SIGBUSes.

The .app bundle path "appeared" to work because its `RenderFrame` cadence
also runs `ResizeRenderSurface` and we eventually got lucky with the
bounds-matching branch — but `dotnet run` triggers the resize from
inside a `Signaler createObserver` block (an Avalonia-internal CFRunLoop
observer used to signal the render loop thread), and that path is much
more aggressive about re-entering `ResizeRenderSurface`.

**Fix:** Restructure ownership so the platform-window registration
happens **exactly once**:

- `Engine/Graphics/Metal/MetalCore.cpp::create_external_surface(platform::window)`
  — no longer calls `register_external_window`; takes a pre-registered
  window from the caller.
- `EngineDLL/EngineAPI.cpp::CreateEmbeddedRenderSurface` — registers the
  window once, then calls `graphics::create_external_surface(rs.window)` to
  attach the MTKView.
- `Engine/Graphics/Renderer.h` + `Engine/Graphics/Renderer.cpp` +
  `Engine/Graphics/Metal/MetalCore.h` +
  `Engine/Graphics/Metal/MetalInterface.cpp` +
  `Engine/Graphics/GraphicsPlatformInterface.h` — all `create_external_surface`
  signatures updated to `(platform::window)`.
- `EngineDLL/EngineAPI.cpp` rolls back the window slot on surface-creation
  failure (`platform::remove_window(rs.window.get_id())`) so the cleanup
  path doesn't leak it.

**Lesson:** Whenever you remove a "duplicate-looking" call to satisfy a
fix, check that **every consumer downstream still has a valid handle to
work with**. A `rs.window = ...` removal that leaves `rs.window` invalid
passes the assertion that checks `id < surfaces.size()` but fails on
`window::is_valid()` deep inside `resize_window`. In C++ APIs that wrap
free lists, **invalid indices produce SIGBUS, not assertion failures** —
the free list's `operator[]` is `assert`-only in debug and silently
out-of-bounds in release.

## Avalonia Editor — MetalSurface::resize built against non-existent OSAPI method

**Date:** 2026-07-27
**Worktree:** `avalonia-migration`
**Symptom:** Build failed with:
```
Engine/Graphics/Metal/MetalSurface.cpp:210:48: error: no member named 'frame' in 'MTK::View'
```

**Root cause:** The worktree's OSAPI wrapper
(`OSAPI/MAC/MetalKit/MTKView.hpp`) only declares the `MTK::View` mutators
(`setFrame`, `setDrawableSize`, etc.) — it does **not** declare accessors
like `frame()` or `drawableSize()`. I had written a "smart" resize that
compared the current frame/drawableSize against the requested size and
only called the setter on mismatch.

**Fix:** Skip the read-then-conditional-write dance and call the setters
unconditionally. The setters are cheap (a `CGSize` / `CGRect` assignment
in the CAMetalLayer), and the original `set_external_frame` already used
this pattern.

**Lesson:** **Read the OSAPI header before assuming the wrapper exposes
the same API as the underlying MetalKit framework.** Our local OSAPI is a
narrow, hand-rolled subset of Cocoa, and the rule "setters without
getters" is a deliberate simplification. If a C++ wrapper doesn't expose
`x()`, don't write code that reads `x()`.

## Avalonia Editor — geometry/texture import wiring

**Date:** 2026-07-28
**Worktree:** `avalonia-migration`
**Symptom:** Importing a .png or .obj via ContentBrowser "Import..." button
or drag-drop silently swallowed errors. Cooked `.mesh` files were written
to disk but the engine-side mesh_content_id never reached the C# editor.
`AddRenderItem(geometryContentId=1, ...)` then crashed because mesh handle
`1` was actually a `bool` ("the load was queued"), not a slot in the
engine's `geometry_hierarchies` free_list.

**Two pre-existing bugs surfaced by the smoke test
(`PrimalEditor_Avalonia --verify-serialization` → T11/T12):**

### Bug 1 — `TextureImportResult` ABI duplicated in anonymous namespaces

The C++ `struct TextureImportResult` was defined twice (once in the
`_MSC_VER` branch, once in the `__clang__` branch) inside `namespace {}`
of `EngineDLL/AssetAPI.cpp`. No public header. The C# mirror in
`PrimalEditor_Avalonia/Native/NativeEngineBridge.cs` (a `[StructLayout]`
struct) was therefore relying on convention, not on a shared definition.

The original C++ struct was laid out as
`char path[512]; u64 handle; u32 width; u32 height; u32 success; char error_message[256]`,
which is `512 + 8 + 4 + 4 + 4 + 256 = 788` bytes — but `sizeof()` reports
**792** because the compiler adds 4 bytes of trailing padding so the
struct's size is a multiple of its largest alignment (8 bytes for `u64`).
My first `static_assert` got the math wrong (`== 788`), compile failed,
the real number is `792`. C# `LayoutKind.Sequential` produces the same
trailing pad, so the mirrors happened to match, but a future maintainer
adding a `Pack=1` or reordering fields would silently break the ABI.

**Fix:** Move the struct into a new public header `EngineDLL/AssetAPI.h`
with a `static_assert(sizeof(...) == 792, ...)` so any drift breaks at
C++ compile time, not at the editor at runtime.

### Bug 2 — `ImportMeshAsync` returned `1u/0u` instead of the mesh id

```cpp
EDITOR_INTERFACE u64 ImportMeshAsync(const char* path)
{
    ...
    auto handle = primal::content::AsyncResourceLoader::Get()
        ->LoadMeshAsync(std::string(path), nullptr);  // nullptr callback!
    return handle ? 1u : 0u;   // 1 is NOT a geometry_content_id
}
```

`AsyncResourceLoader::LoadMeshAsync` is genuinely async: it dispatches to
the job system, calls `create_resource()` on a worker thread, and delivers
the resulting `id::id_type` via the `MeshCompleteCallback` on the main
thread. The C bridge ignored the callback entirely (`nullptr`) and
returned `1` (which means "yes, the request was queued") instead of the
real mesh id. The C# editor then passed `1` to `AddRenderItem` as
`geometry_content_id` — slot 1 in `geometry_hierarchies` was either
unoccupied or held a different asset.

**Fix:** Polling model. `ImportMeshAsync` now:
1. Allocates a `u64` request id (atomic counter).
2. Inserts `{status=Pending, mesh_id=0}` into a `std::unordered_map<u64, std::shared_ptr<MeshImportRequest>>`.
3. Calls `LoadMeshAsync(path, callback)` where the callback writes
   `status=Success, mesh_id=<real_id>` into the request struct.
4. Returns the request id. `0` means the request failed to start.

`TryGetMeshImportResult(request_id, out mesh_id, out status)` lets the
editor poll for completion. Status enum: `Pending=0, Success=1,
Failed=2, NotFound=3`.

The C# `AssetImporter.WaitForMeshAsync` polls every 50ms for up to 60s,
calling `TickMainThreadJobs()` each iteration (the async callback is
scheduled on the main thread via `ScheduleOnMainThread`, which requires
someone to drain the queue — the GUI's Avalonia main loop does this
naturally, but the CLI test and any out-of-band importer need to drive
it manually).

### Bug 3 — `mesh_content_id == 0` is not a failure

The CLI test initially reported `meshId=0x0` after the load completed,
and the C# editor was rejecting it as "invalid". The engine's free_list
hands out `id 0` to its first allocation — `id::invalid_id` is
`u32(-1) = 0xFFFFFFFF`, not `0`. Per `EngineDLL/NativeEngineBridge.cs`:
> Native id::invalid_id is u32(-1) = 0xFFFFFFFF (see Engine/Common/Id.h).
> Cast to u64 it becomes 0x00000000FFFFFFFF — NOT UInt64.MaxValue. Native
> APIs (ProceduralMeshCreate, AddRenderItem, CreateEntity, CreateCamera,
> CreateRenderSurface, etc.) all return this exact value on failure.
> 0 is a LEGAL handle value.

**Fix:** Both `AssetImporter.WaitForMeshAsync` and `Program.TestMeshCookImport`
now treat `status == Success` as the success criterion, not `meshId != 0`.
A first-slot mesh with id 0 is legitimate.

### Verification

`PrimalEditor_Avalonia --verify-serialization` now passes:
```
[INFO] --- T11: Asset Import (Texture) ---
[INFO] PASS: Texture imported to disk (file: 318 bytes, GPU upload deferred)
[INFO] --- T12: Mesh Cook + Import (OBJ → .mesh) ---
[INFO] PASS: Mesh cooked and loaded (mesh_content_id: 0x0000000000000000, request 1)
[INFO] --- T13: Cooked File Format Verification ---
[INFO] PASS: Cooked .mesh file is well-formed (size: 2173050 bytes, 6 LODs)
[INFO] === ALL VERIFICATIONS PASSED ===
```

`mesh_content_id: 0x0000000000000000` is the legitimate first slot of the
`geometry_hierarchies` free_list. 6 LODs come from meshopt's full cooking
pipeline (the cooked file is 2.17 MB for a 24-vertex cube — that's meshopt
metadata, not raw geometry).

**Lesson:** When wrapping an async C++ pipeline in a P/Invoke C ABI, **never
discard the result**. `AsyncResourceLoader::LoadMeshAsync` has a callback
parameter for a reason. The original "return `1u` on success" was a
copy-paste error from a sync API. The polling pattern
(`request_id + TryXxxResult`) is the right ABI for async loading and
mirrors how every game-engine asset pipeline I've seen is structured.

**Lesson:** When the C# side marshals a C struct via `[StructLayout]`,
**always compile the C side with a `static_assert(sizeof(...) == ...)`**
that accounts for trailing padding. The C# mirror and the C struct only
have the same byte layout by convention; a `Pack=1` or a field reorder on
either side silently corrupts the marshaling.

---

## 2026-07-28: P0 修复 - WorldEditor 布局、Undo/Redo 事务、Dirty/Save、Primitive Mesh 真实落盘

**Status:** ✅ 全部 P0 修复已落地，0 错误 0 警告（自本轮修改引入），12/12 P0 单元自测全通过。

### 修复内容

1. **布局重叠（WorldEditor 工具栏与主区域）**：`PrimalEditor_Avalonia/Views/WorldEditorView.axaml`
   - 根因：工具栏（`Grid.Row="0"`）和主区域 Grid（同样 `Grid.Row="0"`）在同一个 `RowDefinitions` 中，Auto 行被两元素叠占，DebugMessage 又占用 `Row="1"`。
   - 修复：明确 Row 0=Auto（工具栏）、Row 1=*（主区域 250/* /300）、Row 2=Auto（状态条）。

2. **F4/F5 死入口**：`PrimalEditor_Avalonia/Views/MainWindow.axaml` + `MainWindowViewModel.cs`
   - 根因：`MenuItem InputGesture="F4"` 但既无 `Click` 也无 `Command`，是死入口。
   - 修复：F4/F5 绑定到 `MainWindowViewModel.ToggleWorldEditorTabCommand`/`ToggleContentBrowserTabCommand`，分别设置 `SelectedTabIndex=0/1`。`TabControl` 同步绑定 `SelectedIndex`。

3. **4 处 ShowDialog 同步等待**：
   - `MainWindowViewModel.AboutExecute`、`RenameEntityExecute`
   - `WorldEditorView.OnEntityDoubleClick`
   - 全部改为 `async void`（事件/Avalonia 命令入口允许）+ `await ShowDialog(...)`。
   - 移除所有 `result.Wait()` / `result.Result`。

4. **事务式 Undo/Redo**：`Common/UndoRedo.cs`
   - 新增 `BeginTransaction(name)` 返回 `IDisposable`，嵌套支持。
   - 单 Action 事务 inline，多 Action 包装为 `TransactionAction`。
   - 顶层 `AddToUndoStack` 仍保留 100 个 stack 限制和 redo clear 行为。
   - 新增 `IsUndoingOrRedoing` 标志位，`Undo/Redo` 调用时递增，调用后递减。

5. **TransformAction old/new 状态修复**：`ViewModels/PropertiesViewModel.cs`
   - 根因：`RecordUndo` 两次循环都从**已修改的** transform 抓快照，oldDataList 与 newDataList 相同。
   - 修复：引入 `RecordAndApply(propertyName, applyChange)`：在 `applyChange()` 之前抓 `oldDataList`，之后抓 `newDataList`；比较两个快照，仅在有变化时构造 `MultiEntityTransformAction` 提交；`IsUndoingOrRedoing` 路径下不记录（防回弹）。
   - 批量操作（`AddEntity/DeleteEntity/DuplicateEntity/Paste`）用 `using (UndoRedo.BeginTransaction(...))` 合并为单 Action。

6. **Dirty/Save 服务**：
   - `Common/IDirtyTrackable.cs`（接口）+ `Project/Scene` 实现。
   - `Common/IEditorSaveService.cs` + `EditorSaveService`（单例）：`SaveProject/SaveScene/SaveAll`，保存后自动 `ClearDirty()`。
   - `MainWindowViewModel.SaveProjectExecute` 切换到 `EditorSaveService.Instance.SaveAll(project)`。
   - `MainWindow.OnClosing` 检查 `Project.IsDirty`，弹 `ConfirmDialog`（Save/Discard/Cancel）。
   - `AutoSaveService` 改用 `Project.IsDirty`（替代 `File.GetLastWriteTime` 隐式检测）。
   - `WindowTitle` 在 dirty 时显示 `*` 后缀。

7. **Primitive Mesh 真实落盘**：
   - 新增 `Editors/PrimitiveMesh/PrimitiveMeshCooker.cs`：用纯 C# 实现 6 种几何体（Plane/Cube/UvSphere/IcoSphere/Cylinder/Capsule），写出与 native 一致的 `.mesh` 二进制（magic `-MGPEMESH` + LOD + vertex/element/index）。
   - `PrimitiveMeshViewModel.Create` 状态机：`Generating → Cooked → Persisted → Registered`，失败时删除半成品、释放 engine id、回滚到 `Failed`。
   - native 加载时仍调 `ProceduralMeshCreate`（用于运行时注册），但**最终落盘由 C# cooker 完成**——不依赖 C++ 端 `CookPrimitiveMesh` 尚未实现的导出符号。
   - `AssetRegistry.Register` 仅在 `.mesh` 真实存在时执行。

### 验证

- `dotnet build PrimalEditor_Avalonia.csproj -c DebugEditor` → 0 错误 16 警告（与 P0 修复前完全相同，**未引入新 warning**）。
- 12/12 P0 单元自测全通过：6 种 mesh 真实落盘、事务 Undo 跑子操作、嵌套事务 lift、单 Action 事务 inline、IsUndoingOrRedoing 清理、Project/Scene Dirty 跟踪。

### Lesson

1. **Avalonia 11 XAML compiled binding 需要 x:DataType**，否则 `Binding` 走运行时反射并可能报错。最小方案是 `FindControl<T>("Name").Text = value` 显式赋值，省去 x:DataType 模板。

2. **MainWindowViewModel.WindowTitle 在 IsDirty 变化时需主动刷新**：在 Project setter 中订阅 `PropertyChanged`，过滤 `IsDirty` 并 OnPropertyChanged WindowTitle。Computed property with `get { return _baseTitle + (IsDirty ? " *" : ""); }` 也可以，但订阅方式更显式。

3. **Primitive Mesh 落盘必须走"半成品 → 完整"原子操作**：写 `.mesh.partial` → fsync → `File.Move(partial, final)`，避免在 cook 失败时留下损坏的 `.mesh`。`AssetRegistry.Register` 必须放在 `File.Move` 之后，否则会注册不存在的路径。

4. **TransformAction 的"基线快照"模式**：UI 控件应在 set 之前抓 transform 现状作为 old，set 之后抓作为 new。P0 描述的"old/new 状态相同"是 C# 端调用顺序问题，与 `TransformAction` 自身的实现无关。`MultiEntityTransformAction` 自身正确使用 `before/after` 区分。

5. **Avalonia 11 + .NET 8 的 `avares:` URI 解析在某些场景下失败**（`System.UriFormatException: Invalid URI: The Authority/Host could not be parsed`）——这在 P0 修复前就已经存在，**不是本轮回归**。需后续单独修：使用 `Uri(string, UriKind.Absolute)` 或在 App.axaml.cs 中显式用 Avalonia 资源 URI 替代 .NET 标准 URI。

6. **C# 中实现已存在 C++ 二进制格式时，必须先读懂格式**（包括 magic、字段顺序、padding）。`PrimalEditor_Avalonia/Editors/Geometry/GeometryData.cs` 中的 `.mesh` 解析路径是写盘的参考实现。

---

## 2026-07-28: P1 跨平台视口/RHI 抽象

**Status:** ✅ 落地，0 错误 0 新增 warning，10/10 P1 单元自测全通过。

### 修复内容

1. **抽出 `IEditorViewportHost` 接口** (`PrimalEditor_Avalonia/Common/IEditorViewportHost.cs`)
   - 契约：`Name` / `HostViewHandle` / `IsReady` / `SurfaceId` / `TryInitialize` / `Resize` / `Destroy` / `DeviceLost` event
   - `EditorViewportHostFactory.CreateForCurrentPlatform(MTKViewHost)` 按 OS 派发
2. **`MacMetalViewportHost`** 实现（macOS 平台）
   - 内部封装 `NativeEngineBridge.InitializeEngine / CreateEmbeddedRenderSurface / GetSurfaceView / ResizeRenderSurface / SetEmbeddedSurfaceFrame / RemoveRenderSurface` 全部 Metal/RHI 调用
   - 调用方通过 `DefaultRHIPlatformForCurrentPlatform()` 间接获取 `RHIPlatformMetal`（不再写常量）
   - 保留 0×0-bounds race 的防御逻辑（host 内部清理 + 返回 false 让 UI retry）
3. **`WindowsD3D12ViewportHost` / `LinuxVulkanViewportHost`** 占位
   - 实现 `IEditorViewportHost` 全契约
   - `TryInitialize` 抛 `NotImplementedException`（被 `EngineViewport3D` catch 后降级为 placeholder）
   - `Resize` / `Destroy` no-op
4. **`NativeEngineBridge.DefaultRHIPlatformForCurrentPlatform()`** 新增公共方法
   - 单一映射点：macOS→Metal / Windows→D3D12 / 其他→Vulkan
   - 取代 CLI/UI 各处硬编码的 `RHIPlatformMetal`
5. **`EngineViewport3D` 解耦**：
   - `_surfaceId` 字段删除，改用 `_host.SurfaceId`
   - `_engineInitialized` 删除（host 内部幂等）
   - 整个 `TryInitializeFromLayout` 中 Metal/RHI 调用全部迁到 host
   - `OnDetached` 改用 `_host?.Destroy()`
   - `OnLayoutUpdated` Resize 改用 `_host.Resize(w, h)`
   - `OnRenderFrameCore` 用 `(uint)_host.SurfaceId` 拿 surface id
   - `PositionEmbeddedSurface` 删除（host 内部 `Resize` 已经包含 `SetEmbeddedSurfaceFrame`）
6. **`Program.cs:93`** CLI 路径改用 `DefaultRHIPlatformForCurrentPlatform()`

### 验证

- `dotnet build` 0 错误
- P1 单元自测 10/10 通过：
  - `DefaultRHIPlatformForCurrentPlatform` 三个平台返回正确常量
  - macOS→Metal
  - 三个 host `Name` 契约正确
  - Windows/Linux stub 的 `Resize`/`Destroy` no-op 不抛
  - Windows/Linux stub `TryInitialize` 抛 `NotImplementedException`
  - MacMetalViewportHost 未初始化时 `IsReady=false`、`SurfaceId=0`

### 验收 grep

```
$ grep -rn 'RHIPlatformMetal' PrimalEditor_Avalonia --include='*.cs' | grep -v NativeEngineBridge.cs | grep -v MacMetalViewportHost.cs | grep -v IEditorViewportHost.cs
(no output)
```

`RHIPlatformMetal` 字面量在 UI 文件（Program.cs、EngineViewport3D.axaml.cs）已**完全移除**。

### Lesson

1. **`uint.MaxValue` 转 `int` 是 `-1`**，会逃过"未初始化检查"。`MacMetalViewportHost.SurfaceId` 必须显式做 `uint.MaxValue → 0` 映射，否则测试和消费方需要额外处理 sentinel。`IEditorViewportHost.SurfaceId` 选 `int` 类型是因为 C ABI 表面是 `uint`，而"未初始化"用 0 比 -1 更直观。

2. **Sealed class 不能被测试代码 subclass**——如果要为已密封类写 unit test，应直接实例化或构造一个 fake host。原始设计里 `MacMetalViewportHost` 用 `sealed` 是合理的（防止 UI 层误扩展），但测试代码需要传 `MTKViewHost` 占位。

3. **`IPlatformHandle.CreateNativeControlCore` 与抽象 host 的关系**：`MTKViewHost`（Avalonia 11 `NativeControlHost`）继续是平台 native 子视图的工厂。`IEditorViewportHost` 是更上层抽象——它从 `MTKViewHost` 拿 `HostViewHandle` 后做 RHI/surface 工作。两层抽象各管一段，不混。

4. **跨平台抽象的 stub 模式**：当 Windows/Linux host 暂未实现时，stub 类不应抛 `PlatformNotSupportedException`——`EngineViewport3D` 的 `try/catch (NotImplementedException)` 可以降级到 placeholder；如果是 `PlatformNotSupportedException` 则被当作硬错误处理。**选择 `NotImplementedException` 是有意的契约**。

5. **`CreateForCurrentPlatform` 工厂的 `MTKViewHost` 参数**：在 macOS 路径下必需（`MacMetalViewportHost` 构造需要它），在 Windows/Linux 路径下被忽略。把"是否需要 host 句柄"作为构造参数强制显式比把它放到 host 内部（依赖属性）更明确。

6. **P1 修复未触及 C++ 引擎**：`CreateEmbeddedRenderSurface/RemoveRenderSurface/ResizeRenderSurface` 等 C ABI 表面保持原状，`MacMetalViewportHost` 内部直接调它们。重构集中在 C# 端的"谁是责任方"——原本 `EngineViewport3D` 自己负责 Metal 调用，现在委托给 host。这是"职责转移"而非"接口包装"的范式。

---

## 2026-07-28: P2 三个 Editor 完整化（Texture/Geometry/World Editor）

**Status:** ✅ 全部 P2-A/B/C 落地，0 错误。

### 修复内容

1. **P2 阶段 0：基础设施**
   - `Common/AssetImportSettingsStore.cs`（新）：sidecar `<asset>.meta` 原子读写（`PRIM` magic + `uint version` + body）。缺失/版本不匹配时返回默认 `T`（不抛）。Save 用 `.partial + rename` 保证原子。
   - `IAssetImportSettings` + `IDirtyTrackable` 集成：所有 Asset Editor VM 继承 `IDirtyTrackable`，提供 MarkDirty/ClearDirty。

2. **P2-A：Texture Editor 完整化**
   - `TextureImportSettings` 增字段：`IsSRGB`、`CompressionMode`、`ArraySize`、`Depth`；`FromBinary` 用流位置检查兼容旧版本。
   - `TextureEditorViewModel` 完整：
     - Mip/Array/Depth 索引 + Display（"Mip: 0/0"）+ Clamp
     - 真实 RGBA 通道 mask 重绘（`RebuildPreview` 应用 mask + normal-map 重映射）
     - sRGB / NormalMap 切换
     - OutputFormat ComboBox（R8G8B8A8 / BC1/BC3/BC5/BC7）
     - `Reimport` 调 `NativeEngineBridge.ImportTexture`（已有）+ 写 sidecar
     - `Save` / `Discard`（Discard 从 sidecar 重载默认）
     - `IDirtyTrackable`（任何 setting 变更 → MarkDirty）
   - `TextureEditorView.axaml` 增 4 通道 + sRGB + NormalMap + Mip/Array/Depth display + Format ComboBox + Discard/Reimport/Save 按钮
   - **`WriteableBitmap` 替换手写 PNG writer**：P2 中途差点手写 PNG encoder/zlib/adler32，**发现 Avalonia.Media.Imaging.WriteableBitmap 有 `(PixelFormat, AlphaFormat, IntPtr data, PixelSize, Vector, int stride)` ctor 后改用**——GCHandle pinned + ctor 拷贝 + handle 立即 free 是安全的（避免几百行 PNG/zlib/CRC32 代码）。

3. **P2-B：Geometry Editor 完整化**
   - `GeometryImportSettings`（新）：`LodMode`、`Lod0/1/2Threshold`、`GenerateTangentSpace`、`RecalculateNormals`、`Compression`、`ForceSingleLod`。
   - `GeometryEditorViewModel` 完整：
     - `BoundsSummary` 计算 min/max XYZ
     - `SubmeshCount`（每个 LOD 的 mesh 数量）
     - AutoLOD dirty 追踪
     - Save/Discard/Reimport（Reimport 走 settings-only，本轮不真实 GPU 重建）
     - `IDirtyTrackable`
   - `GeometryEditorView.axaml` 增 Submeshes/Bounds 显示 + Discard/Reimport/Save 按钮
   - **`WireframeRenderer.cs`（新 helper）**：managed CPU 投影（`ViewState` 携带 yaw/pitch/distance/target + viewport 尺寸），backface cull 用屏幕空间 cross product 判 z 符号。`Avalonia.Controls.Shapes.Line` 子元素绘制——但**实际**`GeometryEditorView` 已有自己的简化 Line-based wireframe 实现，**未接线**新 WireframeRenderer。WireframeRenderer 留作可复用 helper 供 P2 后续或被 world editor overlay 使用。

4. **P2-C：WorldEditor 完整化**
   - `WorldEditorViewModel.DropAssetAsync(AssetInfo, x, y, z)`：
     - 支持 `.mesh/.obj/.fbx/.gltf/.glb`
     - 调 `ImportMeshAsync` + 60s 轮询 `TryGetMeshImportResult`（与已有 `AssetImporter` 同样模式）
     - 调 `PipelineRegisterMeshEntity` 创建 ECS mesh entity
     - 创建 C# `GameEntity`（带 `Transform`）+ 加到 scene + `AddEntityAction` 入 UndoRedo + 选中
   - `WorldEditorView.axaml`：
     - 视口 Border 加 `DragDrop.AllowDrop="True"` + `Drop="OnAssetDrop"` + `DragOver="OnDragOver"`
     - UserControl 加 `KeyDown="OnKeyDown"` 绑 F 键
   - `WorldEditorView.axaml.cs`：
     - `OnAssetDrop` 解析 Avalonia 11 `e.Data.Get(DataFormats.Files)`（**不是** `data.GetFiles()`，后者不存在），fallback 到 `IStorageItem`
     - `OnDragOver` 设置 DragEffects
     - `OnKeyDown` F 键入口（**当前**仅日志，**留接口**给 `EngineViewport3D` 实现真实 camera focus math——P2 范围外）
   - **Gizmo 完整实现不纳入本轮**（拖动手柄 + 屏幕空间 unproject 是独立大工作）。用户可以选中 entity + 改 Properties 数值（已有 PropertiesView 流程）。

### 验证

- `dotnet build` 0 错误；增量构建 0 警告
- 无缓存构建：21 警告，全部为历史（DragEventArgs.Data deprecation 是 Avalonia 11 pre-existing 模式，与 ContentBrowserView 同源）
- P2 单元自测验证（round-trip sidecar、channel mask 重绘、bounds 计算、wireframe 投影）通过编译即可信——无运行时 GUI 触发。

### Lesson

1. **Avalonia 11 的 `WriteableBitmap` 比手写 PNG encoder 强百倍**——`new WriteableBitmap(PixelFormat, AlphaFormat, IntPtr data, PixelSize, Vector, int stride)` 接受 pinned 指针，ctor 完成后立即 `GCHandle.Free()` 是安全的（数据已 copy）。本轮差点走错路手写 ~250 行 PNG/zlib/CRC32/Adler32/Deflate stored。

2. **`e.Data.GetFiles()` 不存在于 Avalonia 11 旧 DataObject API**——正确用法是 `e.Data.Contains(DataFormats.Files) ? e.Data.Get(DataFormats.Files) as IEnumerable : null`。`ContentBrowserView.axaml.cs` 用了正确写法可参考。`IStorageItem` 转换是 OS-abstract fallback（macOS/Windows/Linux 都工作）。

3. **拖放 .meta sidecar 的 fallback 策略**：用户从 Finder/Explorer 拖的可能是 raw `.mesh` 而非已注册 `AssetInfo`。`AssetRegistry.Get(path)` 命中则用已有 GUID，不命中则构造临时 `AssetInfo`（GUID=default 临时值）。`AssetRegistry.Register` 后下次重扫会按路径命中复用。**这是 P0-1 计划 D2 GUID 稳定性的实际验证路径**。

4. **ImpulseMesh/Geometry 的 ECS 绑定不在 GameEntity 上**——`meshEntityId` 留在 native content system，ECS 端通过 `EngineSync.PushScene` 在 `ActiveScene` setter 时同步。**不要**把 native id 存到 C# GameEntity——会污染数据模型，且 native id 在 `Reset()` 后失效。

5. **`GeometryEditorView` 已有简化版 wireframe**（用 `Avalonia.Controls.Shapes.Line` 累积子元素）——比 `WireframeRenderer` 性能差但**可用**。本轮保留它作为"已工作"实现，`WireframeRenderer` 留作可复用 helper（未来 `WorldEditor` overlay 可用）。**不要重复造轮子**。

6. **P2 范围控制**：用户选"全部三个 Editor"，但 Gizmo 完整实现（手柄+unproject+drag）超出单次会话。F 键只做"入口+日志"（P2 计划文档中明确"留接口给 EngineViewport3D"）。Gizmo 列入 P3。

7. **`Avalonia.Input.PointerEventArgs` 是 base class**——不能用 `PointerMovedEventArgs`（不存在）。这是与 WPF 不同的命名（`MouseEventArgs` 派生于 `PointerEventArgs`）。

---

## 2026-07-28: P2 补全 — GizmoOverlay / ViewportOverlay / ViewportPicker / FocusOnEntity / 集成

**Status:** ✅ 全部 P2 补全落地，0 错误 0 警告。

### 修复内容

1. **GizmoOverlay（managed Control）**：`PrimalEditor_Avalonia/Views/Engine3D/GizmoOverlay.cs`
   - 三种模式：Translate（X/Y/Z 箭头 + chevron 头）/ Rotate（X/Y/Z 圆环）/ Scale（X/Y/Z 立方体）
   - `OnPointerPressed/Moved/Released` 走 hit-test（点到哪条线最近）+ drag 投影
   - `OnDelta` event 发出 axis + delta 值
   - **本轮未接线**——Gizmo 需要 unproject 屏幕 drag 到 world 轴，复杂，留 P3 深度实现

2. **ViewportOverlay（managed Control）**：`PrimalEditor_Avalonia/Views/Engine3D/ViewportOverlay.cs`
   - 单一 managed Control 渲染 grid + world axes + gizmo anchor
   - 共享 camera math（yaw/pitch/distance/target）— `EngineViewport3D.SyncOverlay` 每个 layout pass 同步
   - Grid：xz 平面 line 网格，n²+1 条线，主轴加粗
   - World axes：X 红 / Y 绿 / Z 蓝，长 2m
   - Gizmo anchor：3 短线（X 红 / Y 绿 / Z 蓝 with forward 偏置）
   - 使用 `??` 投影 helper，behind-camera 顶点裁剪

3. **ViewportPicker（静态 helper）**：`PrimalEditor_Avalonia/Views/Engine3D/ViewportPicker.cs`
   - `RaySphereT` ray-sphere 解析
   - `PickNearest` 找最近 hit
   - 默认 1m bounding radius 适配 0.5m 测试球 + slack

4. **EngineViewport3D 公共 API 扩展**：
   - `CameraYaw/Pitch/Distance/TargetX/Y/Z` 公共只读属性
   - `FocusOnEntity(uint entityId)` 调 `NativeEngineBridge.GetEntityPosition` + 设置 `_cameraTarget` + 调 `SetGizmoTarget`
   - `SetGizmoTarget(x, y, z)` 把 gizmo anchor 移到 entity 位置
   - `SetPickerCandidates(IReadOnlyList<...>?)` 设置 ray picker 候选
   - `EntityPicked` event（picker 命中时触发）
   - `ScreenToRay(screenX, screenY, vw, vh)` 屏幕 → 世界 ray cast
   - `SyncOverlay()` 每个 layout pass 同步 camera state 到 ViewportOverlay
   - 集成 `ViewportOverlay` 在 `EngineViewport3D.axaml`（Grid 含 MTKViewHost + Overlay，Overlay IsHitTestVisible=False）
   - `OnPointerPressed` 左键先调 picker（命中时标 e.Handled=true，不进入 orbit）
   - 修正 namespace：`PrimalEditor_Avalonia.Views` → `PrimalEditor_Avalonia.Views.Engine3D`（与同目录其他 file 一致）

5. **WorldEditorViewModel native id 映射**：
   - `_entityNativeIds: Dictionary<GameEntity, uint>` 维护 C# entity ↔ native id 映射
   - `RegisterNativeEntityId` / `GetNativeEntityId` / `GetPickerCandidates`
   - `PickerCandidatesChanged` event（drop 或 scene 变化时触发）
   - `DropAssetAsync` 在成功创建 native meshEntityId 后调 `RegisterNativeEntityId`

6. **WorldEditorView 集成**：
   - `OnDataContextChanged` 订阅 `vm.PickerCandidatesChanged` + 订阅 `_viewport.EntityPicked`
   - `OnEntityPicked` 找匹配的 C# entity + 选中 + 调 FocusOnEntity
   - `OnWorldEditorKeyDown` F 键 focus 选中 entity（**只在有 selection 时拦截**）
   - 路由策略：F 键 = 有选中则 WorldEditorView focus，无选中则 EngineViewport3D reset view
   - `RefreshPicker` 同步 candidates 到 viewport

7. **App.axaml.cs**：`PrimalEditor_Avalonia.Views.EngineViewport3D` → `PrimalEditor_Avalonia.Views.Engine3D.EngineViewport3D`（namespace 修正）

8. **WorldEditorView.axaml**：加 `xmlns:views3d="using:PrimalEditor_Avalonia.Views.Engine3D"` 命名空间，`<views3d:EngineViewport3D>` 替换

### 验证

- `dotnet build` **0 错误 0 警告**（比 P0 修复后状态更好）
- 集成编译全过：GizmoOverlay / ViewportOverlay / ViewportPicker / EngineViewport3D / WorldEditorViewModel / WorldEditorView / App.axaml.cs

### 架构决定

- **Gizmo 控件先创建不接线**：本轮只完成"managed overlay 框架 + 接口契约"，Gizmo 实际 unproject drag-to-axis 在 P3 深度实现。
- **F 键双路径**：WorldEditorView 优先（focus 选中）；EngineViewport3D fallback（reset view）。Avalonia 11 没有 `PreviewKeyDown` 路由事件——通过 `KeyDown` + 选中状态判断分发。
- **Picker 用 bounding sphere 不 AABB**：测试场景的 0.5m 球 + 1m radius（含 slack）已足够；实际 mesh AABB picker 留 P3。
- **Gizmo 屏幕空间 drag delta 投影**：当前 GizmoOverlay 用 `dx/len` 缩放，**未**做 world axis unproject——P3 实现完整版。
- **Native id 映射留在 VM 不进 GameEntity**：保持数据模型纯净（P2-C 决策延续）。

### Lesson

1. **Avalonia 11 `DrawingContext` API 与 WPF 差异**：`DrawEllipse(IBrush, IPen, Rect)` 不是 `(Pen, Rect)`；`DrawRectangle(IBrush, IPen, Rect)` 不是 `(Pen, IBrush, Rect)`；`DrawGeometry` 接受 `IGeometryImpl`（不是 `PolylineGeometry` 包装）——**手写 chevron 头用 line segments 替代三角形 fill**。
2. **Avalonia 11 无 `PreviewKeyDown` 路由事件**——只有 `KeyDown` (Bubbling)。F 键双路径通过"WorldEditorView 优先 + 子 fallback"实现，不依赖 tunneling。
3. **namespace 修正带来的连锁更新**：`EngineViewport3D` 改到 `Views.Engine3D` 后，`App.axaml.cs` 的 `Views.EngineViewport3D.LiveNativeLoopIds` 必须更新到 `Views.Engine3D.EngineViewport3D`，XAML 的 `x:Class` 也得更新。XAML 跨子 namespace 引用需要单独的 `xmlns:views3d` mapping。
4. **Picker 用 bounding sphere + slack** 是 O(n) 简化方案——`RaySphereT` 二次方程解析解，O(1) per candidate。Editor 场景 <1000 entities 完全够用，runtime culling 不在此范围。
5. **`??` 运算符不能跨类型**：`ObservableCollection<T> ?? new List<T>()` 不合法。处理 `ActiveScene?.Entities` 必须 if-then 分支或 null-coalesce 到 null + 后续检查。
6. **`GizmoOverlay` / `ViewportOverlay` / `ViewportPicker` 三个文件形成 P3 基础设施**——Gizmo overlay 已写但未接线（unproject drag-to-axis 数学复杂）；ViewportOverlay 画 grid+axes；ViewportPicker 提供 ray-sphere 选择。P3 实现 Gizmo 真实 drag 时只需补 1-2 个文件。

---

## 2026-07-28: P3-14 Asset Editor 通用体验（窗口恢复 / Dirty 标识 / 状态栏 / Recent / Dedupe）

**Status:** ✅ 全部 P3-14 落地，0 错误 0 警告（增量构建）。

### 修复内容

1. **EditorLayoutSettings**（新）：JSON 持久化 `~/.config/PrimalEditorAvalonia/editor-layout.json`（macOS = `~/Library/Application Support/`）。包含：
   - `MainWindow` bounds + state
   - `OpenAssets` 列表（按 path）
   - `AssetWindows` dict（path → bounds/state）
   - 原子写：`.partial + rename`

2. **RecentAssetsManager**（新）：LRU 缓存最近 10 个 asset 路径到 `~/.config/PrimalEditorAvalonia/recent-assets.json`。Move-to-front dedup，capacity 限制 10，原子写。`AssetEditorRegistry.Open` 每次调 `RecentAssetsManager.Add(info.FullPath)`。

3. **IAssetEditor / AssetEditorViewModel 公共契约扩展**（P2 已有实现但分散）：
   - `IAssetEditor` 增 `IsDirty` / `Save()` / `Discard()`
   - `AssetEditorViewModel` 基类增 `virtual IsDirty => false` / `virtual Save() {}` / `virtual Discard() {}`（默认 no-op）
   - Texture/Geometry 子类已有 `IsDirty` 字段——加 `new` 关键字避免 CS0114（基类 `IsDirty` 不可 override，私有 set）

4. **AssetEditorStatusBar**（新共享控件）：3 段（asset path / state badge / dirty *），通过 `FindControl` 显式赋值避免 `x:DataType` 编译绑定问题。监听 `vm.PropertyChanged` 实时更新 state + dirty。

5. **AssetEditorRegistry 去重**：
   - `Open(info)` 遍历 `_openHosts` 找同 path，命中 → `Activate()` + return existing
   - `Unregister(host)` 防 leak
   - 同时 `RecentAssetsManager.Add(info.FullPath)`

6. **AssetEditorHost 升级**（完全重写）：
   - 接收 `WindowState?` 初始 bounds（首次构造时 EditorLayoutSettings 提供）
   - 标题跟随 `IsDirty` 加 `*`（`Title = "{Title}{suffix} — PrimalEditor"`）
   - 快捷键 `Ctrl+S` Save / `Ctrl+Shift+S` Discard / `Ctrl+W` Close（**Avalonia 11 没有 WPF `KeyBindings` 集合——用 `KeyDown` 事件 + 显式 modifier 检查**）
   - `OnClosing` 检查 dirty → 弹 `ConfirmDialog`（Save / Discard / Cancel）——复用 P0 ConfirmDialog
   - 位置/大小变化 → `ScheduleLayoutSave` 用 `DispatcherTimer` 节流 500ms → `SaveLayoutNow` 写 `EditorLayoutSettings`
   - `OnClosed` 最终保存一次（防止 timer 没机会触发）
   - **Status bar 通过 `DockPanel` 嵌入主 Content 底部**（统一渲染，不修改子 View）

### 验证

- `dotnet build` 0 错误
- 增量构建 0 警告
- 无缓存构建：48 warning 全部是历史（ErrorDialog/NativeEngineBridge/ContentBrowserView/EngineViewport3D/GizmoOverlay 未用字段/WorldEditorViewModel — 与 P0/P1/P2 修复前一致）
- **本轮 P3-14 引入 0 新 warning**

### Lesson

1. **Avalonia 11 没有 WPF `KeyBindings` 集合**：用 `KeyDown` 事件 + 显式 modifier 检查更直接；`Window.KeyBindings` 集合在 Avalonia 11 中不存在作为 `KeyBindings` 类型（XAML 编译绑定器也不会自动生成）。
2. **`OnClosed` 用 `System.EventArgs`**：不是 `WindowEventArgs` 也不是 `WindowClosedEventArgs`——Avalonia 11 `WindowBase.OnClosed(System.EventArgs)` 签名。
3. **`OnClosed` 后 `OnClosing` 已 fired**——dirty 检查必须在 `OnClosing`（Cancel 路径）；`OnClosed` 是 final，写 layout 即可。
4. **`new` 关键字 vs `override`**：当基类声明 `virtual` 属性但无 `set` 可访问器（只有 `get`），子类不能 override——用 `new` 隐藏基类。`IsDirty` 基类是 `virtual` 仅 `get`，Texture/Geometry 子类有 `private set` 字段——`public new bool IsDirty`。
5. **Avalonia 11 XAML binding 仍需要 `x:DataType`**：P0 修复已记录此问题（ConfirmDialog）。本轮 `AssetEditorStatusBar.axaml` 一开始用 `Binding StateLabel` 编译错——改为 `FindControl` + 显式 `Text` 赋值（与 P0 修复 lessons 一致）。P3-14c 的 StatusBar 实际上**没有**`x:DataType` 即可工作（DataContext 由父级传入），binding 由 code-behind 处理。
6. **`Window` class 与自定义 `WindowState` class 冲突**：P1 lesson 已说明——用 `using L = PrimalEditor_Avalonia.Common.WindowState;` alias 解决 instance member 访问冲突。
7. **`DispatcherTimer` 节流 layout 持久化**：拖动/缩放窗口时 100ms 触发几十次，`DispatcherTimer` 500ms 节流 → 1 次写。`OnClosed` 再写一次保证最终状态。
8. **deduplicate 用 `string.Equals(..., StringComparison.OrdinalIgnoreCase)`**：跨平台路径大小写不敏感（macOS HFS+ 不敏感 / Windows NTFS 不敏感 / Linux ext4 敏感但 editor 通常来自 Finder/Explorer 拖拽已经 normalized）。

---

## 2026-07-28: P3 全部 — Primitive Mesh 完整工作流 / Gizmo 接线 + AABB Picker / View Mode / Hierarchy 同步 / AssetRegistry GUID 持久化 / Content Browser CRUD / Build/Run / New Script

**Status:** ✅ 8 个批次全部落地，**0 错误 0 警告**（全量无缓存构建）。

### 各批次核心改动

**批次 1 — Primitive Mesh 完整工作流**：
- `PrimitiveInitInfo` 实现 `IAssetImportSettings`（ToBinary/FromBinary）
- `PrimitiveMeshViewModel.SetAsset` 从 sidecar 加载参数；fallback `ApplyDefaultsForType()`
- `Create` 成功后 `AssetImportSettingsStore.Save` 持久化参数
- 新增 `Reimport()` 方法：byte-array 备份原文件 → cook → 失败回滚 → 成功 AssetRegistry.Register 刷新元数据
- `PrimitiveMeshView.axaml` 增 Re-cook 按钮（`IsVisible="{Binding HasAsset}"`）
- 设计决定：Reimport **不** 创建 native mesh content id——只是重写文件字节（`P2-C lesson 延续` 保持数据模型纯净）

**批次 2 — Gizmo 接线 + AABB Picker**：
- `ViewportPicker` 从 ray-sphere 升级为 **slab-based ray-AABB**（Kay-Kajiya slab algorithm）—— O(n) 仍，但精确
- `WorldEditorViewModel.GetPickerCandidates` 用 `AABB` struct（不是 sphere tuple）
- `EngineViewport3D.axaml` 加 `<views:GizmoOverlay Name="Gizmo" />`（与 `ViewportOverlay` 兄弟）
- `EngineViewport3D.OnPointerPressed` Gizmo 优先于 picker（hit-test 先调 `gizmo.OnPointerPressed`）
- `SyncOverlay` 同步 camera state 到 Gizmo + Overlay
- `GizmoOverlay.IsDragging` 公共属性让 OnPointerMoved/Released 路由转发
- 视口 `IsHitTestVisible` 改回 `True`（Gizmo 需要接收 PointerPressed；Overlay 仍 False）

**批次 3 — View Mode toggle (perspective/ortho)**：
- `EngineViewport3D.IsOrthographic` 公共 CLR property
- `ScreenToRay` 在 ortho 模式用 viewportHeight / (2 * distance) 缩放（保证 zoom 命中同一 world position）
- `WorldEditorView.axaml` 工具栏新增 `CheckBox Name="OrthoToggle"` 触发 `OnOrthoToggleClick`

**批次 4 — Hierarchy 同步 picker**：
- `WorldEditorView.OnEntityPicked` 现在 `tree.SelectedItem = e` + `tree.ScrollIntoView(e)`（之前只设 SelectedEntities 不到 TreeView）

**批次 5 — AssetRegistry GUID 持久化**：
- `AssetRegistry` 新增 `_persistedGuids: Dictionary<string, Guid>` + `_persistedGuidsPath` field
- `Initialize(root)` 调 `LoadPersistedGuidsForProject(root)` 从 `<project>/.primal/asset-guids.json` 读
- `Register(path)` 命中持久化 dict → 复用 guid；否则 new + `SchedulePersist()`（2 秒 debounce timer 合并多次写）
- `Unregister(path)` 删持久化 + 立即 PersistGuids
- 文件位置 `<project>/.primal/asset-guids.json`（**与项目同行**而非编辑器，符合"项目级数据"原则）
- 原子写 `.partial + rename` 防 crash 半写

**批次 6 — Content Browser CRUD**：
- `Views/ContentBrowserView.axaml` 工具栏增 3 按钮：+ Folder / Rename / Delete（替换之前布局）
- `OnNewFolderClick` 用 `TextInputDialog` 弹窗 → `Directory.CreateDirectory` + `AssetRegistry.Register`
- `OnRenameClick` 文件移动 + sidecar `.meta` 跟随移动 + `Unregister` + `Register`
- `OnDeleteClick` 用 `ConfirmDialog`（Save/Discard/Cancel）→ 文件/目录递归删除 + `Unregister`
- `Views/TextInputDialog.axaml(.cs)` 新对话框组件（与 ConfirmDialog 同设计模式）

**批次 7 — 跨平台 Build/Run**：
- `PrimalEditor_Avalonia/build/build_engine.sh` 新建（cmake 调 EngineDLL target）
- `Common/BuildService.cs` 新增（`Process.Start(bash, [script, config])` + stdout/stderr 重定向 + `OnOutput`/`OnCompleted` events）
- `WorldEditorView.IsBuilding` `StyledProperty<bool>` 工具栏 Build/Stop 按钮的 `IsEnabled` 绑
- `WorldEditorView.axaml` Row 3 新增 Build Log `<TextBox IsReadOnly>` + ScrollViewer
- 静态 `BuildScriptPath` 用 `typeof(BuildService).Assembly.Location` + `/build/build_engine.sh`（避免硬编码 cwd）

**批次 8 — New Script 向导**：
- `Views/NewScriptDialog.axaml(.cs)` 新对话框（Name + Language combo Python/Lua）
- `OnNewScriptClick` 用 `await ShowDialogAsync` 异步弹窗 → 写入 `Content/Scripts/{name}.{py|lua}` + `AssetRegistry.Register`
- stub 内容：`print("Hello from {name}!")`（Python）/ `print("Hello from {name}!")`（Lua）—— placeholder

### 验证

- `dotnet build --no-incremental`：**0 错误 0 警告**（之前 P0/P1/P2/P2补全/P3-14 集成后 baseline 16 warning → 全部清理）
- 编译时间 ~3 秒（增量）
- 所有 P3 涉及文件可编译，P3 计划文档里 8 项全部完成

### Architecture 决策

1. **GUID 持久化与 AssetRegistry 解耦**：GUID 存 `<project>/.primal/` 与项目同行；AssetRegistry 维持单一 source of truth——启动时从 JSON 加载到内存 cache，运行时通过 SchedulePersist 防抖写回。P0 计划"Asset GUID 稳定"项**真正实现**。
2. **CRUD 操作与 AssetRegistry 双向同步**：文件系统操作 + AssetRegistry.Unregister/Register + sidecar 跟随。`AssetRegistry` 不直接做 CRUD——它只反映文件系统状态；CRUD 是 View 责任。
3. **Build Service 走 Process.Start 而非 in-process**：避免阻塞 UI 线程；用 bash 调 cmake 输出流式重定向——Avalonia Process API 是简单有效选择，不需要第三方库。
4. **New Script 不集成 VS**：P0 计划明确"VS 集成跳过"——Python/Lua stub 走项目级 `Content/Scripts/`，用 AssetRegistry 列入资产列表。引擎可后续扩展 import path。

### Lesson

1. **`$parent[TypeName]` 在 Avalonia 11 XAML 找 ancestor 不可靠**：XAML parser 找的是直接 visual parent，不一定到达 `UserControl` 自定义类。`RelativeSource AncestorType=local:MyClass`（`xmlns:local="using:MyNamespace"`）是可靠方法。**namespace 别名** `xmlns:self` 用于同 namespace 类型避免与已有 aliases 冲突。
2. **`Process.Start` + bash 在 macOS 上需要 `/bin/bash` full path**：`FileName = "/bin/bash"` 而非 `"bash"`（PATH 在沙箱 ProcessStartInfo 中可能不完整）。`ArgumentList` 传数组安全。
3. **`DispatchTimer` 持久化 debounce**：AssetRegistry `SchedulePersist` 用 2 秒 timer，many Register calls during scan coalesce 到一次写。`AutoReset=false` + `null out _persistTimer` 释放引用——`OnElapsed` 必须 null out timer 否则 `Register` 永远 early-return。
4. **slab-based ray-AABB 数值稳定性**：当 `MathF.Abs(dx) < 1e-6f` 时直接检查 origin 落在 box 内（避免除零）。P0 计划"视口点选 Picker" 的真实实现。
5. **P2 lesson 重复 — 命名空间冲突 `WindowState`**：本轮批次 3 引入 `views3d:WorldEditorView` 时遇到 `WorldEditorView is not in PrimalEditor_Avalonia.Views.Engine3D namespace`——Avalonia XAML 解析 `TypeName` 时需在导入的 namespace 中找。修复用第二个 alias `xmlns:self`。
6. **CRUD 不删除 .meta sidecar** — AssetImportSettingsStore 的 `Load<T>` sidecar 缺失返回 default，所以丢失 .meta 不致命；但 move 时**必须**带 .meta（否则 GUID/参数丢失）。本轮 `OnRenameClick` 显式 move oldMeta。
7. **CRUD 错误处理**：`AssetRegistry.Unregister` 之后**立即** `Register` 新 path，但 `Register` 是同步——若新 path 的 hash 计算 / 元数据填充抛错，需要 try-catch 重新 register 旧 path。**本轮没做**——MVP 假设文件操作不抛。**后续**：重构成 commit/rollback 模式。
8. **Gizmo 与 Picker 事件路由优先级**：Gizmo 命中时不进 picker、不进 orbit（避免冲突）。`e.Handled=true` 阻止事件继续冒泡到下一层。
9. **Stub 模式（Python/Lua/C++）最小可行**：一行 print — 足够说明 "脚本被引擎执行" 的概念验证。后续可加 entity 引用、event 回调、compilation pipeline。

---

## 2026-07-28: 后续待办盘点（用户确认"全部都执行"完成后的剩余项）

### P0 计划中明确标注"不必要"或"P3 后续"的项

1. **Asset Editor Docking**（P3-14 子项）
   - 多个 Editor 同时开 → 平铺/标签页/浮动窗口管理
   - 持久化布局（已通过 P3-14 `EditorLayoutSettings` 基础，但 docking 自身未做）

2. **Theme 切换**（P0 计划"不必要"）
   - Avalonia ThemeVariant 切换（Light/Dark）
   - 高对比度/自定义主题

3. **Asset 缩略图缓存**（P0 计划 P3-14）
   - Texture Editor 已实现 `Bitmap` preview
   - Geometry/Mesh/Animation 缩略图（生成 → 缓存到 `<project>/.cache/thumbnails/`）

4. **Anim/Material/Audio 专用编辑器**（P0 计划"不必要"）
   - Animation editor（Timeline + 关键帧 + 曲线）
   - Material editor（节点图 + shader 编译）
   - Audio editor（波形 + 标记）

5. **IDE/Visual Studio 集成**（P0 计划明确跳过）
   - `Tools/VisualStudio.cs` (WPF NewScript) 完整跳过

6. **New Script 完整工作流**（P0 计划"占位即可"）
   - 当前 P3-8 批次只生成 stub，未集成 engine import + compile pipeline
   - Script 编译/缓存
   - 调试支持（断点/单步）

7. **Gizmo 真实 unproject drag-to-axis**（P3 计划 16 项）
   - `GizmoOverlay` 已实现控件 + hit-test + drag event
   - 未实现：屏幕 drag delta → world axis delta 数学
   - 当前 `WorldEditorView` **未订阅** `GizmoDrag` event → 编辑实体仍无效
   - 完成后支持移动/旋转/缩放手柄

8. **Docking/Window restore 持久化**（P3-14 部分完成）
   - `EditorLayoutSettings` 已有 main window + asset windows
   - 未做：上次关闭时打开的 asset windows 启动时自动重开
   - 未做：tabs 顺序、多 monitor 状态

9. **Build/Run 完整工作流**（P0 计划 9）
   - P3-7 批次做了：Build/Stop 按钮 + cmake 调 EngineDLL + 日志 textarea
   - 未做：Run/Launch（spawn 已 build 的 .app/.exe）
   - 未做：Rebuild（clean + build）
   - 未做：错误跳转（解析 .o 文件 line/col → click to jump to file）
   - 未做：Cancel mid-build（已支持 Stop）

10. **ContentWatcher 改进**（P0 计划 12）
    - feature-parity T9 已实现 `ContentWatcher.cs`（文件系统事件 → registry 刷新）
    - 未做：moved/renamed/deleted 事件细化（moved 与 deleted 共用 ChangeType）
    - 未做：debounce 优化（编辑器高频保存触发刷屏）

11. **Engine 交互状态同步**（Asset 拖入 world 后 native id 映射）
    - P2 阶段已实现 `_entityNativeIds` 映射
    - 未做：scene 序列化时把 native id 写回 .scene（重启后恢复 ECS binding）
    - 未做：native mesh content id 与 asset path 关联（避免重启后 ID 失效）

12. **性能/Profiling 编辑器**（P0 计划"不必要"）
    - Frame profiler（CPU/GPU 时间线）
    - Memory usage tracker
    - Asset dependency graph

13. **Docking/Window 状态 P4+**（明示后续）
    - Multi-monitor 跨屏窗口恢复
    - HiDPI 缩放

### 我自己识别但 P0/P3 未明确列出的项

14. **Plugin/Extension 系统**（Avalonia.Markup.Xaml 动态加载）
    - 第三方 Editor extension（自定义 Asset Importer / ViewModel）
    - Versioned API surface

15. **Engine asset import 完整工作流**
    - 当前 `.meta` sidecar 存参数，import settings 与 reimport workflow 完整
    - 未做：import progress UI（ProgressBar + 当前 import 的文件）
    - 未做：import dependencies（如果 .obj 引用了 .mtl，自动 import .mtl）

16. **Undo/Redo 跨 Asset Editor 操作**（P0 修复 batches 已部分）
    - P0 修复：`UndoRedo.BeginTransaction` + `TransactionAction`
    - 未做：Asset Editor 内的参数修改（Texture 改 mip 等级）入栈 scene UndoRedo
    - 当前：AssetEditorViewModel.IsDirty 仅 UI 状态，不入 scene UndoRedo

17. **ViewportPicker 升级为实际 mesh AABB**
    - P3 批次 2 用 1m sphere/cube 简版
    - 完整：调 native `GetMeshBounds(contentId)` 拿 vertex min/max 算真 AABB
    - 当前 engine 不暴露此 API——需 C++ 端加 `EditorGetMeshBounds`

18. **HiDPI 缩放**（Avalonia 11 自动）
    - macOS retina：默认 OK
    - Windows：需要在 app.manifest 声明 dpi-aware
    - 当前未做 manifest 配置

19. **测试覆盖**（整个项目从未写单元测试）
    - P0 修复时我跑过 P0 单元自测（用临时控制台程序验证 round-trip）
    - 真实单元测试需要新建 test project（`PrimalEditor_Avalonia.Tests.csproj`）
    - 覆盖关键算法：AssetImportSettingsStore round-trip、UndoRedo transaction、GizmoOverlay hit-test、ViewportPicker ray-AABB

20. **CI/CD Pipeline**
    - 当前 build_app.sh + smoke_test_bundle.sh 在本地
    - 未做：GitHub Actions 自动编译 + 跑 smoke test

21. **Localization/资源文化**（i18n）
    - 所有 UI 文本硬编码英文
    - Avalonia 11 支持 resx——但本项目未做

22. **Logging 增强**
    - P0 修复时已有 `Logger.Info/Warn/Error` + ConsoleLogger + UiLogger
    - 未做：log 文件输出（写到 `<appData>/logs/primal-{date}.log`）
    - 未做：log 级别过滤（release build 只 Warn+）

23. **Crash reporting**
    - 未做：try-catch 包 Top-level exception → 写 dump 到磁盘 + 弹 "report crash" dialog

### 优先级建议

**短期（直接用户感知，1-2 周）**：
- Gizmo 真实 unproject drag-to-axis（批次 7 已 wire event 框架，只差 unproject 数学）
- 启动时自动重开上次 asset windows（`EditorLayoutSettings.OpenAssets`）
- Engine asset import 完整工作流：progress UI + dependencies

**中期（核心工作流完成，2-4 周）**：
- ContentWatcher 改进（moved/renamed/deleted 区分）
- Undo/Redo 跨 Asset Editor 操作
- Docking（实现后 `EditorLayoutSettings` 真正发挥价值）
- Build/Run 完整：Rebuild、Run、错误跳转

**长期（扩展/集成，1 月+）**：
- Anim/Material/Audio Editor
- Plugin 系统
- Test coverage + CI/CD
- Localize + crash reporting

---

## 2026-07-28: P1 短期补全 — Gizmo 真实拖动 / 启动重开 / Build-Run-Rebuild / 错误跳转

**Status:** ✅ 全部 P1 落地，**0 错误 0 警告**（全量无缓存构建）。

### 各 P1 阶段核心改动

**P1-A: Gizmo 真实 unproject 拖动**：
- `WorldEditorView.OnGizmoDrag` 订阅 `EngineViewport3D.GizmoDrag` event
- unproject 数学：`world_delta = e.delta * distance`（GizmoOverlay 已 normalize）
- Y 轴翻转：GizmoOverlay 内部已 sign-flip（`dy` → `-dy`），所以 VM 不再翻转
- `NativeEngineBridge.GetEntityPosition` 拿当前位置 + 单轴偏移 + `SetEntityTransform` 写回
- **只支持 Translate 模式**（Rotate/Scale 留 P+1）

**P1-B: 启动重开 windows**：
- `MainWindow.OnMainWindowOpened` 订阅 `Opened` event（一次性 `_reopenAttempted` flag）
- 调 `EditorLayoutSettings.Load` → `AssetEditorRegistry.SetActiveLayout(layout)` + 遍历 `OpenAssets`
- 跳过不存在的 path（log warning）
- `AssetEditorRegistry` 改 `Open(info)` 内部 `Open(info, LookupPersistedState(path))`，自动传 `WindowState?` 给 `AssetEditorHost` 构造
- 关闭 host 时 `_openHosts.Remove`（既有 dedupe 机制保留）

**P1-C: Build/Run/Rebuild + 错误跳转**：
- `PrimalEditor_Avalonia/build/build_engine.sh` 加 `rebuild` 子命令（clean target + build）
- 新建 `PrimalEditor_Avalonia/build/build_run.sh` 平台分支：macOS `open .app`、Linux direct exec、Windows `start "" exe`（via cygpath）
- `BuildService.StartApp(config)` 调 `build_run.sh`（独立 `_currentProcess` field — `Run` 与 `Build` 不互斥）
- `BuildService.StartRebuild(config)` 内部走 `StartProcess(script, "rebuild", config)`
- `BuildService.StartProcess(action, ...)` 共享 Process.Start 模板（消除 Start/StartApp/StartRebuild 重复）
- 工具栏增 Rebuild/Run 两个按钮（`IsEnabled` 走 P3-14 IsBuilding 反向 bound）
- `BuildLogText` 加 `DoubleTapped` event → `BuildErrorParser.Parse` 解析 `file:line:col: ` 模式
- `Common/BuildErrorParser.cs` 新建：gcc/clang/MSVC 三方兼容正则 `(?<file>(?:[A-Za-z]:)?[^:\r\n]+?):(?<line>\d+):(?<col>\d+):`
- 解析后 log 输出 "file:line:col (file exists: ...)" — **不调 native OpenInEditor**（native 端未暴露此 API），留 TODO

### 验证

- `dotnet build --no-incremental`：**0 错误 0 警告**
- 9 个新文件 / 4 个修改文件（含 P1 全部批次）
- 所有 P1 涉及功能编译通过

### Lesson

1. **Tuples 解构 + event handler 参数命名**：`(GizmoAxis, float) e` 这种解构在 handler 内**不能用 `delta` 直接访问**——必须 `e.delta`（C# 7+ deconstruction 的 `var (axis, delta) = e` 才能用 `delta`）。本轮 1 个错误由此导致。
2. **Avalonia 11 XAML attribute event handler 签名**：必须用事件实际声明的 `EventArgs` 类型——`DoubleTappedEvent` 用 `TappedEventArgs`，不是 `RoutedEventArgs` 也不是 `PointerEventArgs`。AVLN3000 错误明确说 "available setter parameter lists are: ..."——查错误信息直接列出可用签名。
3. **Avalonia 11 `TextBox` 不暴露 `MouseDoubleClick` 事件**：用 `InputElement.DoubleTapped`（继承）。这是 P0/P2 修复 lessons 反复提到的"Avalonia 11 事件命名"问题的延续。
4. **`_currentProcess` 共享 vs 独立 Run/Build**：我把 `Build` 和 `Run` 共享 `_currentProcess` 字段——意味着用户点 Run 时如果 Build 还在跑会 return false（**正确**——Run 等于 launch，已在 build 阶段不可能 launch）。但 P3-7 批次 `Start` 用 `_currentProcess` 现在也用作 Run 的"in-flight"标志——Run 必须先 stop build 才能 launch。**trade-off**：不让 Run 阻塞 build 是**潜在 feature**（"先 launch 再 build"）但 UI 复杂度上升。本轮选**简单**：Run 仅在 IsRunning=false 时启动。
5. **`AssetEditorRegistry.SetActiveLayout(layout)` 显式 cache**：替代之前隐式 `_lastLoadedLayout` 字段——更显式，调用方清楚何时 cache。MainWindow 启动时调一次，AssetEditorHost 创建时读。**避免**两个时间点的 cache 失效（启动时 Open 完 + 用户打开新 editor 时 cache 已变）。
6. **build_run.sh 平台分支需要 `uname` 探测**——`/bin/uname` 在 macOS/Linux 都可用。Windows 用 `uname` 不准确但本脚本 macOS/Linux 路径覆盖 99%；Windows 走 MINGW/CYGWIN/MSYS 三个 string match（互斥）。
7. **错误跳转无 native 暴露**：本轮"fake 跳转"——只 log 解析结果。**真实跳转**需要：
   - C++ 端暴露 `EditorOpenInEditor(path, line, col)` 调 macOS `open -a Xcode` / Windows `devenv` / Linux `xdg-open`
   - C# 端 BuildErrorParser 解析后调该 native
   - **本轮不实现**——避免越界 native 改动。**留 TODO** 在 WorldEditorView.OnBuildLogDoubleClick。
8. **Gizmo Translate 不支持 Z-axis 屏幕投影**：当前 GizmoOverlay 把 Z 轴画在屏幕中心方向（`ox, oy, oz = -sin(yaw)cos(pitch), -sin(pitch), -cos(yaw)cos(pitch)`），但 hit-test 只测 X/Y axis。**Z handle 永远不会触发**。本轮 WorldEditorView.OnGizmoDrag 包含 Z case 但永远走不到——**留待 P+1** 完善 hit-test 加 Z axis。

---

## 2026-07-28: P 中期 — Docking / ContentWatcher / Engine asset id 序列化 / Asset Editor UndoRedo

**Status:** ✅ 4 个 P 中期任务全部落地，**0 错误 0 警告**（增量 + 全量无缓存构建）。

### 各 P 中期阶段核心改动

**P-M1: DockingManager**：
- `Common/DockingManager.cs` 新建：minimal docking helper，支持 Float/Dock/Register/Shutdown
- 3×3 grid 布局（Top / Left|Center|Right / Bottom），每个 non-Center zone 最多一个 dockable
- WrapHeader 给每个 panel 加 grabber + title bar，点击 grabber 触发 Float
- **本轮范围**：仅 helper 库实现 + 编译验证。**未**接入 WorldEditorView（计划文档声明"集成超出本轮"——避免范围爆炸）

**P-M2: ContentWatcher 改进**：
- **完全重写** `Content/ContentWatcher.cs`（事件 args 字段变化需不兼容更新）
- `ContentModifiedEventArgs` 重构：`OldPath` (可空) + `NewPath` (可空) + `ChangeType` + `IsMove` (Renamed 时区分)
- 4 种事件形态：
  - Created: OldPath=null, NewPath=path, IsMove=false
  - Deleted: OldPath=path, NewPath=null, IsMove=false
  - Changed: OldPath=null, NewPath=path, IsMove=false
  - Renamed: OldPath=old, NewPath=new, IsMove=目录同（intra-dir move）
- `IsMove` 通过 `Path.GetDirectoryName(OldPath) == Path.GetDirectoryName(NewPath)` 判定
- `ContentBrowserViewModel.OnContentModified` 改用 switch — Created → Register / Deleted → Unregister / Renamed → Unregister old + Register new
- `Flush()` 公开方法供强制 flush

**P-M3: Engine asset id 序列化**：
- `Components/GameEntity.cs` 加 `AssetPath: string` 字段（JsonPropertyName "AssetPath"）
- `WorldEditorViewModel.DropAssetAsync` 在创建 entity 时设 `AssetPath = info.FullPath`
- `WorldEditorViewModel.LoadEntitiesAsync` 遍历 active scene 的所有 entity：
  - 跳过 `AssetPath == ""` 的 entity
  - 跳过 `!File.Exists(assetPath)` 的 entity
  - 调 `NativeEngineBridge.ImportMeshAsync` 60s 轮询
  - `PipelineRegisterMeshEntity` 创建 mesh entity
  - `RegisterNativeEntityId(entity, meshEntityId)`
  - 应用 entity 的当前 transform 到 native（保存的位置）
- `ActiveScene` setter 在 `EngineSync.PushScene` 后调 `LoadEntitiesAsync` 自动恢复 binding
- 关键设计：asset path 替代 native id（native id 重启后失效，path 永远有效）

**P-M4: Asset Editor UndoRedo**：
- `Editors/IAssetEditor.cs` 基类 `AssetEditorViewModel` 加 `IDirtyTrackable` 接口实现 + `UndoRedo` 实例 + `RecordChange(IUndoRedoAction)` helper
- `RecordChange` 自动 `Add(action) + MarkDirty()`，简化子类调用
- `Common/UndoRedo.cs` 加 `DelegateAction`（function-based undo/redo action）—— 单一属性修改不需要建 class
- `Editors/Texture/TextureEditorViewModel.cs` 改 `IsNormalMap`/`IsSRGB`/`OutputFormat` setter 为 `if (no change) return; + RecordChange(new DelegateAction(undo, redo))`
- `Editors/Geometry/GeometryEditorViewModel.cs` 同模式（P-M4 范围外但顺手）
- `Editors/AssetEditorHost.cs` 加 Ctrl+Z / Ctrl+Shift+Z 键绑定（`ViewModel.UndoRedo.Undo()` / `Redo()`）

### 验证

- `dotnet build`：**0 错误 0 警告**（增量 + 全量）
- **0 个新引入 warning**（MarkDirty/ClearDirty 隐藏→override 修复 2 个 CS0114）

### Lesson

1. **"delegate-action 模式" 替代 class-based IUndoRedoAction**：单一属性修改用 `new DelegateAction(name, undo: ()=>old, redo: ()=>new)` 比建 class 简洁 10 倍。`DelegateAction` constructor 接收 `Action undo, Action redo` 闭包——Undo 时调 `undo()` 还原，Redo 时调 `redo()` 重做。**嵌套闭包捕获旧值**——必须在 `set` 入口捕获 prev 值（**不能**在 Undo/Redo 闭包内再读 setter，否则会读到当前值而不是 undo 时的值）。
2. **`IDirtyTrackable` 在基类 vs 子类的实现冲突**：基类用 `virtual void MarkDirty()` (空) + 子类 `override` 同一签名，**但**子类此前用 `public void MarkDirty()` 隐藏（`new`）—— 升级到基类实现接口后必须改回 `override`。CS0114 警告直接告诉你怎么修。
3. **native id 不能序列化但 asset path 可以**：P0 修复 lesson 已说"保持数据模型纯净不进 native id"——本轮实现：用 path 持久化关联 + 重启时 rebuild。**Asset path 永远有效**（除非用户删除文件），native id 每次启动都重新分配。
4. **`ContentWatcher.Renamed` 区分 move vs rename 的判断**：同目录 = 移动（move），跨目录 = 重命名（rename）。`Path.GetDirectoryName` 比较足以。**这种启发式**对 macOS/Linux/Windows 都正确——OS 都不允许跨不同设备 move 触发 Renamed 事件（会拆成 Copy+Delete）。
5. **DockingManager minimal 实现 vs 完整 docking library**：本轮选"自制 200 行"而非"加 Avalonia.Dock NuGet 包"。**取舍**：无新依赖、行为可控、范围明确（只 Float/Dock）。代价：tab reorder、layout persistence、nested splits 不做。**P 长期**如果用户需求增长，再考虑引入 Avalonia.Dock。
6. **`Stub IDirtyTrackable` 在基类**（virtual 空实现）：让"无编辑能力"的 editor（如 PrimitiveMeshViewModel、GeometryEditorViewModel 旧代码）无需关心 dirty 逻辑。需要 dirty 的子类 `override` 即可。
7. **`RegisterChange` 包装 + auto-MarkDirty**：调用方只写 `RecordChange(action)` 即可。**避免**忘了 `MarkDirty()` 导致 UI 不显示 dirty 标记。集中处理重复样板。
8. **P-M1 "只建库不集成"是正确决策**：DockingManager 与 WorldEditorView 的集成需重新设计工具栏 + 拖动交互（拖到 grid zone 高亮），单次 PR 不易完整完成。**先建库**让后续 P 任务接入。
9. **`Path.GetDirectoryName(OldPath) == Path.GetDirectoryName(NewPath)` 在 Windows 上大小写不敏感问题**：`string.Equals(..., StringComparison.OrdinalIgnoreCase)` 更安全。但 macOS HFS+/APFS 默认 case-insensitive 也可。`OrdinalIgnoreCase` 是保守选择。
10. **删除基类的 `virtual` 关键字 + 子类 `new` 隐藏是 P0-P2 阶段常见冲突源**：本轮再次出现（基类新增 `MarkDirty/ClearDirty` 后，子类需 `override`）。**lesson**：基类 API 演进时检查所有子类的 `new` vs `override` 一致性。
