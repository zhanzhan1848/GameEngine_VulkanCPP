#!/usr/bin/env bash
# build_app.sh wraps the dotnet apphost into a runnable macOS .app bundle,
# and copies the native engine dylibs (libEngineDLL, libContentTools,
# libfbxsdk) from the main repo's Darwin/<Config>/ into the dotnet output
# dir. The .app wrap is required for Avalonia to register as a foreground
# app (raw apphost gets StandardFlavor=NULL and the window never gets
# focus). The dylib copy is required because the .NET SDK silently drops
# AfterTargets hooks on its Build entry-point on incremental builds, so
# we do the copy here. Repo root is discovered via `git rev-parse
# --git-common-dir` so it works for both the main checkout and any
# worktree at any depth.
#
# Usage: ./build_app.sh [configuration]   # default: DebugEditor
set -euo pipefail

CONFIG="${1:-DebugEditor}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(cd "$HERE/.." && pwd)"
BUILD_DIR="$PROJ_DIR/../Darwin/PrimalEditor_Avalonia/$CONFIG"
APP_DIR="$BUILD_DIR/PrimalEditor.app"
APPHOST="$BUILD_DIR/PrimalEditor_Avalonia"

# Resolve repository root (main checkout). Used only as a LAST-RESORT fallback
# for native dylibs and shader metallibs. The worktree's own build outputs take
# priority so the running binary matches the code under review; silently copying
# from the main checkout was the root cause of "I edited the worktree but the
# .app keeps using stale dylib" reports.
REPO_ROOT=""
if command -v git >/dev/null 2>&1; then
  if git_common_dir="$(git -C "$PROJ_DIR" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)"; then
    if [[ -n "$git_common_dir" && "$git_common_dir" != "/" ]]; then
      REPO_ROOT="$(cd "$(dirname "$git_common_dir")" && pwd -P)"
    fi
  fi
fi
if [[ -z "$REPO_ROOT" || ! -d "$REPO_ROOT/Darwin" ]]; then
  REPO_ROOT="$(cd "$PROJ_DIR/../.." && pwd -P)"
fi

# WT_NATIVE_DIR: the worktree's own Darwin/<Config>/ build output. This is the
# SAME directory layout CMake/PrimalEditor_Avalonia.cmake emits when the user
# builds with the standard out-of-source flow inside the worktree.
WT_NATIVE_DIR="$PROJ_DIR/../Darwin/$CONFIG"

# Darwin/Debug/ is where `cmake --build build-wt` ACTUALLY writes: the
# project uses a single-config CMake generator (Unix Makefiles) with
# CMAKE_BUILD_TYPE force-set to "Debug" in the top-level CMakeLists.txt.
# Single-config generators ignore --config silently, so $CONFIG above
# (typically DebugEditor) only affects this script's copy path — not the
# build's output location. The dylib in Darwin/<Config>/ is either
# empty or a leftover from an older multi-config setup, and copying it
# to the .app bundle silently loads stale code. We treat Darwin/Debug/
# as the canonical build output and prefer it whenever it has the dylibs
# we're looking for.
WT_DEBUG_DIR="$PROJ_DIR/../Darwin/Debug"

# Pick the first existing directory for the dylib source.
NATIVE_LIB_DIR=""
LIB_SRC_LABEL=""
if [[ -d "$WT_NATIVE_DIR" ]] && ! [[ -d "$WT_DEBUG_DIR" ]]; then
  NATIVE_LIB_DIR="$WT_NATIVE_DIR"
  LIB_SRC_LABEL="worktree"
elif [[ -d "$REPO_ROOT/Darwin/$CONFIG" ]] && ! [[ -d "$WT_DEBUG_DIR" ]]; then
  NATIVE_LIB_DIR="$REPO_ROOT/Darwin/$CONFIG"
  LIB_SRC_LABEL="main checkout"
  echo "[build_app] WARNING: using main-checkout dylib at $NATIVE_LIB_DIR; the current worktree's edits are NOT running" >&2
  echo "[build_app]   worktree path tried: $WT_NATIVE_DIR" >&2
  echo "[build_app]   build it inside the worktree with: cmake --build build-wt --config $CONFIG" >&2
fi

