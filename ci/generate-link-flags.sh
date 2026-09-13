#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes a linker response file listing every static archive the CMake iOS build
# produced, by FULL PATH, for the Xcode app target to consume through
# OTHER_LDFLAGS '@$(SRCROOT)/link-flags.txt'. Run as an Xcode preBuildScript
# (src/ios/project.yml) before every build.
#
# Adapted from /Users/kiddreads/cemu-ios-muffin/ci/generate-link-flags.sh, which exists
# for a reason it states plainly: "CMake scatters .a outputs across many subdirectories
# ... and every time a new one turned up somewhere not already hand-listed in
# LIBRARY_SEARCH_PATHS, linking failed with 'library X not found' even though the .a
# existed. Full paths sidestep the whole search-path problem - if find locates it, the
# linker can find it too."
#
# Eden's tree has the same shape: src/*/lib*.a from the project's own targets, plus
# everything CPM fetched and built under <build>/_deps - and, for CPM's "ci"/prebuilt
# packages (FFmpeg's bundled path among them), archives that were only ever downloaded
# into .cache/cpm under the SOURCE tree and never touch <build> at all. Two finds cover
# all three; see the CPM_CACHE_DIR block below for why the second one exists.
#
# ===========================================================================
# WHAT THIS SCRIPT DELIBERATELY DOES NOT DO
# ===========================================================================
# It does NOT emit -u _vkGetInstanceProcAddr, and it does NOT force_load a MoltenVK
# archive. Both were proposed and both are wrong for this build, verified by reading:
#
#   * MOLTENVK_LIBRARY is set at externals/CMakeLists.txt:436-443 and consumed ONLY by
#     src/yuzu/CMakeLists.txt:385-391 - the Qt desktop app, which is built with
#     ENABLE_QT=OFF. Nothing in the iOS target links MoltenVK at all.
#   * Vulkan is resolved at RUNTIME by dlopen. src/video_core/vulkan_common/
#     vulkan_library.cpp:28-53 has an iOS branch that tries $LIBVULKAN_PATH, then
#     <bundle>/Frameworks/libMoltenVK.dylib, then <bundle>/Frameworks/libvulkan.1.dylib,
#     then dlopen(nullptr).
#
# So there is no vk* symbol anywhere in the link. `-u _vkGetInstanceProcAddr` would
# force an undefined symbol that nothing provides and FAIL THE LINK OUTRIGHT. MoltenVK
# reaches the app as an embedded dylib (ci/fetch-moltenvk.sh), which is the first path
# vulkan_library.cpp tries anyway.
#
# Corollary worth keeping in mind: the app links fine with no MoltenVK present. It
# fails later, at GPU init on a device. That is the correct place for it to fail in
# cut one.

set -euo pipefail

OUT="${1:?usage: generate-link-flags.sh <output-file> [build-dir]}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The app workflow configures into build-ios/; .github/workflows/build-ios-core.yml
# uses build/. Accept either, preferring an explicit argument, then the environment,
# then whichever exists.
BUILD_DIR="${2:-${EDEN_BUILD_DIR:-}}"
if [ -z "$BUILD_DIR" ]; then
    for candidate in "$REPO_ROOT/build-ios" "$REPO_ROOT/build"; do
        if [ -d "$candidate" ]; then
            BUILD_DIR="$candidate"
            break
        fi
    done
fi

if [ -z "$BUILD_DIR" ] || [ ! -d "$BUILD_DIR" ]; then
    echo "generate-link-flags.sh: FATAL - no CMake build directory found." >&2
    echo "  Looked for: $REPO_ROOT/build-ios and $REPO_ROOT/build" >&2
    echo "  Build the core first; see .github/workflows/build-ios-core.yml." >&2
    exit 1
fi

echo "generate-link-flags.sh: scanning $BUILD_DIR"

: > "$OUT"

# sort -u because CMake visits some archives through more than one path, and the
# linker warns about duplicates. Debug variants are excluded the way cemu's does.
ARCHIVES="$(find "$BUILD_DIR" -name '*.a' -not -path '*/debug/*' | sort -u)"

if [ -z "$ARCHIVES" ]; then
    echo "generate-link-flags.sh: FATAL - no .a files under $BUILD_DIR" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# CPM's prebuilt/CI packages, e.g. FFmpeg.
