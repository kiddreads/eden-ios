#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Gets an iphoneos arm64 MoltenVK dynamic library and puts it where Eden looks for it.
#
# Two modes:
#   fetch-moltenvk.sh                       download + verify + stage under build-ios/moltenvk/
#   fetch-moltenvk.sh --embed <App.app>     copy the staged dylib into <App>/Frameworks/
#
# ===========================================================================
# WHAT IS ACTUALLY PUBLISHED - verified against the releases and the packaging scripts
# ===========================================================================
# Checked 2026-09-11 with `gh api repos/KhronosGroup/MoltenVK/releases`.
#
# Every release from v1.2.4 onward publishes exactly these assets:
#     MoltenVK-all.tar            ~180 MB
#     MoltenVK-ios.tar            ~34 MB      <- the one we want
#     MoltenVK-macos.tar          ~60 MB
#     MoltenVK-macos-privateapi.tar           (v1.4.1 and later only)
# There is no .xcframework asset, no .zip, and no per-dylib asset. Tags older than
# v1.2.4 carry NO assets at all, and neither do v1.2.11, v1.2.11-rc1 and v1.2.11-b1 -
# for 1.2.11 the artifacts live on the separate tag `v1.2.11-artifacts`. The previous
# version of this script defaulted to MOLTENVK_VERSION=v1.2.11, which has no assets,
# so it could never have downloaded anything.
#
# *** THERE IS NO iOS libMoltenVK.dylib IN ANY RELEASE. ***
#
# Scripts/package_dylibs.sh in MoltenVK's own tree ends with:
#     # App store distribution does not support naked dylibs, so only include a naked
#     # dylib for macOS.
#     copy_dylib "" "macOS"
#     #copy_dylib "-iphoneos" "iOS"
#     #copy_dylib "-iphonesimulator" "iOS-simulator"
# The iOS lines are commented out, upstream and in the Ryujinx fork alike. Docs/
# MoltenVK_Runtime_UserGuide.md agrees: its section heading is literally "Install
# MoltenVK as a Dynamic Library on _macOS_", and the only dylib path it names is
# Package/Latest/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib.
#
# So the old `find -name 'libMoltenVK.dylib' -path '*ios*'` could never match. The only
# shippable *dynamic* iOS artifact is the framework binary inside the dynamic
# xcframework:
#     MoltenVK/dynamic/MoltenVK.xcframework/<ios device slice>/MoltenVK.framework/MoltenVK
# which is an ordinary Mach-O dylib whose LC_ID_DYLIB is @rpath/MoltenVK.framework/
# MoltenVK. This script extracts it, renames it to libMoltenVK.dylib and rewrites the
# install name, because that is the filename Eden opens (below). "No naked dylibs" is
# an App Store *policy*; dyld loads one from inside a bundle perfectly well, and this
# port ships through TrollStore / StikDebug / sideload, not the App Store.
#
# A tidier alternative exists and is NOT taken here because it is a change to Eden
# proper rather than to this script: ship the real MoltenVK.framework and add
# "Frameworks/MoltenVK.framework/MoltenVK" to the candidate list in vulkan_library.cpp.
# Worth doing in a pass that owns that file.
#
# ===========================================================================
# WHY AN EMBEDDED DYLIB AND NOT A STATIC LINK
# ===========================================================================
# src/video_core/vulkan_common/vulkan_library.cpp:28-55 has an iOS branch that tries,
# in order:
#     $LIBVULKAN_PATH
#     GetBundleDirectory()/"Frameworks/libMoltenVK.dylib"     <- THIS ONE (line 32)
#     GetBundleDirectory()/"Frameworks/libvulkan.1.dylib"     (line 34)
#     dlopen(nullptr)                                          <- the static case (line 49)
#
# The bundle dylib is the first real path it tries, and it needs no cooperation from
# the linker. The static alternative requires MoltenVK's vk* symbols to survive into
# the executable's export table so dlopen(nullptr)+dlsym can find them, which needs
# -force_load plus -u roots to survive -dead_strip, and none of that has ever been run
# for this project. Worse, nothing currently links MoltenVK at all on iOS:
# MOLTENVK_LIBRARY (externals/CMakeLists.txt:436-443) is consumed only by
# src/yuzu/CMakeLists.txt, the Qt desktop app, built with ENABLE_QT=OFF.
#
# Eden passes an ABSOLUTE path to dlopen, so the staged install name does not decide
# whether the load succeeds. It is rewritten anyway, so the bundle is internally
# consistent and so anything that ever does link it resolves correctly.
#
# ===========================================================================
# WHICH FORK, AND WHY THE DEFAULT IS NOT KHRONOS
# ===========================================================================
# Eden's own cpmfile.json:162-168 pins MoltenVK to
#     repo    V380-Ori/Ryujinx.MoltenVK
#     version v1.4.1-ryujinx
#     artifact MoltenVK-macOS.tar
# - a fork, not KhronosGroup/MoltenVK. That fork publishes MoltenVK-ios.tar alongside
# MoltenVK-macos.tar at the same tag, so defaulting to it keeps the iOS and macOS
# halves of this project on ONE MoltenVK, which is what you want when a rendering bug
# has to be reproduced on a Mac. Set MOLTENVK_REPO=KhronosGroup/MoltenVK to use
# upstream instead; both layouts are handled, because nothing here is matched by a
# hardcoded path.
#
# Note the remaining upstream gap, which is NOT this script's to fix: externals/
# CMakeLists.txt:433 guards the MoltenVK block with a plain `if (APPLE)`, true for iOS,
# and line 443 then points MOLTENVK_LIBRARY at .../dylib/macOS/libMoltenVK.dylib. An
# iOS configure today resolves MoltenVK to a *macOS* dylib. It is harmless only because
# nothing consumes it. The real fix is an `if (IOS)` branch there plus an iOS artifact
# entry in cpmfile.json.
#
# ===========================================================================
# WHAT THIS SCRIPT VERIFIES, AND WHY EACH CHECK EARNS ITS PLACE
# ===========================================================================
#   arm64             lipo -archs. A simulator-only or x86_64 slice is useless.
#   platform == IOS   vtool -show-build, falling back to otool -l. THE IMPORTANT ONE.
#                     Note the old check was `grep -qiE 'platform (2|IOS)|...'`, which
#                     is unanchored and therefore matches "platform IOSSIMULATOR" - it
#                     would have happily staged a simulator binary. A simulator or
#                     macOS dylib inside an iOS bundle fails at dlopen on the device,
#                     with nothing at build time to warn you.
#   Mach-O dylib      file(1). Catches an .a, a text file, or a 404 page saved as .tar.
#   exports vk*       nm. MoltenVK can be built with MVK_HIDE_VULKAN_SYMBOLS=1 (README,
#                     "Hiding Vulkan API Symbols"); such a build dlopens fine and then
#                     every dlsym returns null. That is a black screen with no error.
#   sane size         a broad band only - the archive was never downloaded here, so the
#                     true size is unmeasured. It is there to catch 0 bytes and 300 MB,
#                     not to be precise.
#
# ===========================================================================
# DISK
# ===========================================================================
# The archive is ~32 MB and expands to several hundred MB, most of it static libraries
# this script does not want. So: the tar is LISTED first, only the handful of members
# that could be the iOS framework binary are extracted, and the scratch directory
# defaults to the same volume as the repo rather than $TMPDIR on the system disk.
# A free-space precheck fails early instead of filling a disk.
#
# If you already have the archive, EDEN_MOLTENVK_TARBALL=<path to MoltenVK-ios.tar>
# skips the download entirely; EDEN_MOLTENVK_DYLIB=<path> skips this script's fetch
# mode altogether and is read directly by --embed.