# 合并后（910ffdfe 起）CMake 不再强制 CMAKE_BUILD_TYPE=Debug：build-wt 以
# DebugEditor 配置配置时，编辑器原生库真实落点是 Darwin/DebugEditor/
# （本脚本 $WT_NATIVE_DIR）。此时它才是与 .app 配置一致的产物；
# Darwin/Debug/ 是测试配置（可含 ENABLE_VULKAN，符号集也可能因构建时序
# 而陈旧——曾把缺 UpdateSceneEnvironment 的旧库拷进 .app 导致
# EntryPointNotFound 连环失败）。因此：DebugEditor/ 存在即优先；
# 仅当其缺锚点符号而 Debug/ 有时，才回退 Debug/（沿用下方锚点检查）。
if [[ -z "$NATIVE_LIB_DIR" && -d "$WT_NATIVE_DIR" ]]; then
  NATIVE_LIB_DIR="$WT_NATIVE_DIR"
  LIB_SRC_LABEL="worktree"
fi

# When Darwin/Debug/ exists alongside Darwin/<Config>/, it almost
# certainly contains the real build output (single-config generator,
# CMAKE_BUILD_TYPE=Debug). Prefer it. The symbol-presence check
# guards against the rare case where Debug/ was built before a
# newly-added ABI symbol was introduced — if Debug/ is missing the
# expected symbol AND the <Config>/ copy has it, fall back to <Config>/.
#
# "Expected symbol" anchor: CookMeshWithPipeline is the most recent
# ABI addition to libContentTools. If it exists in the dylib, the
# dylib was linked from current sources. We pick the dylib that
# has it. This is a per-call O(1) `nm` lookup (~1ms on macOS); the
# benefit (correct dylib loaded into the editor every time) far
# outweighs the cost.
if [[ -z "$NATIVE_LIB_DIR" && -d "$WT_DEBUG_DIR" ]]; then
  NATIVE_LIB_DIR="$WT_DEBUG_DIR"
  LIB_SRC_LABEL="worktree"
fi
if [[ "$NATIVE_LIB_DIR" == "$WT_NATIVE_DIR" && -d "$WT_DEBUG_DIR" ]]; then
  cfg_has=$({ nm -gU "$WT_NATIVE_DIR/libContentTools.dylib" 2>/dev/null | grep -c CookMeshWithPipeline; } 2>/dev/null || echo 0)
  dbg_has=$({ nm -gU "$WT_DEBUG_DIR/libContentTools.dylib" 2>/dev/null | grep -c CookMeshWithPipeline; } 2>/dev/null || echo 0)
  if [[ "$cfg_has" -eq 0 && "$dbg_has" -gt 0 ]]; then
    echo "[build_app] WARNING: $WT_NATIVE_DIR/libContentTools.dylib is stale (missing recent ABI symbols); using $WT_DEBUG_DIR" >&2
    echo "[build_app]   rebuild inside the worktree to refresh $WT_NATIVE_DIR:" >&2
    echo "[build_app]     $PROJ_DIR/build/build_engine.sh $CONFIG" >&2
    NATIVE_LIB_DIR="$WT_DEBUG_DIR"
    LIB_SRC_LABEL="worktree (Debug fallback — $CONFIG dylib was stale)"
  fi
fi

# Last-resort fallbacks for setups that don't have Darwin/Debug/ (older
# multi-config trees before CMAKE_BUILD_TYPE was forced to Debug).
if [[ -z "$NATIVE_LIB_DIR" && -d "$WT_NATIVE_DIR" ]]; then
  NATIVE_LIB_DIR="$WT_NATIVE_DIR"
  LIB_SRC_LABEL="worktree"
fi
if [[ -z "$NATIVE_LIB_DIR" && -d "$REPO_ROOT/Darwin/$CONFIG" ]]; then
  NATIVE_LIB_DIR="$REPO_ROOT/Darwin/$CONFIG"
  LIB_SRC_LABEL="main checkout"
  echo "[build_app] WARNING: using main-checkout dylib at $NATIVE_LIB_DIR; the current worktree's edits are NOT running" >&2
fi

if [[ -z "$NATIVE_LIB_DIR" ]]; then
  echo "[build_app] ERROR: native dylib not found in either:" >&2
  echo "[build_app]   worktree: $WT_NATIVE_DIR or $WT_DEBUG_DIR" >&2
  echo "[build_app]   main:     $REPO_ROOT/Darwin/$CONFIG" >&2
  echo "[build_app]   build the engine first (cmake --build build-wt inside the worktree)" >&2
  exit 1
