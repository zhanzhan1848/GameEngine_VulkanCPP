#!/usr/bin/env bash
# build_app.sh — wrap a published Avalonia apphost into a runnable macOS .app bundle.
#
# Why this exists:
#   Avalonia 11 on macOS launched via `dotnet run` (or directly as a plain Mach-O
#   apphost binary) registers with WindowServer without an Info.plist, which
#   means macOS treats the process as a CLI tool, not a foreground app. The
#   MainWindow is created, IsVisible is true, but it gets no focus and never
#   appears in the visible-process list. lsappinfo reports StandardFlavor=NULL.
#
#   Wrapping the same binary into a proper .app bundle (Info.plist + PkgInfo +
#   Contents/MacOS/<executable>) flips it into a foreground-eligible app:
#   System Events sees it, WindowServer can hand it focus on launch, and the
#   window shows up on screen.
#
# Usage:
#   ./build_app.sh [configuration]    # default: DebugEditor
#
# Inputs:
#   Darwin/PrimalEditor_Avalonia/<configuration>/   (the dotnet build output)
# Outputs:
#   Darwin/PrimalEditor_Avalonia/<configuration>/PrimalEditor.app/
set -euo pipefail

CONFIG="${1:-DebugEditor}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_DIR="$(cd "$HERE/.." && pwd)"
BUILD_DIR="$PROJ_DIR/../Darwin/PrimalEditor_Avalonia/$CONFIG"
APP_DIR="$BUILD_DIR/PrimalEditor.app"
APPHOST="$BUILD_DIR/PrimalEditor_Avalonia"

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
