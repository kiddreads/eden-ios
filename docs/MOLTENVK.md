<!--
SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# MoltenVK on iOS: what ships, and what Eden asks for that Apple GPUs cannot give

`docs/IOS_PORT_NOTES.md` lists under **Unverified**:

> Which mandatory Vulkan features MoltenVK actually lacks on which Apple GPU family.
> Nothing in this port has read MoltenVK's source or run it.

This document reads MoltenVK's source. It does not run it — nothing in this project has run
anything on a device yet, and no claim here should be read as if it had.

## How to read this

Every section is one of two kinds, and they are never mixed in a sentence:

- **VERIFIED** — read out of a file. Eden citations are `path:line` against this checkout.
  MoltenVK citations are against `KhronosGroup/MoltenVK` at `main`, fetched through the GitHub
  API on 2026-09-11. Eden pins the fork `V380-Ori/Ryujinx.MoltenVK@v1.4.1-ryujinx`
  (`cpmfile.json:162-168`); the fork's packaging matches upstream where this document quotes it,
  but its line numbers will drift from `main`. Quotes are exact; line numbers are a hint.
- **INFERRED** — reasoned from the above. Flagged in place and collected again in
  [Part 4](#part-4--inferred-not-verified). Nothing in Part 4 is a basis for a decision yet.

There is a third category that matters more than either: **things nobody can know without a
device**. Performance is all of them. This document never estimates a frame rate.

---

## Part 0 — the finding that changes what the rest means

**The `IsMoltenVK()` special case in `vulkan_device.cpp` cannot fire. It is dead code.**

VERIFIED. `Device::GetSuitability` (`src/video_core/vulkan_common/vulkan_device.cpp:999`) runs in
this order:

| line | what happens |
|---|---|
| 1008 | `properties.properties = physical.GetProperties();` — core 1.0 properties only |
| 1082 | mandatory **extension** check, against the string set read at ~1017 — correct, see Part 2.1 |
| 1142 | `physical.GetFeatures2(features2);` |
| **1147-1172** | **`FOR_EACH_VK_MANDATORY_FEATURE(CHECK_FEATURE)` — the block containing the MoltenVK whitelist** |
| 1184-1185 | `properties.driver` is added to the `properties2` pNext chain |
| **1234** | **`physical.GetProperties2(properties2);` — the only call that fills `properties.driver`** |

The whitelist at `vulkan_device.cpp:1150-1153` is:

```cpp
if (IsMoltenVK() && (strcmp(#name, "geometryShader") == 0 ||
                    strcmp(#name, "logicOp") == 0 ||
                    strcmp(#name, "shaderCullDistance") == 0 ||
                    strcmp(#name, "wideLines") == 0)) {
```

and `IsMoltenVK()` is (`vulkan_device.h:1090-1092`):

```cpp
bool IsMoltenVK() const noexcept {
    return properties.driver.driverID == VK_DRIVER_ID_MOLTENVK;
}
```

`driver` is declared `VkPhysicalDeviceDriverProperties driver{}` (`vulkan_device.h:1190`), so
`driverID` is value-initialised to `0`. `VK_DRIVER_ID_MOLTENVK` is 14, and 0 is not a valid
`VkDriverId` at all. At line 1150 the comparison is `0 == 14`. **`IsMoltenVK()` is false for
every device, on every platform, at the moment the mandatory-feature check runs.**

MoltenVK's side is fine — it reports the ID correctly at `MVKDevice.mm:854`
(`supportedProps12.driverID = VK_DRIVER_ID_MOLTENVK;`), surfaced through
`VK_KHR_driver_properties`, which it advertises (`MVKExtensions.def:58`) and which Eden lists as
mandatory (`vulkan_device.h:121-126`). Eden simply asks before it has looked.

### Why this is not, today, a boot failure

VERIFIED. `suitable` is advisory. `vulkan_device.cpp:456` and `:473-474`:

```cpp
// vulkan_device.cpp:456
const bool is_suitable = GetSuitability(surface != VkSurfaceKHR{});

// vulkan_device.cpp:473-474, after the driver_id locals at 458-469
if (!is_suitable)
    LOG_WARNING(Render_Vulkan, "Unsuitable driver - continuing anyways");
```

Nothing else reads the result; there is no device-rejection path and no second candidate device
on iOS. So the observable effect on a device is a log, not a refusal:

```
Missing required feature geometryShader
Missing required feature logicOp
Missing required feature shaderCullDistance
Missing required feature wideLines
Unsuitable driver - continuing anyways
```

Two consequences worth stating separately, because they point in opposite directions:

- The whitelist is doing nothing, so **whether a feature is on the whitelist currently makes no
  difference to anything**. The four names in it are not four solved problems; they are four
  names in unreachable code. Part 2 below is therefore about what Eden *would* tolerate once the
  ordering is fixed, and equally about what it tolerates now — which is everything, loudly.
- Because it tolerates everything, a genuinely fatal gap will not announce itself here either.
  `LOG_ERROR` lines are the only signal, and they are already expected to appear.

**This is a bug in `vulkan_device.cpp`, which this lane does not own.** The fix is small — move
the driver-properties query before the feature check, or read `driverID` from a separate early
`GetProperties2` — but it belongs to whoever owns that file. Recorded here, not applied.

---

## Part 1 — what MoltenVK actually publishes for iOS

### 1.1 The release assets

VERIFIED, via `gh api repos/KhronosGroup/MoltenVK/releases` on 2026-09-11. Every release from
`v1.2.4` onward publishes exactly:

| asset | size (v1.4.2) |
|---|---|
| `MoltenVK-all.tar` | 180 MB |
| `MoltenVK-ios.tar` | 34 MB |
| `MoltenVK-macos.tar` | 60 MB |
| `MoltenVK-macos-privateapi.tar` | 60 MB — **v1.4.1 and later only, and macOS only** |

No `.xcframework` asset, no `.zip`, no per-dylib asset. Tags before `v1.2.4` carry no assets.
Neither do `v1.2.11`, `v1.2.11-rc1` or `v1.2.11-b1` — for 1.2.11 the artifacts live on a separate
tag, `v1.2.11-artifacts`. `ci/fetch-moltenvk.sh` previously defaulted to `MOLTENVK_VERSION=v1.2.11`
and so could never have downloaded anything, which is consistent with its own header saying
nothing had been downloaded while it was written.

### 1.2 There is no iOS `libMoltenVK.dylib`. There never has been.

VERIFIED, and it is the single most load-bearing fact in this document, because Eden opens a file
by that exact name.

`Scripts/package_dylibs.sh`, upstream **and** in the Ryujinx fork at `v1.4.1-ryujinx`, ends:

```bash
# App store distribution does not support naked dylibs, so only include a naked dylib for macOS.
copy_dylib "" "macOS"
#copy_dylib "-iphoneos" "iOS"
#copy_dylib "-iphonesimulator" "iOS-simulator"
#copy_dylib "-appletvos" "tvOS"
#copy_dylib "-appletvsimulator" "tvOS-simulator"
#copy_dylib "-xrvos" "xrOS"
#copy_dylib "-xrsimulator" "xrOS-simulator"

# For legacy support, symlink old dylib location to new location
ln -sfn "dynamic/dylib" "${mvk_pkg_prod_path}/dylib"
```

Every non-macOS line is commented out. `Docs/MoltenVK_Runtime_UserGuide.md` agrees: its section
heading is literally *"Install MoltenVK as a Dynamic Library on **macOS**"*, and the only dylib
path it names is `Package/Latest/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib`. For every other
platform the guide offers only the xcframework.

The only shippable **dynamic** iOS artifact is therefore the framework binary:

```
MoltenVK/dynamic/MoltenVK.xcframework/<ios device slice>/MoltenVK.framework/MoltenVK
```

an ordinary Mach-O dylib whose `LC_ID_DYLIB` is `@rpath/MoltenVK.framework/MoltenVK`
(stated in the user guide's xcframework section).

"No naked dylibs" is an **App Store policy**, not a dyld restriction. dyld loads a naked dylib
from inside a bundle perfectly well, and this port ships through TrollStore / StikDebug /
sideload. So `ci/fetch-moltenvk.sh` extracts that framework binary, stages it as
`libMoltenVK.dylib`, and rewrites the install name.

This matters because Eden looks for exactly one filename
(`src/video_core/vulkan_common/vulkan_library.cpp:28-55`):

```
$LIBVULKAN_PATH
GetBundleDirectory()/"Frameworks/libMoltenVK.dylib"      <- line 32
GetBundleDirectory()/"Frameworks/libvulkan.1.dylib"      <- line 34
dlopen(nullptr)                                          <- line 49, the static case
```

A tidier alternative — ship the real `MoltenVK.framework` and add
`Frameworks/MoltenVK.framework/MoltenVK` to that candidate list — is **not** taken by the script,
because it is a change to `vulkan_library.cpp`. Recommended for a pass that owns that file.

### 1.3 `logicOp` and `wideLines` are gated on a build flag that has no iOS release

VERIFIED, and it is the reason two of the four whitelisted names are false.
`MVKDevice.mm:2797` and `:2801`:

```objc
_features.logicOp   = getMVKConfig().useMetalPrivateAPI;
_features.wideLines = getMVKConfig().useMetalPrivateAPI;
```

`useMetalPrivateAPI` is off unless MoltenVK is built with `MVK_USE_METAL_PRIVATE_API=1`. The only
release asset built that way is **`MoltenVK-macos-privateapi.tar`**. There is no
`MoltenVK-ios-privateapi.tar` in any release of either repository. So on any stock iOS MoltenVK,
`logicOp` and `wideLines` are false, and the only way to change that is to build MoltenVK from
source — which this project cannot do on its current machine.

INFERRED: shipping a private-API MoltenVK inside an iOS app would also be an App Store rejection
risk, which is irrelevant to a sideloaded build but relevant to anyone who forks this later.

---

## Part 2 — the feature gap

### 2.1 Mandatory extensions: all four are fine

VERIFIED. Eden's `FOR_EACH_VK_MANDATORY_EXTENSION` (`vulkan_device.h:121-126`) is four entries,
checked at `vulkan_device.cpp:1082` against the enumerated extension strings — a check that, unlike
the feature check, runs against data that is actually populated.

| Eden requires | MoltenVK | min iOS |
|---|---|---|
| `VK_EXT_vertex_attribute_divisor` | advertised (`MVKExtensions.def:191`) | 8.0 |
| `VK_KHR_driver_properties` | advertised (`:58`) | 8.0 |
| `VK_KHR_sampler_mirror_clamp_to_edge` | advertised (`:98`) | **14.0** |
| `VK_KHR_shader_float_controls` | advertised (`:104`) | 8.0 |

Plus `VK_KHR_swapchain` when a surface is required (`vulkan_device.cpp:1084-1086`) — advertised.

No mandatory extension is missing. MoltenVK's own runtime floor is iOS 15
(`MoltenVK_Runtime_UserGuide.md`, "Build and Runtime Requirements"), above the 14.0 entry above,
so the version column never bites.

INFERRED: advertising `VK_KHR_sampler_mirror_clamp_to_edge` is not the same as the feature bit
being true — `MVKDevice.mm:2864` sets `samplerMirrorClampToEdge` from a Metal capability. Eden
only checks the extension string here, so a device that advertises the extension with the feature
off would pass this check. Not investigated further.

### 2.2 Mandatory features: one real gap, and it is not one of the four

VERIFIED against `MVKPhysicalDevice::initFeatures()`, `MVKDevice.mm:2790-2876`, which opens with
`mvkClear(&_features)` — **everything starts false and is turned on explicitly**, so a feature
that is never mentioned is permanently false.

Eden's 29 mandatory entries (`vulkan_device.h:146-174`):

| feature | MoltenVK on iOS | source |
|---|---|---|
| `depthBiasClamp` | true | `MVKDevice.mm:2798` |
| `depthClamp` | true | `:2823` |
| `drawIndirectFirstInstance` | `indirectDrawing && baseVertexInstanceDrawing` | `:2820` |
| `dualSrcBlend` | true | `:2822` |
| `fillModeNonSolid` | true | `:2799` |
| `fragmentStoresAndAtomics` | true | `:2815` |
| **`geometryShader`** | **never assigned → false** | — |
| `imageCubeArray` | Apple4+ | `:2841` |
| `independentBlend` | true | `:2795` |
| `largePoints` | true | `:2800` |
| **`logicOp`** | **private-API only → false** | `:2797` |
| `multiDrawIndirect` | true | `:2812` |
| **`multiViewport`** | **Apple5+ only** | `:2845` |
| `occlusionQueryPrecise` | Apple3+ | `:2834` |
| `robustBufferAccess` | true | `:2793` |
| `samplerAnisotropy` | true | `:2803` |
| `sampleRateShading` | true | `:2796` |
| `shaderClipDistance` | true | `:2810` |
| **`shaderCullDistance`** | **never assigned → false** | — |
| `shaderImageGatherExtended` | true | `:2804` |
| `shaderStorageImageWriteWithoutFormat` | true | `:2807` |
| **`tessellationShader`** | **Apple3+ → true, but emulated — see 2.4** | `:2835` |
| `vertexPipelineStoresAndAtomics` | true | `:2814` |
| **`wideLines`** | **private-API only → false** | `:2801` |
| `hostQueryReset` | true | `:212` |
| `shaderDemoteToHelperInvocation` | true | `:234` |
| `shaderDrawParameters` | true | `:169` |
| `variablePointers` | true | `:166` |
| `variablePointersStorageBuffer` | true | `:165` |

**The result is narrower than expected, and worth stating plainly: of 29 mandatory features,
exactly five are not unconditionally true, and four of those five are the four Eden already
names.** The last five entries — `variablePointers`, `shaderDrawParameters`,
`shaderDemoteToHelperInvocation`, `hostQueryReset` — were the real risk (a false there is a gap
with no whitelist entry and no fallback) and all four are hardcoded true.

The one uncovered gap is **`multiViewport`**, true only from `MTLGPUFamilyApple5`.

INFERRED, from Apple's published GPU family table rather than from any file read here: Apple5 is
the A12. That makes the practical floor **iPhone XS / XR / 2018 iPad Pro and later** — which is
also roughly where a Switch emulator's other requirements land, so it costs little. On A11 and
earlier `multiViewport` is false, and once Part 0 is fixed that is an un-whitelisted missing
mandatory feature. Whether that *should* become a fifth whitelist entry or a hard floor is a
product decision, not a technical one, and it is not made here.

The `imageCubeArray` (Apple4 = A11) and `occlusionQueryPrecise` / `tessellationShader`
(Apple3 = A9/A10) rows are all satisfied at that floor.

### 2.3 Extensions Eden names that MoltenVK does not implement at all

VERIFIED by absence from `MVKExtensions.def` (the complete list of what MoltenVK advertises) and,
for the feature structs, from `MVKDeviceFeatureStructs.def`. None of these is mandatory; all are
`FOR_EACH_VK_RECOMMENDED_*` or plain `FOR_EACH_VK_EXTENSION`, so absence is logged, not fatal.

| extension | Eden's list | note |
|---|---|---|
| `VK_EXT_transform_feedback` | recommended feature | see 2.5 — the big one |
| `VK_EXT_custom_border_color` | recommended feature | see 2.6 |
| `VK_EXT_border_color_swizzle` | feature ext | pairs with the above |
| `VK_EXT_vertex_input_dynamic_state` | recommended | |
| `VK_EXT_depth_bias_control` | recommended | three feature bits Eden asks for |
| `VK_EXT_conservative_rasterization` | recommended | |
| `VK_EXT_conditional_rendering` | general | |
| `VK_EXT_depth_range_unrestricted` | recommended | |
| `VK_EXT_color_write_enable` | feature ext | |
| `VK_EXT_descriptor_buffer` | feature ext | |
| `VK_EXT_astc_decode_mode` | general | Apple GPUs decode ASTC natively regardless |
| `VK_EXT_filter_cubic` / `VK_IMG_filter_cubic` | general | |
| `VK_KHR_workgroup_memory_explicit_layout` | feature ext | feeds `support_explicit_workgroup_layout` |
| `VK_KHR_pipeline_executable_properties` | feature ext | diagnostics only |
| `VK_NV_geometry_shader_passthrough` | recommended | NV-only; never going to exist |
| `VK_NV_viewport_array2`, `VK_NV_viewport_swizzle` | recommended | NV-only |

Present and used, for contrast: `VK_KHR_swapchain`, `VK_KHR_spirv_1_4`,
`VK_KHR_create_renderpass2`, `VK_KHR_depth_stencil_resolve`, `VK_KHR_image_format_list`,
`VK_KHR_push_descriptor`, `VK_KHR_draw_indirect_count`, `VK_EXT_memory_budget`,
`VK_EXT_tooling_info`, `VK_EXT_subgroup_size_control`, `VK_EXT_shader_stencil_export`,
`VK_EXT_shader_viewport_index_layer`, `VK_EXT_robustness2`, `VK_EXT_provoking_vertex`,
`VK_EXT_extended_dynamic_state` 1/2/3, `VK_EXT_4444_formats`, `VK_EXT_index_type_uint8`,
`VK_EXT_line_rasterization`, `VK_EXT_primitive_topology_list_restart`, `VK_KHR_maintenance1`
through `maintenance9`.

That is a better hit rate than the NV-shaped parts of Eden's list would suggest. The extended
dynamic state family in particular is fully present, which matters for pipeline-state churn.

### 2.4 Tessellation: reports true, runs as compute

VERIFIED that MoltenVK reports `tessellationShader = true` on Apple3+ (`MVKDevice.mm:2835`) and
that it wires tessellation into the subgroup story (`MVKDevice.mm:1482`, which adds
`VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT` to the supported subgroup stages *only* when
`_features.tessellationShader`) — treating the control stage as a compute stage, which is what it
is under the hood.

VERIFIED that Metal has no tessellation control/evaluation shader stages; it has a fixed-function
tessellator driven by a compute kernel that writes a patch buffer. MoltenVK therefore translates
a Vulkan tessellation pipeline into multiple Metal passes.

**This is the "emulates badly" case the lane was asked about, and it is worse than a missing
feature, because it is invisible.** A missing feature produces a log line. This produces a
`VK_TRUE`, a pipeline that compiles, and a cost that only shows up as frame time. Eden will not
know to avoid it.

INFERRED, not measured: the per-draw cost is a pipeline split and extra buffer traffic. Nothing
here estimates it. There is also a visible interaction already in the tree —
`vk_rasterizer.cpp:1171-1172` has `UNIMPLEMENTED_IF` on tessellation combined with transform
feedback, which on iOS is moot only because transform feedback is absent entirely (2.5).

### 2.5 Transform feedback: absent, and Eden already knows how to cope

VERIFIED absent — no `EXT_transform_feedback` in `MVKExtensions.def`, no `TransformFeedback` entry
in `MVKDeviceFeatureStructs.def`. Metal has no stream-output stage, so this is structural, not an
oversight.

VERIFIED that Eden degrades rather than dies. `vulkan_device.cpp:1463-1467` gates the extension on
both the feature bit and `maxTransformFeedbackBuffers > 0`, and the consumers check
`IsExtTransformFeedbackSupported()` first:

- `vk_rasterizer.cpp:1151-1163` — returns early, `LOG_WARNING("Transform feedback requested by
  guest but VK_EXT_transform_feedback is unavailable; queries disabled")`, once.
- `vk_pipeline_cache.cpp:187` and `:241` — `LOG_WARNING("XFB requested in pipeline key but device
  lacks VK_EXT_transform_feedback; ignoring XFB decorations")`.

So this is a **rendering-correctness** gap, not a boot blocker: pipelines still build, XFB
decorations are dropped, and the guest's stream-output results are silently wrong.

INFERRED: on Switch titles, transform feedback backs geometry-shader emulation paths and some
particle/skinning work, so the symptom would be missing or frozen geometry in specific titles
rather than a general failure. Which titles, and how bad, is unknown and unknowable without
running one.

### 2.6 Custom border colour

VERIFIED absent (`VK_EXT_custom_border_color` not in `MVKExtensions.def`, `CustomBorderColor` not
in `MVKDeviceFeatureStructs.def`). Eden lists `custom_border_color.customBorderColors` as
*recommended* (`vulkan_device.h:181`), so it logs and continues, and it carries a whole sampler
budget for it (`Device::TryReserveCustomBorderColorSamplers`, `vulkan_device.cpp:1581`) which will
simply never be exercised.

INFERRED: Metal's `MTLSamplerBorderColor` offers three fixed values — transparent black, opaque
black, opaque white — against Vulkan's arbitrary RGBA. A guest border colour outside that set has
no representation, so the fallback is a nearest-of-three approximation. Visible as wrong edge
colours on clamped-to-border sampling. Not verified against MoltenVK's sampler code, which was not
read.

### 2.7 Subgroups: the shape is right, the control is missing

This one is subtler than the others and is the one most likely to be mis-assessed from a distance.

VERIFIED good news — the subgroup **width** matches. `MVKDevice.mm:2708-2711` sets, for
`kAppleVendorId`, `maxSubgroupSize = 32` and `minSubgroupSize = 4`. Eden's `GuestWarpSize` is 32
(`vulkan_device.h:215`). So `extensions.subgroup_size_control`, gated at
`vulkan_device.cpp:1455-1459` on `minSubgroupSize <= 32 && maxSubgroupSize >= 32`, **passes**, and
`is_warp_potentially_bigger` (`vulkan_device.cpp:505-506`, `maxSubgroupSize > GuestWarpSize`) is
**false** — the favourable branch.

VERIFIED bad news, two parts.

**(a) `requiredSubgroupSizeStages = 0`.** `MVKDevice.mm:916` sets it unconditionally to zero. Eden
reads it in exactly one place, `vulkan_device.h:473-475`:

```cpp
bool IsGuestWarpSizeSupported(VkShaderStageFlagBits stage) const {
    return properties.subgroup_size_control.requiredSubgroupSizeStages & stage;
}
```

which is therefore **false for every stage**. Eden can detect that the subgroup size *may* be 32
but can never *pin* it to 32. Combined with `minSubgroupSize = 4` — and note MoltenVK's own
comment at `:2710`, *"Minimum thread execution width for Apple GPUs is unknown, but assumed to be
4. May be greater."* — the actual width at runtime is not knowable in advance.

**(b) The vertex stage has no subgroup support.** `MVKDevice.mm:1481`:

```objc
pVk11Props->subgroupSupportedStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
```

plus tessellation-control when tessellation is on (`:1482-1484`). **Vertex and geometry are never
included.** Eden feeds this straight into the shader recompiler at
`vk_pipeline_cache.cpp:361-374`, building `supported_subgroup_stages` from
`GetSubgroupSupportedStages()` — so vertex-stage subgroup operations will be compiled out or
rejected on Apple GPUs.

The operation set itself is reasonable where it is supported: `MVKDevice.mm:1485-1503` grants
`BASIC`, and on `simdPermute`/`quadPermute` hardware also `VOTE`, `BALLOT`, `SHUFFLE`,
`SHUFFLE_RELATIVE`, `ROTATE`, `ROTATE_CLUSTERED`, with `ARITHMETIC` on `simdReduction` and `QUAD`
on `quadPermute`. Eden's two consumers — `vk_pipeline_cache.cpp:410-411` (`support_quad_shuffles`,
`support_vote`) and `vk_compute_pass.cpp:439-442` (BASIC + ARITHMETIC + SHUFFLE +
SHUFFLE_RELATIVE, for a fast path) — are both satisfiable on Apple hardware.

INFERRED: the guest assumes an Nvidia 32-wide warp. Fragment-stage divergence between an assumed
32 and an actual 4-to-32 is the kind of thing that produces subtly wrong pixels rather than a
crash, and it cannot be ruled out from source alone.

### 2.8 Documented MoltenVK limitations that intersect this port

VERIFIED, from `Docs/MoltenVK_Runtime_UserGuide.md`, "Known MoltenVK Limitations":

- **`VK_QUERY_TYPE_PIPELINE_STATISTICS` is not supported.** Eden's query cache is broad; whether
  it asks for pipeline statistics was not checked.
- **`VkAllocationCallbacks` are ignored.** Harmless here — Eden does not rely on them.
- **PVRTC content must be loaded by host-visible memory mapping, not via a staging buffer**, or
  the image is malformed. Switch titles use ASTC and BCn, not PVRTC, so INFERRED: not relevant.
- **MoltenVK loads no Vulkan layers.** No validation layers on device. Every Vulkan misuse this
  port commits will surface as a Metal error or a wrong pixel, never as a validation message.
  For a bring-up this is the most expensive item in the list.

---

## Part 3 — what `ci/fetch-moltenvk.sh` does about all this

Summarised; the script's own header carries the detail.

- Resolves the asset through the **releases API**, not a guessed URL, so a tag with no assets
  reports "this tag publishes no assets at all" and lists what exists.
- Defaults to **`V380-Ori/Ryujinx.MoltenVK@v1.4.1-ryujinx`**, matching Eden's own pin
  (`cpmfile.json:162-168`) so the iOS and macOS halves of the project run one MoltenVK.
  `MOLTENVK_REPO=KhronosGroup/MoltenVK` switches to upstream.
- **Lists the archive before extracting** and pulls out only candidate members — the tar expands
  to several hundred MB, most of it static libraries, and the machine driving this port has under
  a gigabyte free. Scratch space defaults to the repo's volume, not `$TMPDIR`.
- Finds the library by **ranked pattern, never a hardcoded path**, because the xcframework slice
  directory name is chosen by `xcodebuild -create-xcframework` and varies by version and fork.
- **Verifies, and refuses rather than stages:** `arm64` via `lipo -archs`; platform `IOS` via
  `vtool -show-build` with an `otool -l` numeric fallback; Mach-O dylib via `file`; exports
  `_vkGetInstanceProcAddr` via `nm`; a plausible size.

Two verification notes worth keeping:

- The **old platform check was wrong in a way that mattered**. It was
  `grep -qiE 'platform (2|IOS)|LC_VERSION_MIN_IPHONEOS'` — unanchored, so it matches
  `platform IOSSIMULATOR` and would have accepted a simulator binary. The new check parses the
  platform token and requires `IOS`, rejecting `IOSSIMULATOR` (7), `MACCATALYST` (6) and
  `MACOS` (1) explicitly.
- The **symbol check is not redundant**. MoltenVK can be built with `MVK_HIDE_VULKAN_SYMBOLS=1`
  (README, "Hiding Vulkan API Symbols"). Such a build `dlopen`s cleanly and then every `dlsym`
  returns null — a black screen with no error anywhere. Checking for `_vkGetInstanceProcAddr` at
  staging time turns that into a build-time message.

**Where the script deliberately does not fail:** `--embed` with nothing staged stays non-fatal and
exits 0. It is called from a build phase (`src/ios/project.yml:247`), nothing in the iOS link
references a `vk*` symbol, and failing there would break a milestone that is otherwise met. It now
prints a loud block naming `vulkan_library.cpp:52` as where the failure will surface, and
`EDEN_MOLTENVK_REQUIRED=1` makes it fatal for any build meant to render. Every case where a
*wrong* dylib could be staged or embedded is fatal, unconditionally — shipping the wrong MoltenVK
is worse than shipping none, because it converts a clear log line into a half-working `dlopen`.

---

## Part 4 — inferred, not verified

Collected so none of it is mistaken for a checked fact.

1. Apple GPU family → device mapping (Apple3 = A9/A10, Apple4 = A11, Apple5 = A12). From Apple's
   published table, not from anything read here. The A12 floor in 2.2 rests entirely on it.
2. Metal's border-colour set is three fixed values, so custom border colour degrades to a
   nearest-match. MoltenVK's sampler code was not read.
3. Tessellation's cost. Asserted to exist, never measured, and unmeasurable without a device.
4. Which titles transform feedback breaks, and how visibly.
5. That fragment-stage subgroup width below 32 can produce wrong pixels under a guest that assumes
   32. Plausible; unproven.
6. That a private-API MoltenVK would be an App Store problem. Irrelevant to a sideloaded build.
7. PVRTC being irrelevant to Switch content.

Not inferred and not verified — simply **unknown**: everything about performance, and whether the
app reaches GPU init at all, which depends on JIT permission long before it depends on Vulkan.

## Part 5 — what would need to change, and who owns it

None of these are in this lane. Listed so they are not lost.

1. **`vulkan_device.cpp`** — populate `properties.driver` before the mandatory-feature check, so
   `IsMoltenVK()` is meaningful. Without this the whitelist is dead code (Part 0). Highest value
   of anything here, and small.
2. **`vulkan_device.cpp`** — once (1) lands, decide `multiViewport`: fifth whitelist entry, or a
   documented A12 floor (2.2).
3. **`vulkan_library.cpp`** — add `Frameworks/MoltenVK.framework/MoltenVK` to the iOS candidate
   list, so the real framework can be shipped instead of a renamed framework binary (1.2).
4. **`externals/CMakeLists.txt:433-444` and `cpmfile.json:162-168`** — the MoltenVK block is
   guarded by a plain `if (APPLE)`, true for iOS, and points `MOLTENVK_LIBRARY` at the **macOS**
   dylib. Harmless only because nothing consumes it with `ENABLE_QT=OFF`. Needs an `if (IOS)`
   branch and an iOS artifact entry.

## Sources

Eden, this checkout:
`src/video_core/vulkan_common/vulkan_device.h` (121-126, 146-174, 178-200, 215, 473-483, 1090-1092,
1190) ·
`src/video_core/vulkan_common/vulkan_device.cpp` (456, 473-474, 505-506, 999-1008, 1082-1090,
1142-1172, 1184-1185, 1234, 1455-1467, 1581) ·
`src/video_core/vulkan_common/vulkan_library.cpp` (28-55) ·
`src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` (187, 241, 361-374, 410-411) ·
`src/video_core/renderer_vulkan/vk_rasterizer.cpp` (1151-1172) ·
`src/video_core/renderer_vulkan/vk_compute_pass.cpp` (439-442) ·
`cpmfile.json` (162-168) · `externals/CMakeLists.txt` (433-444) · `src/ios/project.yml` (247)

MoltenVK, `KhronosGroup/MoltenVK@main`, fetched 2026-09-11:
`MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm` (165-169, 212, 234, 854, 916, 1479-1504, 1482,
2517-2622, 2688-2720, 2790-2876, 2864) ·
`MoltenVK/MoltenVK/GPUObjects/MVKDeviceFeatureStructs.def` ·
`MoltenVK/MoltenVK/Layers/MVKExtensions.def` ·
`Scripts/package_dylibs.sh` · `Scripts/package_moltenvk_xcframework.sh` ·
`Scripts/create_xcframework_func.sh` · `Docs/MoltenVK_Runtime_UserGuide.md` · `README.md`

Releases: `gh api repos/KhronosGroup/MoltenVK/releases`,
`gh api repos/V380-Ori/Ryujinx.MoltenVK/releases`, both 2026-09-11.