fi
echo "[build_app] repo root:  $REPO_ROOT"
echo "[build_app] native lib: $NATIVE_LIB_DIR (source: $LIB_SRC_LABEL)"

# Source-tree staleness scan: warns when someone edits ContentTools/
# or EngineDLL/ source without running `cmake --build` at all. The
# mtime-based symbol check above can't catch that case (neither copy
# of the dylib has the new symbol yet because nothing was rebuilt).
# Just warn; can't pick a better copy.
WT_ROOT="$(cd "$PROJ_DIR/.." && pwd -P)"
if [[ "$LIB_SRC_LABEL" == worktree* && -d "$NATIVE_LIB_DIR" ]]; then
  stale=0
  for dylib in libEngineDLL.dylib libContentTools.dylib; do
    src_dir=""
    case "$dylib" in
      libEngineDLL.dylib)   src_dir="EngineDLL"   ;;
      libContentTools.dylib) src_dir="ContentTools" ;;
    esac
    [[ -z "$src_dir" ]] && continue
    dylib_path="$NATIVE_LIB_DIR/$dylib"
    [[ -f "$dylib_path" ]] || continue
    newer_count="$(find "$WT_ROOT/$src_dir" \( -name '*.cpp' -o -name '*.mm' \) \
                    -newer "$dylib_path" -print -quit 2>/dev/null | wc -l | tr -d ' ')"
    if [[ -n "$newer_count" && "$newer_count" != "0" ]]; then
      stale=1
      break
    fi
  done
  if [[ "$stale" == "1" ]]; then
    echo "[build_app] WARNING: $NATIVE_LIB_DIR dylibs are older than at least one source under ContentTools/ or EngineDLL/" >&2
    echo "[build_app]   rebuild before launching the editor:" >&2
    echo "[build_app]     $PROJ_DIR/build/build_engine.sh $CONFIG" >&2
    echo "[build_app]   (continuing anyway — the dylib may still be runnable; this is a warning, not an error)" >&2
  fi
fi