set -euo pipefail

readonly SELF="fetch-moltenvk.sh"

say()  { echo "$SELF: $*"; }
warn() { echo "$SELF: $*" >&2; }
die()  { echo "$SELF: ERROR: $*" >&2; exit 1; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_DIR="${EDEN_MOLTENVK_STAGE:-$REPO_ROOT/build-ios/moltenvk}"
DYLIB="$STAGE_DIR/libMoltenVK.dylib"
STAMP="$STAGE_DIR/PROVENANCE.txt"

# Broad on purpose; see the header. Bytes.
readonly MIN_BYTES=$((1 * 1024 * 1024))
readonly MAX_BYTES=$((64 * 1024 * 1024))

# ---------------------------------------------------------------------------
# Toolchain. All of these ship with the Command Line Tools; none needs full Xcode.
# ---------------------------------------------------------------------------
require_tools() {
    local missing=()
    local t
    for t in "$@"; do
        command -v "$t" >/dev/null 2>&1 || missing+=("$t")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        die "missing required tool(s): ${missing[*]}
    These ship with the Xcode Command Line Tools. Install them with:
        xcode-select --install"
    fi
}

file_size() {
    # stat(1) is BSD here and GNU on a Linux runner; try both.
    stat -f%z "$1" 2>/dev/null || stat -c%s "$1" 2>/dev/null || echo 0
}

# ---------------------------------------------------------------------------
# describe_platform <single-arch mach-o> -> prints one of IOS / IOSSIMULATOR / MACOS /
# MACCATALYST / TVOS / XROS / UNKNOWN[:detail]
#
# vtool prints the platform symbolically ("platform IOS"); otool prints it numerically
# ("platform 2"). Both are handled because the numeric form is what older otool emits
# and the symbolic form is far harder to misread. Numbers per <mach-o/loader.h>:
# 1 MACOS, 2 IOS, 3 TVOS, 4 WATCHOS, 5 BRIDGEOS, 6 MACCATALYST, 7 IOSSIMULATOR,
# 8 TVOSSIMULATOR, 9 WATCHOSSIMULATOR, 10 DRIVERKIT, 11 VISIONOS, 12 VISIONOSSIMULATOR.
# ---------------------------------------------------------------------------
describe_platform() {
    local f="$1" out plat

    if command -v vtool >/dev/null 2>&1; then
        out="$(vtool -show-build "$f" 2>/dev/null || true)"
        plat="$(printf '%s\n' "$out" | awk '/^[[:space:]]*platform /{print $2; exit}')"
        if [ -n "$plat" ]; then printf '%s\n' "$plat"; return 0; fi
    fi

    out="$(otool -l "$f" 2>/dev/null || true)"
    plat="$(printf '%s\n' "$out" | awk '/^[[:space:]]*platform /{print $2; exit}')"
    case "$plat" in
        1)  echo MACOS;              return 0 ;;
        2)  echo IOS;                return 0 ;;
        3)  echo TVOS;               return 0 ;;
        6)  echo MACCATALYST;        return 0 ;;
        7)  echo IOSSIMULATOR;       return 0 ;;
        8)  echo TVOSSIMULATOR;      return 0 ;;
        11) echo XROS;               return 0 ;;
        12) echo XROSSIMULATOR;      return 0 ;;
        "") : ;;
        *)  echo "UNKNOWN:$plat";    return 0 ;;
    esac

    # No LC_BUILD_VERSION at all. Fall back to the pre-10.14 load commands.
    if printf '%s\n' "$out" | grep -q 'LC_VERSION_MIN_IPHONEOS'; then
        echo IOS_LEGACY; return 0
    fi
    if printf '%s\n' "$out" | grep -q 'LC_VERSION_MIN_MACOSX'; then
        echo MACOS; return 0
    fi
    echo UNKNOWN
}

