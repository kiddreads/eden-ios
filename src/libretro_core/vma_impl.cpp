// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// VulkanMemoryAllocator is header-only: vma.h declares everything and compiles the
// implementation only in a translation unit that defines VMA_IMPLEMENTATION first.
// Eden does that once per FRONTEND - src/yuzu/main_window.cpp, src/yuzu_cmd/yuzu.cpp,
// src/android/app/src/main/jni/native.cpp, src/tests - never in video_core itself.
//
// eden_libretro is a frontend, and it did not do this. So libvideo_core.a referenced
// vmaCreateAllocator, vmaCreateBuffer, vmaCreateImage and the rest, nothing defined
// them, and the app failed at link with a wall of undefined vma* symbols. It could
// only ever have appeared at the final link, because a static library is not linked.
//
// This file exists purely to be that translation unit. It must stay the only one in
// this target, or the symbols become duplicates instead of missing.

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