if [[ -d "$NATIVE_LIB_DIR" ]]; then
  # In-place install-name fixup at WT_NATIVE_DIR. CMake emits the
  # dylibs with @executable_path/libfbxsdk.dylib and @rpath/@loader_path
  # mismatches that only resolve inside an .app bundle. Anyone
  # dlopening the WT_NATIVE_DIR dylib directly (smoke tests, the
  # dotnet apphost before build_app finishes, the build's own
  # verification step) would otherwise get a silent "Library not
  # loaded" failure that looks like a missing entry point. Fixing
  # at the source means a single cmake build is self-contained.
  for dylib in libContentTools.dylib libEngineDLL.dylib; do
    src="$NATIVE_LIB_DIR/$dylib"
    [[ -f "$src" ]] || continue
    chmod +w "$src" 2>/dev/null || true
    case "$dylib" in
      libContentTools.dylib)
        install_name_tool -change @rpath/libfbxsdk.dylib @loader_path/libfbxsdk.dylib "$src" 2>/dev/null || true
        install_name_tool -change @executable_path/libfbxsdk.dylib @loader_path/libfbxsdk.dylib "$src" 2>/dev/null || true
        ;;
      libEngineDLL.dylib)
        install_name_tool -change @rpath/libContentTools.dylib @loader_path/libContentTools.dylib "$src" 2>/dev/null || true
        install_name_tool -id @loader_path/libEngineDLL.dylib "$src" 2>/dev/null || true
        ;;
    esac
  done

  for dylib in libEngineDLL.dylib libContentTools.dylib libfbxsdk.dylib; do
    src="$NATIVE_LIB_DIR/$dylib"
    dst="$BUILD_DIR/$dylib"
    if [[ -f "$src" ]]; then
      cp -f "$src" "$dst"
      echo "[build_app] copied  $src -> $dst"
      # Verify the engine dylib copy actually wrote identical bytes. cp -f can
      # silently truncate-and-stale when src is being concurrently rebuilt; a
      # checksum mismatch means the running binary will not match the code
      # under review, so fail loud rather than ship a confused .app.
      if [[ "$dylib" == "libEngineDLL.dylib" ]] && command -v shasum >/dev/null 2>&1; then
        src_sum="$(shasum -a 256 "$src" 2>/dev/null | awk '{print $1}')"
        dst_sum="$(shasum -a 256 "$dst" 2>/dev/null | awk '{print $1}')"
        echo "[build_app] checksum  libEngineDLL.dylib src=$src_sum dst=$dst_sum"
        if [[ -n "$src_sum" && -n "$dst_sum" && "$src_sum" != "$dst_sum" ]]; then
          echo "[build_app] ERROR: libEngineDLL.dylib checksum mismatch after copy" >&2
          echo "[build_app]   src: $src" >&2
          echo "[build_app]   dst: $dst" >&2
          echo "[build_app]   this usually means the source file was overwritten" >&2
          echo "[build_app]   mid-copy by a parallel build. Rebuild and retry." >&2
          rm -f "$dst"
          exit 1
        fi
      fi
    fi
  done

  # Copy the astcenc CLI binary used by ContentTools' Metal texture cook.
  # Prefer the worktree's built binary; fall back to the editor output dir.
  for astcenc in \
    "$REPO_ROOT/third_party/astc-encoder/build/Source/astcenc-neon" \
    "$BUILD_DIR/astcenc-neon"; do
    if [[ -f "$astcenc" ]]; then
      cp -f "$astcenc" "$BUILD_DIR/astcenc-neon"
      echo "[build_app] copied  astcenc-neon -> $BUILD_DIR/astcenc-neon"
      break
    fi
  done

  # Copy engine shader metallib (and the dedicated BlitToDrawable.metallib)
  # into the editor's cwd. The native engine resolves the shader blob via
  # a bare relative path (./shaders/metal/shaders.metallib), interpreted
  # against the process cwd, which the C# program pins to the binary
  # directory at startup. Without this copy, the editor opens but the
  # render surface stays black because the blit-to-drawable shader can't
  # be found and BlitRenderTargetToSurface becomes a no-op.
  #
  # The CMake target BuildEngineShaderBlob emits the metallib to
  # <build-dir>/shaders/metal/. The main-repo build output
  # (<REPO_ROOT>/Darwin/<Config>/) may or may not have it depending on
  # which cmake target was last built. We check both the legacy main-repo
  # build path AND the cmake build-tree path. The cmake build-tree path
  # is the source of truth for `cmake --build build-wt --target
  # BuildEngineShaderBlob` (the build directory the worktree's build-wt
  # uses); the legacy path is the main checkout's Darwin/Debug/...
  # directory. The editor's render surface stays BLACK if neither path has
  # the shader blob, because EngineShader::load_engine_shaders silently
  # returns false → fx::initialize, prepass::initialize, taa::initialize,
  # and ssgi::initialize all log "shader blob missing function or PSO
  # descriptor invalid" and disable their passes. The clear-color
  # fallback in MetalPostProcess then draws a black surface instead of
  # the composited scene.
  # The engine reads the shader blob via Engine/Graphics/Metal/MetalShader.cpp
  # ::load_engine_shaders, which expects the LEGACY PACKED format (9 copies of
  # u64 size + 16-byte hash + metallib, packed by build_engine_shader_blob.py
  # into shaders.metallib.packed). The raw shaders.metallib (single metallib,
  # starts with MTLB) is NOT what the engine wants — copying the raw one
  # produces a black surface because the engine reads the leading 8 bytes
  # as a huge u64 size and fails the tmp-file write.
  #
  # The CMake build (CMake/EngineShaderBlob.cmake) emits BOTH the raw
  # metallib (.metallib) and the packed blob (.metallib.packed) into
  # <build-dir>/shaders/metal/. We always prefer the .packed form and
  # fall back to the raw form only if the packed form is absent (older
  # builds that predate the packer).
  #
  # Search order (worktree first, then main checkout fallback):
  #   1. <worktree>/build-wt/shaders/metal/        (cmake --build build-wt path)
  #   2. <worktree>/Darwin/<Config>/shaders/metal/ (premake/legacy path)
  #   3. <main checkout>/Darwin/<Config>/shaders/metal/   (last-resort; WARNING)
  # Keep the worktree shader output in sync with its source. Otherwise this
  # script can find a valid but stale main-checkout metallib and silently run
  # old viewport coordinate logic even though the C# app came from this
  # worktree.
  WT_SHADER_BUILD_DIR="$WT_ROOT/build-wt"
  WT_BLIT_SOURCE="$WT_ROOT/Engine/Graphics/Metal/shaders/BlitToDrawable.metal"
  WT_BLIT_OUTPUT="$WT_SHADER_BUILD_DIR/shaders/metal/BlitToDrawable.metallib"
  if [[ -d "$WT_SHADER_BUILD_DIR" ]] &&
     { [[ ! -f "$WT_BLIT_OUTPUT" ]] || [[ "$WT_BLIT_SOURCE" -nt "$WT_BLIT_OUTPUT" ]]; }; then
    echo "[build_app] rebuilding worktree Metal shaders (missing or stale BlitToDrawable.metallib)"
    cmake --build "$WT_SHADER_BUILD_DIR" --target BuildEngineShaderBlob -j2
  fi

  for metallib in shaders.metallib BlitToDrawable.metallib; do
    found=0
    for src in \
      "$PROJ_DIR/../build-wt/shaders/metal/${metallib}.packed" \
      "$PROJ_DIR/../../build-wt/shaders/metal/${metallib}.packed" \
      "$PROJ_DIR/../Darwin/$CONFIG/shaders/metal/${metallib}.packed" \
      "$PROJ_DIR/../Darwin/Debug/shaders/metal/${metallib}.packed" \
      "$PROJ_DIR/../Darwin/Release/shaders/metal/${metallib}.packed" \
      "$REPO_ROOT/Darwin/Debug/shaders/metal/${metallib}.packed" \
      "$REPO_ROOT/Darwin/Release/shaders/metal/${metallib}.packed" \
      "$PROJ_DIR/../build-wt/shaders/metal/$metallib" \
      "$PROJ_DIR/../../build-wt/shaders/metal/$metallib" \
      "$PROJ_DIR/../Darwin/$CONFIG/shaders/metal/$metallib" \
      "$PROJ_DIR/../Darwin/Debug/shaders/metal/$metallib" \
      "$PROJ_DIR/../Darwin/Release/shaders/metal/$metallib" \
      "$REPO_ROOT/Darwin/Debug/shaders/metal/$metallib" \
      "$REPO_ROOT/Darwin/Release/shaders/metal/$metallib" \
      ; do
      if [[ -f "$src" ]]; then
        # If the first match comes from a main-checkout path, warn so the user
        # knows the worktree's edits are not the ones running.
        case "$src" in
          # Worktree paths live under the worktree dir, which is itself a
          # child of $REPO_ROOT, so a bare "$REPO_ROOT"/* pattern would
          # mis-flag them. Skip any path under the worktree root.
          "$REPO_ROOT/$WT_NATIVE_DIR"|"$PROJ_DIR"/*|"$PROJ_DIR/.."/*) ;;
          "$REPO_ROOT"/*)
            echo "[build_app] WARNING: shader $metallib resolved from main-checkout fallback ($src); worktree path missing" >&2 ;;
        esac
        mkdir -p "$BUILD_DIR/shaders/metal"
        cp -f "$src" "$BUILD_DIR/shaders/metal/$metallib"
        echo "[build_app] copied  $src -> $BUILD_DIR/shaders/metal/$metallib"
        found=1
        break
      fi
    done
    if [[ $found -eq 0 ]]; then
      echo "[build_app] WARNING: $metallib not found in any expected location; the editor viewport will be black"
    fi
  done

  if [[ ! -e "$BUILD_DIR/libPrimalEngine.dylib" && -f "$BUILD_DIR/libEngineDLL.dylib" ]]; then
    ln -sf libEngineDLL.dylib "$BUILD_DIR/libPrimalEngine.dylib"
    echo "[build_app] symlink  $BUILD_DIR/libPrimalEngine.dylib -> libEngineDLL.dylib"
  fi
  if [[ ! -e "$BUILD_DIR/PrimalEngine.dylib" && -f "$BUILD_DIR/libEngineDLL.dylib" ]]; then
    ln -sf libEngineDLL.dylib "$BUILD_DIR/PrimalEngine.dylib"
    echo "[build_app] symlink  $BUILD_DIR/PrimalEngine.dylib -> libEngineDLL.dylib"
  fi

  if [[ -f "$BUILD_DIR/libContentTools.dylib" ]]; then
    chmod +w "$BUILD_DIR/libContentTools.dylib" 2>/dev/null || true
    install_name_tool -change @rpath/libfbxsdk.dylib @loader_path/libfbxsdk.dylib "$BUILD_DIR/libContentTools.dylib" 2>/dev/null || true
    install_name_tool -change @executable_path/libfbxsdk.dylib @loader_path/libfbxsdk.dylib "$BUILD_DIR/libContentTools.dylib" 2>/dev/null || true
  fi
  if [[ -f "$BUILD_DIR/libEngineDLL.dylib" ]]; then
    chmod +w "$BUILD_DIR/libEngineDLL.dylib" 2>/dev/null || true
    install_name_tool -change @rpath/libContentTools.dylib @loader_path/libContentTools.dylib "$BUILD_DIR/libEngineDLL.dylib" 2>/dev/null || true
    install_name_tool -id @loader_path/libEngineDLL.dylib "$BUILD_DIR/libEngineDLL.dylib" 2>/dev/null || true
  fi
fi

if [[ ! -x "$APPHOST" ]]; then
  echo "[build_app] apphost not found at $APPHOST" >&2
  echo "[build_app] Run 'dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c $CONFIG' first." >&2
  exit 1
fi

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "[build_app] build directory $BUILD_DIR missing." >&2
  exit 1
fi

echo "[build_app] configuration: $CONFIG"
echo "[build_app] build dir:      $BUILD_DIR"
echo "[build_app] target bundle: $APP_DIR"

# Wipe previous bundle contents (preserve the .app directory itself so the
# launcher doesn't have to re-create it).
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

# Info.plist (required for foreground-app registration).
TEMPLATE="$HERE/Info.plist.template"
if [[ ! -f "$TEMPLATE" ]]; then
  echo "[build_app] Info.plist.template missing at $TEMPLATE" >&2
  exit 1
fi
cp "$TEMPLATE" "$APP_DIR/Contents/Info.plist"

# PkgInfo: the 8-byte marker macOS uses to recognize an app (APPL????).
printf 'APPL????' > "$APP_DIR/Contents/PkgInfo"

# Copy the apphost binary into Contents/MacOS/<bundle-executable-name>.
# The bundle executable name must match Info.plist's CFBundleExecutable.
cp "$APPHOST" "$APP_DIR/Contents/MacOS/PrimalEditor_Avalonia"

# Copy all sibling files (DLLs, resources, symlinks) into Contents/MacOS/ so
# the binary can find its deps via @loader_path-relative resolution.
# This includes the libEngineDLL.dylib that the post-build CopyNativeEngineLibsUnix
# target already drops into $BUILD_DIR.
shopt -s dotglob nullglob
for entry in "$BUILD_DIR"/*; do
  base="$(basename "$entry")"
  case "$base" in
    PrimalEditor_Avalonia)         ;;  # already copied as renamed executable
    PrimalEditor.app)              ;;  # don't recurse
    *Darwin.config)                ;;  # build-time helper, not a runtime dep
    *) cp -R "$entry" "$APP_DIR/Contents/MacOS/" 2>/dev/null || true ;;
  esac
done
shopt -u dotglob nullglob

# Re-execute the symlink for libPrimalEngine.dylib → libEngineDLL.dylib
ln -sf libEngineDLL.dylib "$APP_DIR/Contents/MacOS/libPrimalEngine.dylib"

# Make sure the launcher bit is set after the copy (cp preserves mode, but be
# explicit in case the apphost lacked +x).
chmod +x "$APP_DIR/Contents/MacOS/PrimalEditor_Avalonia"

# Touch the bundle so LaunchServices re-reads the Info.plist on next launch.
# (Without this, lsappinfo can cache a previous NULL StandardFlavor.)
touch "$APP_DIR"

# Ad-hoc codesign: required for `open`/LaunchServices to grant the bundle
# foreground status. Without it, XPC services reject the bundle's bundle ID
# and the process cannot connect to com.apple.windowserver as a real app
# (lsappinfo reports StandardFlavor = NULL, and HIServices BringForward has
# no effect when triggered from the dark). --deep is needed so all nested
# dylibs and frameworks inherit a matching signature in the bundle.
codesign --force --deep --sign - "$APP_DIR" >/dev/null 2>&1 || true

# Re-register with LaunchServices so LS sees the new bundle immediately.
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
  -f "$APP_DIR" >/dev/null 2>&1 || true

echo "[build_app] bundle built:"
echo "  exec:  $APP_DIR/Contents/MacOS/PrimalEditor_Avalonia"
echo "  info:  $APP_DIR/Contents/Info.plist"
echo
echo "[build_app] To launch:"
echo "  open '$APP_DIR'        # macOS LaunchServices (recommended — gives focus)"
echo "  '$APP_DIR/Contents/MacOS/PrimalEditor_Avalonia'   # raw exec (may have focus issues)"