# ---------------------------------------------------------------------------
# verify_dylib <path> [--quiet]
# Returns 0 if the file is an iphoneos-device arm64 MoltenVK that exports the Vulkan
# entry points. Prints why not on failure. Does not modify the file.
# ---------------------------------------------------------------------------
verify_dylib() {
    local f="$1" quiet="${2:-}" sz archs plat kind
    local r=0
    _v() { [ "$quiet" = "--quiet" ] || warn "  $*"; }

    [ -f "$f" ] || { _v "not a file: $f"; return 1; }

    kind="$(file -b "$f" 2>/dev/null || echo unknown)"
    case "$kind" in
        *"Mach-O"*dynamically*linked*shared*library*) : ;;
        *"Mach-O"*bundle*)                            : ;;
        *"universal binary"*)                         : ;;
        *) _v "not a Mach-O dynamic library: $kind"; return 1 ;;
    esac

    sz="$(file_size "$f")"
    if [ "$sz" -lt "$MIN_BYTES" ] || [ "$sz" -gt "$MAX_BYTES" ]; then
        _v "implausible size: $sz bytes (expected between $MIN_BYTES and $MAX_BYTES)"
        r=1
    fi

    archs="$(lipo -archs "$f" 2>/dev/null || echo '')"
    case " $archs " in
        *" arm64 "*) : ;;
        *) _v "no arm64 slice (lipo -archs: ${archs:-<none>})"; r=1 ;;
    esac

    plat="$(describe_platform "$f")"
    case "$plat" in
        IOS)        : ;;
        IOS_LEGACY) _v "note: LC_VERSION_MIN_IPHONEOS only, no LC_BUILD_VERSION - accepting" ;;
        *)          _v "wrong platform: $plat (need IOS; IOSSIMULATOR and MACOS are not it)"; r=1 ;;
    esac

    if ! nm -g --defined-only "$f" 2>/dev/null | grep -q '_vkGetInstanceProcAddr'; then
        _v "does not export _vkGetInstanceProcAddr - looks like an MVK_HIDE_VULKAN_SYMBOLS=1"
        _v "build. It would dlopen fine and then every dlsym would return null."
        r=1
    fi

    return $r
}

