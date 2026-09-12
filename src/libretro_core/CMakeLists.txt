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

create_target_directory_groups(eden_libretro)

# The app links this target and includes "eden_libretro.h" and "libretro.h".
target_include_directories(eden_libretro PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# core is PUBLIC because eden_libretro.h's siblings expose Core::System types.
# frontend_common and audio_core are deliberately NOT linked:
#   * frontend_common would be needed for ContentManager::AreKeysPresent and
#     FirmwareManager, but firmware_manager.h includes
#     core/hle/service/am/frontend/applet_mii_edit.h and I have not verified what that
#     drags in. retro_content.cpp calls Core::Crypto::KeyManager directly instead.
#   * audio_core would be needed only once a real libretro sink exists; the current
#     audio path emits silence and touches nothing in audio_core.
target_link_libraries(eden_libretro
    PUBLIC core
    PRIVATE common input_common hid_core video_core network)

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
