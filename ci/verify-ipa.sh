#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Gate an IPA before it is uploaded or released.
#
#   verify-ipa.sh <path to .ipa> [expected-entitlements.plist]
#
# Adapted from /Users/kiddreads/cemu-ios-muffin/ci/verify-ipa.sh. Two changes from that
# script are deliberate and are the reason this is not a straight copy - see checks 9
# and 11.
#
# The premise, learned on this project rather than assumed: a green build can ship an
# empty archive. .github/workflows/build-ios-core.yml grew a whole step ("Check
# libdynarmic.a actually contains the arm64 backend") because every backend/arm64 source
# was being wrapped in `#if defined(ARCHITECTURE_ARM64)` while the build defined
# ARCHITECTURE_arm64 - libdynarmic.a built green with no JIT backend in it. "It
# compiled" is not the claim worth gating on.

set -uo pipefail

IPA="${1:?usage: verify-ipa.sh <ipa> [expected-entitlements.plist]}"
EXPECTED_ENTS="${2:-}"

FAILURES=0
CHECKS=0

pass() { CHECKS=$((CHECKS+1)); echo "  ok    $*"; }
fail() { CHECKS=$((CHECKS+1)); FAILURES=$((FAILURES+1)); echo "  FAIL  $*"; }
info() { echo "        $*"; }

echo "verify-ipa.sh: $IPA"

if [ ! -f "$IPA" ]; then
    echo "verify-ipa.sh: no such file" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

unzip -qq "$IPA" -d "$WORK" || { echo "verify-ipa.sh: could not unzip" >&2; exit 1; }

# --- 1. Payload layout ------------------------------------------------------
if [ -d "$WORK/Payload" ]; then
    pass "top-level Payload/ directory"
else
    fail "no top-level Payload/ directory - an IPA must have one"
    exit 1
fi

# --- 2. exactly one .app ----------------------------------------------------
APP_COUNT="$(find "$WORK/Payload" -maxdepth 1 -name '*.app' | wc -l | tr -d ' ')"
APP="$(find "$WORK/Payload" -maxdepth 1 -name '*.app' | head -1)"
if [ "$APP_COUNT" = "1" ]; then
    pass "exactly one .app ($(basename "$APP"))"
else
    fail "expected 1 .app in Payload/, found $APP_COUNT"
    exit 1
fi

# --- 3. no symlinks ---------------------------------------------------------
# iOS refuses to install a bundle containing symlinks.
SYMLINKS="$(find "$APP" -type l | wc -l | tr -d ' ')"
if [ "$SYMLINKS" = "0" ]; then
    pass "no symlinks in the bundle"
else
    fail "$SYMLINKS symlink(s) in the bundle"
    find "$APP" -type l | head -10 | sed 's/^/        /'
fi

# --- 4. no case-insensitive filename collisions -----------------------------
# The device filesystem is case-sensitive; the build machine's usually is not, so two
# files differing only in case survive packaging and collide on install.
COLLISIONS="$(find "$APP" | tr 'A-Z' 'a-z' | sort | uniq -d | wc -l | tr -d ' ')"
if [ "$COLLISIONS" = "0" ]; then
    pass "no case-insensitive filename collisions"
else
    fail "$COLLISIONS case-insensitive collision(s)"
    find "$APP" | tr 'A-Z' 'a-z' | sort | uniq -d | head -10 | sed 's/^/        /'
fi

# --- 5. Info.plist and CFBundleExecutable -----------------------------------
PLIST="$APP/Info.plist"
if [ -f "$PLIST" ]; then
    pass "Info.plist present"
else
    fail "no Info.plist"
    exit 1
fi

EXEC_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$PLIST" 2>/dev/null || true)"
if [ -n "$EXEC_NAME" ] && [ -f "$APP/$EXEC_NAME" ]; then
    pass "CFBundleExecutable = $EXEC_NAME, and it exists"
else
    fail "CFBundleExecutable missing or does not name a real file (got '$EXEC_NAME')"
    exit 1
fi
BIN="$APP/$EXEC_NAME"

# --- 6. exec bit ------------------------------------------------------------
if [ -x "$BIN" ]; then
    pass "executable bit set on $EXEC_NAME"
else
    fail "$EXEC_NAME is not executable - zip did not preserve the mode"
fi

# --- 7. deployment target ---------------------------------------------------
# CMakeModules/toolchains/iOS.cmake pins 15.0 and says why: "iOS 15 is the floor
# deliberately: it keeps older hardware in scope, which is a project goal."
MIN_OS="$(/usr/libexec/PlistBuddy -c 'Print :MinimumOSVersion' "$PLIST" 2>/dev/null || true)"
if [ "$MIN_OS" = "15.0" ]; then
    pass "MinimumOSVersion = 15.0"
else
    fail "MinimumOSVersion is '$MIN_OS', expected 15.0"
fi