describe_staged() {
    local f="$1"
    printf '%s, %s, %s bytes\n' \
        "$(lipo -archs "$f" 2>/dev/null || echo '?')" \
        "$(describe_platform "$f")" \
        "$(file_size "$f")"
}

# ===========================================================================
# --embed
# ===========================================================================
if [ "${1:-}" = "--embed" ]; then
    APP_DIR="${2:?usage: fetch-moltenvk.sh --embed <path to .app>}"
    require_tools lipo otool nm file

    # An explicit override wins, so a developer with a dylib from elsewhere can point
    # at it without touching this script. Set-but-missing is a typo, not a fallback.
    if [ -n "${EDEN_MOLTENVK_DYLIB:-}" ]; then
        [ -f "$EDEN_MOLTENVK_DYLIB" ] ||
            die "EDEN_MOLTENVK_DYLIB is set to '$EDEN_MOLTENVK_DYLIB', which does not exist."
        DYLIB="$EDEN_MOLTENVK_DYLIB"
    fi

    if [ ! -f "$DYLIB" ]; then
        # NON-FATAL BY DEFAULT, deliberately, and this is a real trade-off rather than
        # laziness. This mode is invoked from a build phase in src/ios/project.yml:247,
        # and the bar for cut one is that the project generates and the target links -
        # a link that does not involve MoltenVK at all (see the header). Failing here
        # would break a milestone that is otherwise met. The app fails at GPU init on a
        # device instead, which is the honest place for a missing runtime dependency to
        # surface, and ci/verify-ipa.sh already reports the empty Frameworks/ as a
        # warning.
        #
        # Set EDEN_MOLTENVK_REQUIRED=1 to make it fatal. Do that for any build meant to
        # actually render.
        warn "============================================================"
        warn "WARNING: no libMoltenVK.dylib staged at"
        warn "    $DYLIB"
        warn ""
        warn "The app will build, link and package. On a device it will reach"
        warn "GPU init, log 'No Vulkan library found' from vulkan_library.cpp:52,"
        warn "and show a black screen."
        warn ""
        warn "Fix: run './ci/fetch-moltenvk.sh' with no arguments first, or set"
        warn "EDEN_MOLTENVK_DYLIB to a local iphoneos arm64 MoltenVK dylib."
        warn "============================================================"
        if [ "${EDEN_MOLTENVK_REQUIRED:-0}" = "1" ]; then
            die "EDEN_MOLTENVK_REQUIRED=1 and no dylib is staged."
        fi
        exit 0
    fi

    # Verify at embed time too. The staged file may predate this script, may have been
    # placed by hand, or may come from EDEN_MOLTENVK_DYLIB. Shipping a wrong dylib is
    # strictly worse than shipping none, because the failure moves from a clear log
    # line to a dlopen that half-works.
    if ! verify_dylib "$DYLIB"; then
        die "refusing to embed $DYLIB - it is not an iphoneos arm64 MoltenVK (see above)."
    fi

    mkdir -p "$APP_DIR/Frameworks"
    cp -f "$DYLIB" "$APP_DIR/Frameworks/libMoltenVK.dylib"
    say "embedded -> $APP_DIR/Frameworks/libMoltenVK.dylib"
    say "           $(describe_staged "$APP_DIR/Frameworks/libMoltenVK.dylib")"

    # NOTE for whoever signs this: codesign does NOT sign nested code automatically.
    # Frameworks/libMoltenVK.dylib must be signed BEFORE the enclosing .app, or the
    # bundle signature is invalid and the app will not launch.
    exit 0
