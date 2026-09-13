# SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Derived from suyu's src/libretro_core/CMakeLists.txt (suyu-emu/suyu-v0.0.4),
# GPL-3.0-or-later. suyu builds a SHARED library; iOS will not load a dynamic library
# the app did not ship inside its own signed bundle, and will not load one produced at
# runtime at all, so a libretro core on iOS is linked into the frontend executable.
# CMakeModules/toolchains/iOS.cmake already forces BUILD_SHARED_LIBS OFF for the same
# reason.
#
# libretro.h is NOT authored here. It is the standard upstream libretro ABI header,
# vendored verbatim - see this directory's provenance note in the port README and the
# fetch step in the workflow. Do not hand-write or trim it.

add_library(eden_libretro STATIC
    eden_libretro.h
    libretro.h
    retro_audio.cpp
    retro_audio.h
    retro_content.cpp
    retro_content.h
    retro_core.cpp
    retro_core_state.h
    retro_emu_window.cpp
    retro_emu_window.h
    retro_input.cpp
    retro_input.h
    retro_video.cpp
    retro_video.h
    vma_impl.cpp
)

# VulkanMemoryAllocator's implementation does not compile clean under Eden's -Werror
# set - vk_mem_alloc.h has unused variables of its own. That is vendored third-party
# code: the warnings are not ours to fix, and -Werror on someone else's header only
# means we cannot use the library at all. Scoped to this one translation unit, so the
# project's own sources keep every warning they had.
set_source_files_properties(vma_impl.cpp PROPERTIES COMPILE_OPTIONS
    "-Wno-unused-variable;-Wno-unused-parameter;-Wno-missing-field-initializers;-Wno-nullability-completeness;-Wno-unused-function")

create_target_directory_groups(eden_libretro)

# The app links this target and includes "eden_libretro.h" and "libretro.h".
target_include_directories(eden_libretro PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# core is PUBLIC because eden_libretro.h's siblings expose Core::System types.
#
# frontend_common IS now linked (2026-09-13; it was not before). The reason it was
# excluded no longer applies: the concern was about OUR OWN SOURCE including
# firmware_manager.h, whose applet_mii_edit.h drag-in was unverified - retro_content.cpp
# never does that, it calls Core::Crypto::KeyManager directly, so nothing here ever
# touches frontend_common's headers. Linking the already-built archive is a different
# question, and the real app link needs it regardless of any header question:
# src/core/hle/service/ns/query_service.cpp calls PlayTime::PlayTimeManager, which lives
# in frontend_common (play_time_manager.cpp), and core.a does not embed it - two
# separate static archives, so the symbol is only resolved if something downstream of
# core links frontend_common too. Verified play_time_manager.h pulls in nothing beyond
# common/ headers, and frontend_common's only unconditional dependencies (core, common,
# and the header-only SimpleIni/frozen interface libraries) are already satisfied here -
# ENABLE_UPDATE_CHECKER is OFF in this build, so its httplib/nlohmann_json/OpenSSL
# dependencies never enter the picture.
#
# audio_core is still not linked directly. A previous pass wired a real libretro audio
# sink into it under ENABLE_LIBRETRO, but its symbols were already reported to resolve
# transitively - eden_libretro links core PUBLIC, and src/core/CMakeLists.txt links
# audio_core PRIVATE, which reaches this target as LINK_ONLY and lands after
# eden_libretro in the static link line. Add it directly here only if a build ever shows
# an audio_core symbol undefined.
target_link_libraries(eden_libretro
    PUBLIC core
    PRIVATE common input_common hid_core video_core network frontend_common)

target_link_libraries(eden_libretro PRIVATE ${PLATFORM_LIBRARIES})

# The retro_* entry points must reach the final link; everything else can stay hidden.
# libretro.h already marks them __attribute__((visibility("default"))) through
# RETRO_API, so this only narrows the rest.
set_target_properties(eden_libretro PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# TODO(build): symbol prefixing. iOS cannot dlopen a core, so retro_init, retro_run and
# friends are linked directly into the app binary with their unprefixed C names. That
# is fine while there is exactly one core; the moment a second is present it is a
# duplicate-symbol link failure. The fix is a per-core symbol prefix plus a dispatch
# struct handed to the frontend. It is a build-system change, not a source change, but
# it has to be decided before anyone ships a multi-core frontend.