#
# CMakeLists.txt:13 sets CPM_SOURCE_CACHE to ${CMAKE_SOURCE_DIR}/.cache/cpm - a path
# under the SOURCE tree, not under whatever CMAKE_BINARY_DIR happened to be. CPMUtil's
# AddPackage() (CMakeModules/CPMUtil.cmake) downloads/extracts every bundled package
# into get_cache_path(), i.e. "$CPM_SOURCE_CACHE/<name>/<version-key>/", and for a "ci"
# package (cpmfile.json's "ci": true, e.g. the "ffmpeg-ci" entry AddJsonPackage(ffmpeg)
# resolves to under YUZU_USE_BUNDLED_FFMPEG - externals/ffmpeg/CMakeLists.txt:181-203)
# that IS the package: a prebuilt archive, never compiled into $BUILD_DIR at all. The
# FFmpeg::FFmpeg imported target's .a files physically live here, so the $BUILD_DIR
# scan above never sees them and every FFmpeg symbol video_core references
# (_av_strerror, _avcodec_open2, _av_frame_alloc, _av_hwdevice_ctx_create, ...) comes up
# undefined at the app's link - the CMake-level target_link_libraries(video_core
# PRIVATE ${FFmpeg_LIBRARIES}) at src/video_core/CMakeLists.txt:420 is real, but this
# script - not CMake - is what puts archives on the actual Xcode link line.
#
# This is not FFmpeg-specific: any "ci"-flagged cpmfile.json package (present or
# future) lands the same way, so the fix is to scan the whole cache, not one name.
#
# Not the OpenSSL trap (CMakeLists.txt:392-399, "libcrypto.a comes out built for
# macOS"): that was openssl-cmake building OpenSSL FROM SOURCE via its own Configure
# script, which auto-detects the HOST platform and picked darwin64-arm64-cc under
# cross-compilation. AddCIPackage (CMakeModules/CPMUtil.cmake:1023-1046) instead names
# the artifact it requests after the TARGET: "ios-aarch64" whenever IOS and
# CPMUTIL_ARM64 are both set, so the file ffmpeg-ci downloads is asked for by iOS/arm64
# name, not assumed from the host. Whether crueter-ci/FFmpeg's release actually
# published that artifact was NOT checked here - no network access in this lane - but
# the download mechanism itself is not the OpenSSL failure mode.
CPM_CACHE_DIR="${EDEN_CPM_CACHE_DIR:-$REPO_ROOT/.cache/cpm}"
if [ -d "$CPM_CACHE_DIR" ]; then
    echo "generate-link-flags.sh: scanning $CPM_CACHE_DIR (CPM prebuilt packages)"
    # cpmfile.json's "moltenvk" entry (repo V380-Ori/Ryujinx.MoltenVK) fetches
    # "MoltenVK-macOS.tar" UNCONDITIONALLY - the artifact name is not platform-
    # parameterised the way ffmpeg-ci's is, so it lands in the CPM cache as a macOS
    # slice regardless of what is actually being built. The broad find below
    # swept it up and handed the linker
    #   .cache/cpm/moltenvk/.../MoltenVK.xcframework/macos-arm64_x86_64/libMoltenVK.a
    # which failed with "building for iOS, but linking in object file ... built for
    # macOS" - a real app-link failure, not a warning.
    #
    # It should never have been a candidate at all: this project's whole MoltenVK
    # design, above, is that it reaches the app as an embedded RUNTIME DYLIB via
    # ci/fetch-moltenvk.sh, and nothing on the iOS side links it statically -
    # MOLTENVK_LIBRARY is consumed only by src/yuzu (the Qt desktop target, which
    # this build has ENABLE_QT=OFF for). Excluded by package name rather than by
    # platform string, so a future macOS-only CPM package cannot repeat this by
    # happening to share a slice name.
    CPM_ARCHIVES="$(find "$CPM_CACHE_DIR" -name '*.a' -not -path '*/debug/*' -not -path '*/moltenvk/*' | sort -u)"
    if [ -n "$CPM_ARCHIVES" ]; then
        ARCHIVES="$(printf '%s\n%s\n' "$ARCHIVES" "$CPM_ARCHIVES" | sort -u)"
    fi
else
    echo "generate-link-flags.sh: note - no CPM cache at $CPM_CACHE_DIR (nothing bundled/prebuilt, or not yet configured)"
fi

printf '%s\n' "$ARCHIVES" >> "$OUT"