fi

# ===========================================================================
# fetch + verify + stage
# ===========================================================================
require_tools curl tar lipo otool nm file install_name_tool

mkdir -p "$STAGE_DIR"

# Do not blindly trust an existing file - an interrupted earlier run can leave a short
# one, and a hand-placed one can be the macOS dylib.
if [ -f "$DYLIB" ]; then
    if verify_dylib "$DYLIB" --quiet; then
        say "already staged and verified: $DYLIB"
        say "  $(describe_staged "$DYLIB")"
        exit 0
    fi
    warn "a file exists at $DYLIB but does not verify; re-fetching."
    verify_dylib "$DYLIB" || true
    rm -f "$DYLIB"
fi

MOLTENVK_REPO="${MOLTENVK_REPO:-V380-Ori/Ryujinx.MoltenVK}"
# Matches Eden's own pin, cpmfile.json:167. Keep the two in step when bumping.
if [ "$MOLTENVK_REPO" = "V380-Ori/Ryujinx.MoltenVK" ]; then
    MOLTENVK_VERSION="${MOLTENVK_VERSION:-v1.4.1-ryujinx}"
else
    MOLTENVK_VERSION="${MOLTENVK_VERSION:-v1.4.2}"
fi
ASSET_NAME="${MOLTENVK_ASSET:-MoltenVK-ios.tar}"

# Scratch on the same volume as the repo, not $TMPDIR. The repo commonly lives on an
# external work volume precisely because the system disk has no room.
SCRATCH_PARENT="${EDEN_MOLTENVK_TMP:-$STAGE_DIR}"
mkdir -p "$SCRATCH_PARENT"
TMP="$(mktemp -d "$SCRATCH_PARENT/fetch.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# Free-space precheck. ~32 MB for the tar plus room for the members we pull out.
AVAIL_KB="$(df -Pk "$TMP" | awk 'NR==2 {print $4}')"
REQUIRED_KB=$((250 * 1024))
if [ -n "${AVAIL_KB:-}" ] && [ "$AVAIL_KB" -lt "$REQUIRED_KB" ]; then
    die "only $((AVAIL_KB / 1024)) MB free on the volume holding $TMP; need about $((REQUIRED_KB / 1024)) MB.
    Point EDEN_MOLTENVK_TMP at a volume with room, or set EDEN_MOLTENVK_DYLIB to a
    dylib you already have and skip the download."
fi

say "repo    $MOLTENVK_REPO"
say "tag     $MOLTENVK_VERSION"
say "asset   $ASSET_NAME"

# ---------------------------------------------------------------------------
# Resolve the asset through the releases API rather than guessing a URL. The API is
# authoritative, tells us the size up front, and its failure modes are legible: a tag
# with no assets (v1.2.11) reports an empty list instead of a 404 on a URL that looks
# plausible.
# ---------------------------------------------------------------------------
TARBALL="$TMP/moltenvk.tar"

# EDEN_MOLTENVK_TARBALL short-circuits the download for a tar you already have. The
# machine driving this port has no room to download a 32 MB archive, and an air-gapped
# or rate-limited runner has the same problem for a different reason. Everything after
# this point - listing, ranking, extraction, verification - is identical either way, so
# this is a real supported path and not a test hook.
if [ -n "${EDEN_MOLTENVK_TARBALL:-}" ]; then
    [ -f "$EDEN_MOLTENVK_TARBALL" ] ||
        die "EDEN_MOLTENVK_TARBALL is set to '$EDEN_MOLTENVK_TARBALL', which does not exist."
    say "using local archive $EDEN_MOLTENVK_TARBALL (skipping the download)"
    ASSET_URL="file://$EDEN_MOLTENVK_TARBALL"
    cp -f "$EDEN_MOLTENVK_TARBALL" "$TARBALL"
else

API="https://api.github.com/repos/$MOLTENVK_REPO/releases/tags/$MOLTENVK_VERSION"
# NOTE the ${AUTH[@]+"${AUTH[@]}"} idiom at the call site. macOS ships bash 3.2, where
# `set -u` treats "${AUTH[@]}" on an EMPTY array as an unbound variable and aborts. The
# plain form worked here only when GITHUB_TOKEN happened to be set, which is the kind
# of bug that passes on a runner and fails on a laptop.
AUTH=()
[ -n "${GITHUB_TOKEN:-}" ] && AUTH=(-H "Authorization: Bearer $GITHUB_TOKEN")