# --- 8. iPhone AND iPad -----------------------------------------------------
# docs/IOS_PORT_NOTES.md: "iPhone AND iPad. A-series AND M-series equal."
FAMILIES="$(/usr/libexec/PlistBuddy -c 'Print :UIDeviceFamily' "$PLIST" 2>/dev/null | tr -d ' ' | tr '\n' ' ')"
if echo "$FAMILIES" | grep -q 1 && echo "$FAMILIES" | grep -q 2; then
    pass "UIDeviceFamily includes iPhone (1) and iPad (2)"
else
    fail "UIDeviceFamily does not cover both idioms: $FAMILIES"
fi

# --- 8b. the two Files.app keys --------------------------------------------
# These are why this check exists at all: cemu-ios-muffin confirmed on a real IPA that
# xcodebuild's GENERATE_INFOPLIST_FILE + INFOPLIST_FILE merge silently DROPS
# UIFileSharingEnabled while keeping LSSupportsOpeningDocumentsInPlace. Checking the
# built artifact rather than the pbxproj is the only way to catch that.
for key in UIFileSharingEnabled LSSupportsOpeningDocumentsInPlace; do
    VALUE="$(/usr/libexec/PlistBuddy -c "Print :$key" "$PLIST" 2>/dev/null || true)"
    if [ "$VALUE" = "true" ]; then
        pass "$key = true"
    else
        fail "$key is '$VALUE', expected true - without it the data root is invisible in Files.app"
    fi
done

# --- 9. arm64 Mach-O executable --------------------------------------------
if lipo -archs "$BIN" 2>/dev/null | grep -qw arm64; then
    pass "binary is arm64"
else
    fail "binary is not arm64: $(lipo -archs "$BIN" 2>&1)"
fi
if otool -h "$BIN" 2>/dev/null | tail -1 | grep -qE '\s2\s'; then
    pass "Mach-O filetype 2 (MH_EXECUTE)"
else
    info "could not confirm MH_EXECUTE from otool -h; not failing on it"
fi

# --- 9b. THE LIBRETRO ENTRY POINTS ARE STILL IN THE BINARY ------------------
# NOT IN CEMU'S SCRIPT. Added for the reason in the file header: a library can archive
# and link perfectly while containing nothing, and this project has already shipped one
# that did. ci/generate-link-flags.sh force-loads libeden_libretro.a; this is what
# checks the force-load actually worked.
MISSING_SYMS=""
for sym in _retro_init _retro_run _retro_load_game _eden_libretro_set_metal_layer _eden_libretro_resize _eden_libretro_set_visible _eden_libretro_set_data_root; do
    # -g --defined-only: global symbols that this binary defines. Darwin's nm has
    # historically read -U as "undefined only" and llvm-nm reads it as --defined-only,
    # so the long form is spelled out rather than relying on which nm is on PATH.
    if ! nm -g --defined-only "$BIN" 2>/dev/null | grep -q " $sym\$"; then
        MISSING_SYMS="$MISSING_SYMS $sym"
    fi
done
if [ -z "$MISSING_SYMS" ]; then
    pass "libretro entry points present in the binary"
else
    fail "libretro symbols absent from the binary:$MISSING_SYMS"
    info "the core did not make it into the link - check ci/generate-link-flags.sh"
fi

