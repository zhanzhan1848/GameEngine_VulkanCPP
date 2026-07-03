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
NATIVE_LIB_DIR="$REPO_ROOT/Darwin/$CONFIG"
echo "[build_app] repo root:  $REPO_ROOT"
echo "[build_app] native lib: $NATIVE_LIB_DIR"

if [[ -d "$NATIVE_LIB_DIR" ]]; then
  for dylib in libEngineDLL.dylib libContentTools.dylib libfbxsdk.dylib; do
    src="$NATIVE_LIB_DIR/$dylib"
    dst="$BUILD_DIR/$dylib"
    if [[ -f "$src" ]]; then
      cp -f "$src" "$dst"
      echo "[build_app] copied  $src -> $dst"
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
  # which cmake target was last built. We check both.
  for metallib in shaders.metallib BlitToDrawable.metallib; do
    found=0
    for src in \
      "$REPO_ROOT/Darwin/Debug/shaders/metal/$metallib" \
      "$REPO_ROOT/Darwin/Release/shaders/metal/$metallib" \
      ; do
      if [[ -f "$src" ]]; then
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