RELEASE_JSON="$TMP/release.json"
if ! curl -sfL ${AUTH[@]+"${AUTH[@]}"} -H 'Accept: application/vnd.github+json' "$API" -o "$RELEASE_JSON"; then
    die "the GitHub API has no release tagged '$MOLTENVK_VERSION' in $MOLTENVK_REPO.
    $API returned an error.
    List the real tags with:
        gh api repos/$MOLTENVK_REPO/releases --jq '.[].tag_name'
    If you are rate-limited, set GITHUB_TOKEN."
fi

# Deliberately no jq: it is not on a stock macOS runner. python3 is.
ASSET_URL="$(python3 - "$RELEASE_JSON" "$ASSET_NAME" <<'PY'
import json, sys
with open(sys.argv[1]) as fh:
    rel = json.load(fh)
want = sys.argv[2]
for a in rel.get("assets", []):
    if a.get("name") == want:
        print(a.get("browser_download_url", ""))
        break
PY
)"

if [ -z "$ASSET_URL" ]; then
    warn "release '$MOLTENVK_VERSION' in $MOLTENVK_REPO has no asset named '$ASSET_NAME'."
    warn "--- assets this release actually publishes ---"
    python3 - "$RELEASE_JSON" >&2 <<'PY'
import json, sys
with open(sys.argv[1]) as fh:
    rel = json.load(fh)
assets = rel.get("assets", [])
if not assets:
    print("  (none - this tag publishes no assets at all)")
for a in assets:
    print("  %-32s %10d bytes" % (a.get("name"), a.get("size", 0)))
PY
    die "no usable asset. Pick one of the above with MOLTENVK_ASSET=, or a tag that has
    assets with MOLTENVK_VERSION=. Note that several MoltenVK tags (v1.2.11,
    v1.2.11-rc1, v1.2.11-b1, and everything before v1.2.4) carry no assets; for
    1.2.11 the artifacts live on the separate tag 'v1.2.11-artifacts'."
fi

say "url     $ASSET_URL"

curl -fL --retry 3 --retry-delay 2 --progress-bar "$ASSET_URL" -o "$TARBALL" ||
    die "download failed: $ASSET_URL"

TAR_BYTES="$(file_size "$TARBALL")"
[ "$TAR_BYTES" -gt $((4 * 1024 * 1024)) ] ||
    die "downloaded $TAR_BYTES bytes, which is far too small to be $ASSET_NAME.
    That is usually an error page saved under the right filename. First bytes:
    $(head -c 200 "$TARBALL" | tr -d '\0')"

say "downloaded $((TAR_BYTES / 1024 / 1024)) MB"
fi

# ---------------------------------------------------------------------------
# List before extracting. The archive expands to several hundred MB, nearly all of it
# static libraries and headers we do not want, so only candidate members come out.
# ---------------------------------------------------------------------------
LISTING="$TMP/listing.txt"
tar -tf "$TARBALL" > "$LISTING" 2>/dev/null || die "cannot read $ASSET_NAME as a tar archive."

# Ranked candidates. Nothing is a hardcoded path: the slice directory name is chosen by
# xcodebuild -create-xcframework (Scripts/create_xcframework_func.sh) and differs
# between MoltenVK versions and between forks. Ranking only decides what is tried
# first; verify_dylib is what actually decides.
#   1. the framework binary on an iOS device slice
#   2. any naked libMoltenVK.dylib on an iOS path   (in case a fork re-enables it)
#   3. any framework binary at all                  (verification will reject the wrong one)
#   4. any libMoltenVK.dylib at all
# 'simulator' and 'maccatalyst' are excluded by name in the first two; visionOS slices
# are named xros-* and so do not contain "ios".
CANDIDATES="$TMP/candidates.txt"
: > "$CANDIDATES"
{
    grep -E '/MoltenVK\.framework/MoltenVK$' "$LISTING" | grep -i 'ios' \
        | grep -vi 'simulator' | grep -vi 'maccatalyst' || true
    grep -E '/libMoltenVK\.dylib$' "$LISTING" | grep -i 'ios' \
        | grep -vi 'simulator' | grep -vi 'maccatalyst' || true
    grep -E '/MoltenVK\.framework/MoltenVK$' "$LISTING" || true
    grep -E '/libMoltenVK\.dylib$' "$LISTING" || true
} | awk '!seen[$0]++' > "$CANDIDATES"

