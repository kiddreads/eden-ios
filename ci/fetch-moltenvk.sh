#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Gets an iphoneos arm64 libMoltenVK.dylib and puts it where Eden looks for it.
#
# Two modes:
#   fetch-moltenvk.sh                       download + stage under build-ios/moltenvk/
#   fetch-moltenvk.sh --embed <App.app>     copy the staged dylib into <App>/Frameworks/
#
# ===========================================================================
# WHY AN EMBEDDED DYLIB AND NOT A STATIC LINK
# ===========================================================================
# src/video_core/vulkan_common/vulkan_library.cpp:28-53 has an iOS branch that tries,
# in order:
#     $LIBVULKAN_PATH
#     GetBundleDirectory()/"Frameworks/libMoltenVK.dylib"     <- THIS ONE
#     GetBundleDirectory()/"Frameworks/libvulkan.1.dylib"
#     dlopen(nullptr)                                          <- the static case
#
# The bundle dylib is the first real path it tries, and it needs no cooperation from
# the linker. The static alternative requires MoltenVK's vk* symbols to survive into
# the executable's export table so dlopen(nullptr)+dlsym can find them, which needs
# -force_load plus -u roots to survive -dead_strip, and none of that has ever been run
# for this project. Worse, nothing currently links MoltenVK at all on iOS:
# MOLTENVK_LIBRARY (externals/CMakeLists.txt:436-443) is consumed only by
# src/yuzu/CMakeLists.txt:385-391, the Qt desktop app, built with ENABLE_QT=OFF.
#
# ===========================================================================
# THE UPSTREAM GAP THIS SCRIPT WORKS AROUND
# ===========================================================================
# Eden's own dependency metadata has no iOS MoltenVK:
#   cpmfile.json:162-167          fetches artifact "MoltenVK-macOS.tar"
#   externals/CMakeLists.txt:443  sets MOLTENVK_LIBRARY to
#                                   ${moltenvk_SOURCE_DIR}/MoltenVK/dylib/macOS/libMoltenVK.dylib
#                                 under a plain `if (APPLE)` - which is TRUE for iOS.
# So an iOS configure today resolves MoltenVK to a macOS dylib. It happens to be
# harmless because nothing consumes it, but the real fix is a cpmfile.json entry for an
# iOS artifact plus an `if (IOS)` branch in externals/CMakeLists.txt. That is a change
# to Eden proper and outside this script.
#
# This script therefore fetches from MoltenVK's own releases and VERIFIES the platform
# rather than trusting a filename - shipping a macOS dylib inside an iOS bundle
# produces a dlopen failure at runtime and nothing at build time.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_DIR="${EDEN_MOLTENVK_STAGE:-$REPO_ROOT/build-ios/moltenvk}"
DYLIB="$STAGE_DIR/libMoltenVK.dylib"

# ---------------------------------------------------------------------------
# --embed
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--embed" ]; then
    APP_DIR="${2:?usage: fetch-moltenvk.sh --embed <path to .app>}"

    # An explicit override wins, so a developer with a dylib from elsewhere can point
    # at it without touching this script.
    if [ -n "${EDEN_MOLTENVK_DYLIB:-}" ] && [ -f "$EDEN_MOLTENVK_DYLIB" ]; then
        DYLIB="$EDEN_MOLTENVK_DYLIB"
    fi

    if [ ! -f "$DYLIB" ]; then
        # NON-FATAL, deliberately. The bar for cut one is that the project generates
        # and the target links, and the link does not involve MoltenVK at all (see the
        # header). Failing the build here would block a milestone that is otherwise
        # met. The app will fail at GPU init on a device instead, which is the honest
        # place for a missing runtime dependency to show up.
        echo "fetch-moltenvk.sh: WARNING - no libMoltenVK.dylib staged at $DYLIB"
        echo "fetch-moltenvk.sh: the app will build and link, but Vulkan will not load"
        echo "fetch-moltenvk.sh: on a device (vulkan_library.cpp will log 'No Vulkan"
        echo "fetch-moltenvk.sh: library found'). Run this script without --embed first,"
        echo "fetch-moltenvk.sh: or set EDEN_MOLTENVK_DYLIB."
        exit 0
    fi

    mkdir -p "$APP_DIR/Frameworks"
    cp -f "$DYLIB" "$APP_DIR/Frameworks/libMoltenVK.dylib"
    echo "fetch-moltenvk.sh: embedded -> $APP_DIR/Frameworks/libMoltenVK.dylib"

    # NOTE for whoever signs this: codesign does NOT sign nested code automatically.
    # Frameworks/libMoltenVK.dylib must be signed BEFORE the enclosing .app, or the
    # bundle signature is invalid and the app will not launch.
    exit 0
