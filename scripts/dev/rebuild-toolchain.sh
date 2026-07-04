#!/usr/bin/env bash
# Rebuild the KDE Craft toolchain (~/CraftRoot) from scratch for the PaperLibrary fork.
#
# Recovery scaffolding: if ~/CraftRoot is ever lost again, this is the one command.
#   ./rebuild-toolchain.sh /path/to/CraftBootstrap.py
#
# ABI is macos-clang-arm64: the bootstrapper's macOS branch defaults to arm64 under
# --use-defaults, and the arm64 binary cache IS supported on macOS (the "unsupported"
# branch is Linux-only). Do NOT let this pick x86_64 — that's the Rosetta trap.
#
# NOTE: no `set -u` — Craft's own craftenv.sh references unset vars (e.g.
# CRAFT_PYTHON_BIN) and aborts under `set -u` the moment it is sourced.
set -o pipefail

BOOTSTRAP="${1:?usage: rebuild-toolchain.sh /path/to/CraftBootstrap.py}"
PREFIX="$HOME/CraftRoot"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
CRAFTENV="$PREFIX/craft/craftenv.sh"

say() { echo "[rebuild $(date '+%H:%M:%S')] $*"; }

say "starting; prefix=$PREFIX abi=macos-clang-arm64"

# ---- 1. Bootstrap Craft (non-interactive, arm64) ----
if [ -f "$CRAFTENV" ]; then
  say "CraftRoot already bootstrapped — skipping bootstrap"
else
  say "bootstrapping Craft (downloads craft from github)..."
  python3 "$BOOTSTRAP" --prefix "$PREFIX" --use-defaults
  ec=$?; say "bootstrap exit=$ec"
  [ $ec -eq 0 ] || { say "BOOTSTRAP FAILED"; exit 1; }
fi
[ -f "$CRAFTENV" ] || { say "craftenv.sh missing after bootstrap"; exit 1; }
say "verify ABI:"; grep -i "abi\|arch" "$PREFIX/etc/CraftSettings.ini" 2>/dev/null | head

# ---- 2. Install the Qt/KF dependencies from the arm64 binary cache ----
say "installing dependencies via craft (Qt6 + KF6 runtime from binary cache)..."
# shellcheck disable=SC1090
( source "$CRAFTENV" && craft \
    libs/qt6/qtbase \
    libs/qt6/qtsvg \
    libs/qt6/qtwebengine \
    frameworks/extra-cmake-modules \
    frameworks/karchive \
    frameworks/kconfig \
    frameworks/kconfigwidgets \
    frameworks/kcoreaddons \
    frameworks/kcrash \
    frameworks/ki18n \
    frameworks/kiconthemes \
    frameworks/kio \
    frameworks/kwidgetsaddons \
    frameworks/kwindowsystem \
    frameworks/kxmlgui )
ec=$?; say "craft install-deps exit=$ec"
[ $ec -eq 0 ] || { say "craft install-deps FAILED"; exit 1; }

# ---- 3. Verify: configure + build the PaperLibrary app target ----
say "verifying: configuring and building the PaperLibrary app target..."
# shellcheck disable=SC1090
source "$CRAFTENV"
cmake -S "$REPO" -B "$REPO/build" -DCMAKE_PREFIX_PATH="$PREFIX" -DBUILD_TESTING=ON
ec=$?; say "configure exit=$ec"
[ $ec -eq 0 ] || { say "CONFIGURE FAILED"; exit 1; }
cmake --build "$REPO/build" --target paperlibrary
ec=$?; say "build exit=$ec"
[ $ec -eq 0 ] || { say "BUILD FAILED"; exit 1; }

say "SUCCESS — toolchain rebuilt and PaperLibrary builds clean"
