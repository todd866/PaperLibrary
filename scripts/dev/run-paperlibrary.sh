#!/bin/zsh
# Launch PaperLibrary from the build tree with the Craft Qt/KF runtime and
# QtWebEngine helper paths needed by the shell-owned EPUB reader.
#
# WHY THIS EXISTS: `open build/bin/PaperLibrary.app` does NOT work — macOS
# LaunchServices starts the bundle with a clean environment. During development
# the app expects the Craft Qt/KF environment plus QtWebEngine's helper process,
# resources and locales, which live under ~/CraftRoot.
#
# Usage: run-paperlibrary.sh [file ...]   (pass a document to open it in a tab)
set -e
SCRIPT_DIR="${0:A:h}"
REPO="${REPO:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
BUILD="${BUILD:-$REPO/build}"
APP_BIN="$BUILD/bin/PaperLibrary.app/Contents/MacOS/PaperLibrary"
[ -f "$HOME/CraftRoot/craft/craftenv.sh" ] || { echo "TOOLCHAIN MISSING — run scripts/dev/rebuild-toolchain.sh"; exit 1; }
[ -f "$APP_BIN" ] || { echo "App not built: $APP_BIN — build target paperlibrary first"; exit 1; }
# craftenv provides the Qt/DYLD runtime env; it changes CWD and may touch
# unset vars, so guard it and never run under set -u.
source "$HOME/CraftRoot/craft/craftenv.sh" >/dev/null 2>&1 || true
export QT_PLUGIN_PATH="$BUILD/bin:${QT_PLUGIN_PATH:-}"
# QtWebEngine runtime (EPUB web reader): point at the Craft WebEngine helper
# process, resources and locales so the render process can start.
WE="$HOME/CraftRoot/lib/QtWebEngineCore.framework/Versions/A"
export QTWEBENGINEPROCESS_PATH="$WE/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="$WE/Resources"
export QTWEBENGINE_LOCALES_PATH="$WE/Resources/qtwebengine_locales"
pkill -x PaperLibrary 2>/dev/null || true
nohup "$APP_BIN" "$@" >/dev/null 2>"${PAPERLIBRARY_LOG:-/tmp/paperlibrary.log}" &
disown
echo "launched PaperLibrary  pid=$!  (stderr -> ${PAPERLIBRARY_LOG:-/tmp/paperlibrary.log})"