fi

# ---------------------------------------------------------------------------
# fetch + stage
# ---------------------------------------------------------------------------
mkdir -p "$STAGE_DIR"

if [ -f "$DYLIB" ]; then
    echo "fetch-moltenvk.sh: already staged at $DYLIB"
    exit 0
fi

VERSION="${MOLTENVK_VERSION:-v1.2.11}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "fetch-moltenvk.sh: fetching MoltenVK $VERSION"

# UNVERIFIED: the release asset name and the layout inside the archive. Nothing was
# downloaded while writing this (the machine driving this port has no spare disk), so
# the asset is selected by ranked name match and the dylib is LOCATED WITH find rather
# than at a hardcoded path. If the first run fails, the listing printed below is the
# thing to read.
ASSET_URL=""
for candidate in \
    "https://github.com/KhronosGroup/MoltenVK/releases/download/$VERSION/MoltenVK-ios.tar" \
    "https://github.com/KhronosGroup/MoltenVK/releases/download/$VERSION/MoltenVK-all.tar" ; do
    if curl -sfIL "$candidate" >/dev/null 2>&1; then
        ASSET_URL="$candidate"
        break
    fi
done

if [ -z "$ASSET_URL" ]; then
    echo "fetch-moltenvk.sh: could not find a downloadable MoltenVK asset for $VERSION" >&2
    echo "fetch-moltenvk.sh: set EDEN_MOLTENVK_DYLIB to a local iphoneos arm64 dylib instead" >&2
    exit 1
fi

echo "fetch-moltenvk.sh: $ASSET_URL"
curl -sfL "$ASSET_URL" -o "$TMP/moltenvk.tar"
tar -xf "$TMP/moltenvk.tar" -C "$TMP"

# MoltenVK ships several platform slices; pick the iOS one by path, not by guessing the
# directory name. Prefer a real dylib over the xcframework's static archive.
FOUND="$(find "$TMP" -name 'libMoltenVK.dylib' -path '*ios*' 2>/dev/null | head -1 || true)"
if [ -z "$FOUND" ]; then
    FOUND="$(find "$TMP" -name 'libMoltenVK.dylib' -path '*iOS*' 2>/dev/null | head -1 || true)"
fi

if [ -z "$FOUND" ]; then
    echo "fetch-moltenvk.sh: no iOS libMoltenVK.dylib inside the archive." >&2
    echo "--- what the archive actually contained ---" >&2
    find "$TMP" -maxdepth 4 \( -name '*.dylib' -o -name '*.a' -o -type d \) | head -60 >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# VERIFY THE PLATFORM. This is the whole point of not trusting the filename.
# A macOS dylib in an iOS bundle fails at dlopen on the device with nothing at build
# time to warn you.
# ---------------------------------------------------------------------------
if ! lipo -archs "$FOUND" 2>/dev/null | grep -qw arm64; then
    echo "fetch-moltenvk.sh: $FOUND is not arm64 (got: $(lipo -archs "$FOUND" 2>&1))" >&2
    exit 1
fi

# LC_VERSION_MIN_IPHONEOS / the LC_BUILD_VERSION platform field is what actually
# distinguishes an iphoneos dylib from a macOS one.
if ! otool -l "$FOUND" | grep -qiE 'platform (2|IOS)|LC_VERSION_MIN_IPHONEOS'; then
    echo "fetch-moltenvk.sh: $FOUND does not declare the iOS platform." >&2
    echo "--- load commands ---" >&2
    otool -l "$FOUND" | grep -A4 -iE 'LC_BUILD_VERSION|LC_VERSION_MIN' | head -20 >&2
    echo "fetch-moltenvk.sh: refusing to stage a non-iOS dylib." >&2
    exit 1
fi

cp -f "$FOUND" "$DYLIB"

# An embedded dylib must resolve relative to the bundle. @rpath/@executable_path is how
# a flat iOS bundle finds Frameworks/.
install_name_tool -id "@rpath/libMoltenVK.dylib" "$DYLIB" 2>/dev/null || true

echo "fetch-moltenvk.sh: staged $DYLIB"
echo "fetch-moltenvk.sh: $(lipo -archs "$DYLIB") $(du -h "$DYLIB" | cut -f1)"
