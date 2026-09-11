# SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
# SPDX-License-Identifier: GPL-3.0-or-later

## iOS / iPadOS toolchain ##
#
# CMake has first-class iOS support and externals/cmake-modules/DetectPlatform.cmake
# explicitly defers to it ("Apple, Windows, Android, etc. are not covered, as CMake
# already does that for us"), so this file does not reimplement a cross-compiler setup.
# It pins the target and then states the things about iOS that Eden's build cannot
# infer on its own.
#
# Use:
#   cmake -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=CMakeModules/toolchains/iOS.cmake \
#     [feature flags - see .github/workflows/build-ios-core.yml]
#
# Note that `APPLE` is ON for iOS as well as macOS, so every existing `if (APPLE)`
# branch in this codebase now also applies here. Several of them are macOS-specific
# and will need splitting on `IOS` as the port progresses - that is expected work,
# not a bug in this file.

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)

# Device only. The simulator is x86_64/arm64-simulator and cannot run a JIT
# meaningfully, so it is not a target we pretend to support.
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "iOS target architecture" FORCE)
set(CMAKE_OSX_SYSROOT iphoneos CACHE STRING "iOS sysroot" FORCE)

# iOS 15 is the floor deliberately: it keeps older hardware in scope, which is a
# project goal. Newer OS capabilities are gated behind availability checks rather
# than by raising this number. The root CMakeLists sets 15.0 for macOS already;
# this overrides it for the iOS target so the two cannot drift apart.
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "iOS deployment target" FORCE)

# iOS will not load a dynamic library the app did not ship inside its own signed
# bundle, and will not load one at all that was produced at runtime. Everything
# must therefore be linked statically into the binary.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" CACHE STRING "" FORCE)

# Bitcode has been dead since Xcode 14 and actively breaks hand-written assembly,
# of which dynarmic has plenty.
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE NO CACHE STRING "" FORCE)

# try_compile builds a full app bundle by default when targeting iOS, which fails
# under a plain Ninja/Makefile generator during dependency probing.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Let CMake find host tools (cmake, ninja, python, pkg-config) on the Mac while
# still looking for libraries and headers only inside the iOS sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

## iOS-specific facts the rest of the build should be able to ask about ##

# Host page size is 16 KiB on Apple silicon; the Switch's guest page size is 4 KiB.
# yuzu-lineage "fastmem" maps guest memory directly into the host address space and
# needs host pages no larger than guest pages, so it cannot work here as written.
# The port starts on the soft-MMU path. This is surfaced as a cache variable rather
# than hidden in a workflow so that anyone configuring by hand gets the same answer.
set(EDEN_IOS_HOST_PAGE_SIZE 16384 CACHE STRING "Apple silicon host page size" FORCE)
set(EDEN_IOS_FASTMEM_USABLE OFF CACHE BOOL "fastmem needs host pages <= 4 KiB" FORCE)

# dynarmic writes to pages it later executes. On Apple silicon that requires
# MAP_JIT plus pthread_jit_write_protect_np() around every write, and the process
# must be permitted to have RWX pages at all (StikDebug's CS_DEBUGGED, or the
# entitlements embedded in a TrollStore build).
set(EDEN_IOS_NEEDS_MAP_JIT ON CACHE BOOL "" FORCE)

# There is no Vulkan on iOS - only Metal, with Vulkan emulated by MoltenVK.
set(EDEN_IOS_VULKAN_VIA_MOLTENVK ON CACHE BOOL "" FORCE)

message(STATUS "eden-ios: targeting iOS ${CMAKE_OSX_DEPLOYMENT_TARGET} arm64 (device)")
message(STATUS "eden-ios: fastmem disabled - 16 KiB host pages vs 4 KiB guest pages")