if [ ! -s "$CANDIDATES" ]; then
    warn "no MoltenVK dynamic library of any kind inside $ASSET_NAME."
    warn "--- top of the archive listing ---"
    head -60 "$LISTING" >&2
    warn "--- everything that looks like a library ---"
    grep -E '\.(dylib|a|framework)' "$LISTING" | head -40 >&2 || true
    die "archive layout is not what this script expects. Read the listing above; the
    member you want is the framework binary at
        .../dynamic/MoltenVK.xcframework/<ios device slice>/MoltenVK.framework/MoltenVK"
fi

say "candidate members:"
sed 's/^/  /' "$CANDIDATES"

FOUND=""
FOUND_MEMBER=""
while IFS= read -r member; do
    [ -n "$member" ] || continue
    rm -rf "$TMP/x"; mkdir -p "$TMP/x"
    if ! tar -xf "$TARBALL" -C "$TMP/x" "$member" 2>/dev/null; then
        warn "could not extract $member"
        continue
    fi
    cand="$TMP/x/$member"
    [ -f "$cand" ] || continue

    # Thin a universal slice down to arm64 before inspecting the platform; vtool and
    # otool report per-architecture, and a thin file keeps the checks unambiguous.
    if lipo -info "$cand" 2>/dev/null | grep -q 'Architectures in the fat file'; then
        if lipo -thin arm64 "$cand" -output "$cand.arm64" 2>/dev/null; then
            mv -f "$cand.arm64" "$cand"
        fi
    fi

    say "checking $member"
    if verify_dylib "$cand"; then
        FOUND="$cand"
        FOUND_MEMBER="$member"
        say "  OK"
        break
    fi
    warn "  rejected"
done < "$CANDIDATES"

if [ -z "$FOUND" ]; then
    warn "every candidate failed verification; the reasons are printed above."
    warn "--- archive members that look like libraries ---"
    grep -E '\.(dylib|a)$|/MoltenVK$' "$LISTING" | head -40 >&2 || true
    die "no iphoneos arm64 MoltenVK in $ASSET_NAME from $MOLTENVK_REPO@$MOLTENVK_VERSION.
    Refusing to stage anything. Staging the wrong dylib would produce an app that
    dlopens a library it cannot use and renders a black screen with no explanation,
    which is strictly worse than the current honest 'No Vulkan library found'."
fi

cp -f "$FOUND" "$DYLIB"
chmod u+w "$DYLIB"

# The framework binary's LC_ID_DYLIB is @rpath/MoltenVK.framework/MoltenVK. Eden opens
# an absolute path so dlopen does not care, but leaving a stale id pointing at a
# framework that is not in the bundle is a trap for anything that later links it.
# Fatal on failure: a silent skip leaves exactly that trap.
#
# This edits the Mach-O and so invalidates any signature it carried. That is fine and
# expected - see the signing note in --embed; the dylib is signed after staging, before
# the enclosing .app.
install_name_tool -id "@rpath/libMoltenVK.dylib" "$DYLIB" ||
    die "install_name_tool could not rewrite the install name of $DYLIB"

# Re-verify the staged copy. lipo -thin and install_name_tool both rewrite the file;
# checking the artifact we are actually shipping costs nothing.
verify_dylib "$DYLIB" || die "the staged copy at $DYLIB failed verification after rewriting."

{
    echo "repo    $MOLTENVK_REPO"
    echo "tag     $MOLTENVK_VERSION"
    echo "asset   $ASSET_NAME"
    echo "url     $ASSET_URL"
    echo "member  $FOUND_MEMBER"
    echo "archs   $(lipo -archs "$DYLIB" 2>/dev/null)"
    echo "platform $(describe_platform "$DYLIB")"
    echo "bytes   $(file_size "$DYLIB")"
    echo "sha256  $(shasum -a 256 "$DYLIB" 2>/dev/null | cut -d' ' -f1)"
    echo "staged  $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} > "$STAMP"

say "staged  $DYLIB"
say "        $(describe_staged "$DYLIB")"
say "        provenance in $STAMP"