# --- 10. MoltenVK reachable by a route vulkan_library.cpp accepts -----------
# src/video_core/vulkan_common/vulkan_library.cpp:28-53 tries, on iOS:
#   $LIBVULKAN_PATH, <bundle>/Frameworks/libMoltenVK.dylib,
#   <bundle>/Frameworks/libvulkan.1.dylib, then dlopen(nullptr).
if [ -f "$APP/Frameworks/libMoltenVK.dylib" ] || [ -f "$APP/Frameworks/libvulkan.1.dylib" ]; then
    pass "Vulkan loader present in Frameworks/"
    for d in "$APP/Frameworks"/*.dylib; do
        [ -f "$d" ] || continue
        info "$(basename "$d"): $(lipo -archs "$d" 2>/dev/null)"
    done
elif nm -g --defined-only "$BIN" 2>/dev/null | grep -q '_vkGetInstanceProcAddr'; then
    pass "MoltenVK linked statically (dlopen(nullptr) path)"
else
    # WARNING, not a failure. The app links and packages without MoltenVK - nothing in
    # the iOS build references a vk* symbol at link time, because MOLTENVK_LIBRARY is
    # consumed only by src/yuzu (ENABLE_QT=OFF). This fails at GPU init on a device,
    # not here. Cut one is expected to hit this.
    info "WARNING: no Vulkan loader in the bundle and none linked in."
    info "         The app will launch and fail at GPU init. See ci/fetch-moltenvk.sh."
fi

# --- 11. CFBundle-reserved directory names at the bundle root ---------------
# CHANGED FROM CEMU'S VERSION, DELIBERATELY.
#
# cemu-ios-muffin/ci/verify-ipa.sh's RESERVED list is
#   'Contents|Resources|Support Files|MacOS|Frameworks|PlugIns|SharedFrameworks|SharedSupport|Versions'
# and it fails on any of those at the bundle root. That list would REJECT A CORRECT
# eden-ios BUNDLE: Frameworks/ at the root of a flat iOS bundle is exactly where
# embedded dylibs belong, and it is precisely where vulkan_library.cpp looks
# (GetBundleDirectory()/"Frameworks/libMoltenVK.dylib").
#
# The hazard that check was written for is CFBundle's bundle-version probe, which tests
# for Contents/ and Resources/ - a data directory named `resources` colliding
# case-insensitively with `Resources` is what cost that project two releases.
# Frameworks/ and PlugIns/ are not part of that probe, so they are not the hazard.
# They are removed from the list here.
RESERVED_HITS=""
for name in Contents Resources "Support Files" MacOS SharedFrameworks SharedSupport Versions; do
    if [ -e "$APP/$name" ]; then
        RESERVED_HITS="$RESERVED_HITS $name"
    fi
done
if [ -z "$RESERVED_HITS" ]; then
    pass "no CFBundle-reserved directory at the bundle root"
else
    fail "reserved name(s) at the bundle root:$RESERVED_HITS"
fi

# --- 12. entitlements -------------------------------------------------------
if [ -z "$EXPECTED_ENTS" ]; then
    info "no expected-entitlements file given; skipping the signature comparison"
else
    EMBEDDED="$WORK/embedded.plist"
    if codesign -d --entitlements :- --xml "$APP" > "$EMBEDDED" 2>/dev/null && [ -s "$EMBEDDED" ]; then
        MISSING_ENTS=""
        # PlistBuddy cannot iterate keys directly; plutil converts to a readable form.
        for key in $(plutil -convert xml1 -o - "$EXPECTED_ENTS" | grep -oE '<key>[^<]+</key>' | sed 's/<\/*key>//g'); do
            if ! grep -q "<key>$key</key>" "$EMBEDDED"; then
                MISSING_ENTS="$MISSING_ENTS $key"
            fi
        done
        if [ -z "$MISSING_ENTS" ]; then
            pass "signature carries every expected entitlement key"
        else
            fail "entitlement key(s) missing from the signature:$MISSING_ENTS"
        fi

        # DER form. iOS 15+ reads a signature carrying only the XML blob as declaring NO
        # entitlements, while codesign still happily prints the XML back - so the
        # readback above is not sufficient on its own and the signing step must pass
        # --generate-entitlement-der.
        #
        # UNVERIFIED: CSMAGIC_EMBEDDED_DER_ENTITLEMENTS = 0xfade7172 is from memory of
        # Apple's cs_blobs.h, not read from a header on this machine. If the constant is
        # wrong this check fails every signed IPA rather than passing a bad one, so it
        # fails loudly rather than silently - but confirm it against a real signed build
        # before trusting it. Reported as info, not a failure, for that reason.
        if codesign -d --verbose=4 "$APP" 2>&1 | grep -qi 'DER'; then
            info "signature appears to carry DER entitlements"
        else
            info "NOTE: could not confirm DER entitlements. iOS 15+ ignores XML-only"
            info "      entitlements. Ensure codesign was given --generate-entitlement-der."
        fi
    else
        # An unsigned variant is a legitimate artifact (CI builds with
        # CODE_SIGNING_ALLOWED=NO), so this is only a failure when entitlements were
        # expected.
        fail "expected entitlements but the bundle carries no signature"
    fi
fi

# --- 13. a real NSBundle probe ---------------------------------------------
# Everything above reads the bundle as files. This asks CFBundle whether it agrees that
# this is a loadable bundle, which is the question that actually matters on device.
cat > "$WORK/probe.m" <<'PROBE'
#import <Foundation/Foundation.h>
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 2) { return 2; }
        NSBundle *b = [NSBundle bundleWithPath:[NSString stringWithUTF8String:argv[1]]];
        if (b == nil) { fprintf(stderr, "NSBundle refused the path\n"); return 1; }
        NSString *ident = [b bundleIdentifier];
        if (ident == nil) { fprintf(stderr, "no bundle identifier\n"); return 1; }
        fprintf(stdout, "%s\n", [ident UTF8String]);
        return 0;
    }
}
PROBE

if clang -framework Foundation -o "$WORK/probe" "$WORK/probe.m" 2>/dev/null; then
    if IDENT="$("$WORK/probe" "$APP" 2>&1)"; then
        pass "CFBundle loads the bundle (identifier: $IDENT)"
    else
        fail "CFBundle refused the bundle: $IDENT"
    fi
else
    info "could not build the NSBundle probe on this host; skipping"
fi

# --- summary ----------------------------------------------------------------
echo
echo "verify-ipa.sh: $((CHECKS - FAILURES))/$CHECKS checks passed"
if [ "$FAILURES" -gt 0 ]; then
    echo "verify-ipa.sh: FAILED ($FAILURES)"
    exit 1
fi
echo "verify-ipa.sh: OK"