# ---------------------------------------------------------------------------
# Named gate.
#
# Discovery handles the long tail; these are the ones whose absence means the CMake
# build did not actually produce a linkable core and the failure should say so here
# rather than as three hundred undefined C++ symbols.
#
# This is the set src/libretro_core/CMakeLists.txt links:
#   target_link_libraries(eden_libretro PUBLIC core
#                                       PRIVATE common input_common hid_core video_core network)
# plus eden_libretro itself.
#
# NOTE, and it is a real one: audio_core and frontend_common are NOT in this list, even
# though .github/workflows/build-ios-core.yml builds them. src/libretro_core/
# CMakeLists.txt says why in its own comment - frontend_common is skipped because
# firmware_manager.h's include graph is unverified, and audio_core because the current
# audio path emits silence and touches nothing in it. They are reached transitively via
# core if at all. Requiring them here would fail a build that is actually fine.
REQUIRED="libeden_libretro.a libcore.a libcommon.a libvideo_core.a libhid_core.a libinput_common.a libnetwork.a"
MISSING=""
for lib in $REQUIRED; do
    if ! printf '%s\n' "$ARCHIVES" | grep -q "/$lib\$"; then
        MISSING="$MISSING $lib"
    fi
done

if [ -n "$MISSING" ]; then
    echo "generate-link-flags.sh: FATAL - required archives missing:$MISSING" >&2
    echo "--- what WAS found ---" >&2
    printf '%s\n' "$ARCHIVES" | sed 's|.*/||' | sort -u >&2
    exit 1
fi

# Informational: these are built by build-ios-core.yml but not linked by eden_libretro.
for lib in libdynarmic.a libshader_recompiler.a libaudio_core.a libfrontend_common.a; do
    if ! printf '%s\n' "$ARCHIVES" | grep -q "/$lib\$"; then
        echo "generate-link-flags.sh: note - $lib not found (reached transitively, or not built)"
    fi
done

# ---------------------------------------------------------------------------
# -force_load on the core only.
#
# Every retro_* entry point is called directly by src/ios/Bridge/EdenCoreBridge.m, so
# ordinary archive resolution would pull them in anyway. This is belt and braces
# against the failure mode this project has already hit once: build-ios-core.yml grew a
# whole step ("Check libdynarmic.a actually contains the arm64 backend") because
# libdynarmic.a once "built green with no JIT backend in it" - a library can compile,
# archive and link perfectly while containing nothing. ci/verify-ipa.sh asserts the
# symbols survived into the final binary; this makes sure they got in.
#
# Only ONE archive is force-loaded. Doing it to all ~60 would bloat the binary with
# every unreferenced object in the tree.
EDEN_LIBRETRO="$(printf '%s\n' "$ARCHIVES" | grep '/libeden_libretro\.a$' | head -1)"
echo "-Wl,-force_load,$EDEN_LIBRETRO" >> "$OUT"

# force_load only controls which OBJECT FILES the linker considers - it does not
# stop -dead_strip (on by default for a Release/Archive iOS build) from later
# removing individual FUNCTIONS it judges unreachable, even inside a file it was
# forced to include. verify-ipa.sh's first run of this build proved that is exactly
# what happened: EdenCoreBridge.m calls retro_init()/retro_load_game()/retro_run()
# as ordinary direct C calls (confirmed by reading the source, not assumed), so the
# link succeeding at all already proves those calls resolved and retro_core.o was
# pulled in - "Build the app" would have failed with undefined symbols otherwise.
# The four symbols still went missing from the PACKAGED binary nm -g checks
# against, which is the signature of dead-code elimination running as a later,
# separate pass over already-linked code, not a resolution failure.
#
# -exported_symbol is the standard fix for a C ABI entry point that must survive
# whole-program dead-stripping regardless of what static reachability analysis
# concludes - exactly the libretro-core-baked-into-one-executable shape this port
# exists to make work on iOS, where there is no separate dylib to export from.
for sym in _retro_init _retro_run _retro_load_game _eden_libretro_set_metal_layer; do
    echo "-Wl,-exported_symbol,$sym" >> "$OUT"
done

# ---------------------------------------------------------------------------
# System libraries.
#
# libc++ because Eden's archives are C++ and reference its symbols; CMake adds this
# implicitly and Xcode linking pre-built archives does not.
#
# NOT added, listed here as the next candidates if the link turns up undefined symbols:
#   -lbz2        (if a bundled dependency was built against it)
#   -liconv      (CMakeLists.txt:546 sets PLATFORM_LIBRARIES to `iconv intl` on some
#                 platforms - NOT on the iOS branch, which is why it is not here)
#   -lcompression
#   -lz
# Whether any is needed cannot be settled without running the link, and adding them
# blind hides which one mattered.
echo "-lc++" >> "$OUT"

# Frameworks are declared in src/ios/project.yml's `dependencies:` rather than here, so
# that Xcode also knows about them for header search and module maps. Repeating them in
# the response file would be harmless but would split the list across two files.

echo "generate-link-flags.sh: wrote $(wc -l < "$OUT" | tr -d ' ') linker args to $OUT"
echo "generate-link-flags.sh: $(printf '%s\n' "$ARCHIVES" | wc -l | tr -d ' ') archives"
