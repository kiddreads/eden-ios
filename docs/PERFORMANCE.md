<!--
SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Performance

**Nothing in this document is a measurement.** No game has run, no frame has reached a screen,
and no code in this repository has executed on an iOS device. Everything below is either read
directly out of the source — with a file and line so it can be checked — or explicitly labelled
as unknown.

The honest headline is at the bottom and it is short: **nobody can say how fast this will be
until it runs on hardware.** What can be said now is *which* costs are structural and permanent,
*which* are settings somebody can change, and *what to measure first* so the question stops
being a guess.

---

## 1. The permanent handicap: fastmem is off

### 1.1 Why it is off — two independent reasons, either one sufficient

**Reason one: the page sizes do not match.** The fastmem arena maps guest pages directly into
the host address space, so a guest load becomes a host load. That needs host pages no larger
than guest pages. The Switch guest uses 4 KiB; Apple silicon uses 16 KiB.
`HostMemory::Impl::Init()` asserts on exactly this:

```cpp
ASSERT_MSG(page_size == 0x1000, "page size {:#x} is incompatible with 4K paging", page_size);
```
— `src/common/host_memory.cpp:543`

so iOS takes its own branch in the `HostMemory` constructor
(`src/common/host_memory.cpp:736-757`, guarded by `EDEN_IOS_NO_FASTMEM`, defined at
`src/common/host_memory.cpp:72`) which allocates plain backing memory and leaves
`virtual_base = nullptr`.

That nullptr propagates: `Memory::Impl::SetCurrentPageTable` assigns
`current_page_table->fastmem_arena = system.DeviceMemory().buffer.VirtualBasePointer()`
(`src/core/memory.cpp:54`), which is null, and `ArmDynarmic64::MakeJit` turns a null arena into
`std::nullopt`:

```cpp
config.fastmem_pointer = page_table->fastmem_arena ?
    std::optional<uintptr_t>{reinterpret_cast<uintptr_t>(page_table->fastmem_arena)} :
    std::nullopt;
```
— `src/core/arm/dynarmic/arm_dynarmic_64.cpp:225-227` (the A32 path is the same, at
`src/core/arm/dynarmic/arm_dynarmic_32.cpp:185-187`)

**Reason two: there is no fault handler to make it work.** Fastmem depends on the host
delivering page faults back into the JIT. dynarmic gates the whole fastmem emission path on
`ctx.fastmem.SupportsFastmem()` (`.../backend/arm64/emit_arm64_memory.cpp:505`), which forwards
to the exception handler. On iOS the build selects `exception_handler_generic.cpp`
(`src/dynarmic/src/dynarmic/CMakeLists.txt:327-339`), whose `SupportsFastmem()` returns `false`
unconditionally (`src/dynarmic/src/dynarmic/backend/exception_handler_generic.cpp:38-40`).

So even if the page sizes matched tomorrow, fastmem would still be off until somebody writes a
Mach exception handler that works under iOS's restrictions. The CMake comment at
`src/dynarmic/src/dynarmic/CMakeLists.txt:327-338` says why the macOS one is not that handler:
`mach/mach_exc.defs` is not in the iPhoneOS SDK, the `mig` invocations hardcode `-arch x86_64`,
and Mach exception ports are restricted on iOS anyway.

**One trap worth knowing about.** `Settings::IsFastmemEnabled()` returns `true` on iOS —
the `__APPLE__` case falls through to `#else return true;`
(`src/common/settings.cpp:185-199`). It does not check the host page size the way the Linux
arm64 case does (`return getpagesize() == 4096;`). Nothing breaks today, because
`fastmem_arena` is null regardless and `fastmem_pointer` therefore ends up `nullopt` anyway.
But anything new that reads `IsFastmemEnabled()` as "the arena exists" will be wrong on iOS.

### 1.2 What the generated code actually does differently

This is the concrete answer to "what does it cost". Take a non-ordered 64-bit guest load,
`CpuAccuracy::Auto` (the default — `src/common/settings.h:266`), 39-bit guest address space.

`EmitReadMemory` picks the path at `.../backend/arm64/emit_arm64_memory.cpp:646-653`:
`ShouldFastmem` returns `nullopt`, `page_table_pointer` is non-zero, so
`InlinePageTableEmitReadMemory` is used.

**With fastmem** — `CpuAccuracy::Auto` sets `fastmem_address_space_bits = 64`
(`src/core/arm/dynarmic/arm_dynarmic_64.cpp:346`), so `FastmemEmitVAddrLookup` returns
`(Xfastmem, Xaddr)` and emits **nothing at all**
(`.../emit_arm64_memory.cpp:525-526`). The entire guest access is:

```
    LDR   Xvalue, [Xfastmem, Xaddr]          ; emit_arm64_memory.cpp:300 (EmitMemoryLdr)
```

**1 instruction. 1 load. No branches. NZCV untouched.**

**Without fastmem** — `InlinePageTableEmitVAddrLookup`
(`.../emit_arm64_memory.cpp:257-294`) emits, with the config Eden sets at
`src/core/arm/dynarmic/arm_dynarmic_64.cpp:214-222`:

```
    AND   Xscratch0, Xaddr, #0xFFF           ; :246   misalignment check…
    CMP   Xscratch0, #(4096 - 8)             ; :247   (detect_misaligned_access_via_page_table
    B.HI  fallback                           ; :248    = 16|32|64|128, page-boundary-only)
    LSR   Xscratch0, Xaddr, #12              ; :266   address-space range check…
    TST   Xscratch0, #(~0 << 27)             ; :267   (silently_mirror_page_table = false,
    B.NE  fallback                           ; :268    unused_top_bits = 64-39 = 25)
    LSL   Xscratch0, Xscratch0, #3           ; :272   × sizeof(PageEntryData)
    LDR   Xscratch0, [Xpagetable, Xscratch0] ; :274   *** the second, dependent load ***
    TBNZ  Xscratch0, #0, fallback            ; :278   marked bit (page_table_marked_bit = 0,
                                             ;         an engaged optional, so this IS emitted)
    AND   Xscratch0, Xscratch0, #ATTR_MASK   ; :282   strip the packed attribute bits
    CBZ   Xscratch0, fallback                ; :290   unmapped / rasterizer-cached → callback
    LDR   Xvalue, [Xscratch0, Xaddr]         ; :300   the actual guest load
```

**12 instructions. 2 loads, the second dependent on the first. 4 conditional branches. NZCV
clobbered.** Stores are the mirror image (`EmitMemoryStr`, `.../emit_arm64_memory.cpp:360`),
same eleven-instruction preamble.

Variants, so the number is not over-claimed:

| case | fastmem | page table |
|---|---|---|
| 64-bit non-ordered load/store | 1 | 12 |
| 8-bit access | 1 | 9 — the misalignment check is skipped entirely (`:222`) |
| ordered (LDAR/STLR) access | 2 — an extra `ADD` (`:308` load, `:368` store) | 13 |
| `page_table_sign_extension` set | — | +1 `SBFM` (`:287`); only when the backing allocation lands below 2^39 (`arm_dynarmic_64.cpp:234-239`), which is not the normal iOS case |

So the inline cost of a guest memory access goes from **one instruction to roughly a dozen**,
and — the part that instruction counts understate — from **one load to two dependent loads**.
The page-table load must complete before the address of the real load is known. On an
out-of-order core that is a serialised dependency chain on the critical path of every single
guest load and store, and guest loads and stores are a large fraction of all guest instructions.

It also costs code size, which costs i-cache, which costs again. Eleven extra instructions at
every memory-access site inside a 128 MiB code cache
(`src/core/arm/dynarmic/arm_dynarmic_64.cpp:261-262`, per core, four cores) is not a rounding
error.

### 1.3 What fastmem's absence does *not* cost

Being honest about this matters, because it bounds the damage.

- **Exclusive accesses are unaffected.** On the arm64 backend, `EmitExclusiveReadMemory` and
  `EmitExclusiveWriteMemory` always take the callback-only path regardless of fastmem
  (`.../emit_arm64_memory.cpp:657-659` and `:673-675`). `fastmem_exclusive_access` and
  `recompile_on_exclusive_fastmem_failure` are set (`arm_dynarmic_64.cpp:231-232`) but do
  nothing here. LDXR/STXR-heavy code — mutexes, frame sync — pays the same on every host.
- **GPU-tracked pages may be *cheaper* without fastmem.** A page marked
  `RasterizerCachedMemory` (`src/core/memory.cpp:451-521`, `MarkRasterizerCached` at `:486`)
  has its pointer zeroed by the marked bit, so the page-table path simply takes the
  `TBNZ`/`CBZ` branch to the callback. With fastmem the same access takes a real host page
  fault and a signal/Mach-exception round trip before reaching the same callback. dynarmic's own design notes concede the point: "the
  constant i-cache trashing and pipeline hazards introduced by the VDSO signal handlers"
  (`docs/dynarmic/Design.md:612`).
- **`Memory::Protect` is simply skipped.** `ProtectRegion`, `MarkRegionDebug` and
  `RasterizerMarkRegionCached` all guard their `host_buffer->Protect(...)` call on
  `current_page_table->fastmem_arena` (`src/core/memory.cpp:99`, `:399`, `:456`) and fall
  through to the page-entry bookkeeping, which runs either way. Correctness is preserved by the
  page type; only the host-level `mprotect` is skipped.

### 1.4 The fallback path, when it is taken

Both paths share the same fallback: `WrappedReadMemory*` / `WrappedWriteMemory*`
(`.../emit_arm64_memory.cpp:441-456`, and `:481-500` for stores), which reaches Eden's
callbacks at
`src/core/arm/dynarmic/arm_dynarmic_64.cpp:24-101`, then `Memory::Impl::Read<T>` /
`Write<T>` (`src/core/memory.cpp:623-649`), which calls `GetPointerImpl`
(`src/core/memory.cpp:568-601`) — a bounds check, an atomic page-entry load, a pointer
extraction, a `switch` on page type, and for rasterizer-cached pages a full
`HandleRasterizerDownload` / `HandleRasterizerWrite` (`src/core/memory.cpp:681-737`) with a GPU
memory-manager lookup and, on the write side, a lock on the system core.

That is expensive, and it is the same expense on every platform. What changes on iOS is only
*how* the JIT gets there.

### 1.5 The page table itself

`PageTable::entries` is a `SparseLargeVector<PageEntryData>` (`src/common/page_table.h:150`),
8 bytes per entry, one entry per 4 KiB guest page. A 39-bit address space is therefore
2²⁷ entries = **1 GiB of virtual reservation**, committed lazily at host-page granularity
(`src/common/sparse_large_vector.h:77-85`). `HostPageSize` is `sysconf(_SC_PAGESIZE)`
(`src/common/sparse_large_vector.h:31`) = 16384 on iOS, so each commit covers 2048 entries —
8 MiB of guest address space per commit, versus 2 MiB on a 4 KiB host.

Coarser commits mean fewer commit faults and more resident bytes per touched region. Which way
that nets out is **unmeasured**, and it interacts with jetsam (see §4).

---

## 2. Other settings that materially affect speed, and are available to us

These are levers that exist today. None of them has been tuned for iOS; the defaults below are
what the code picks, and several of them are picked by a `#ifdef __ANDROID__` that iOS does not
match — meaning iOS silently inherits the *desktop* default.

| Setting | Default on iOS | Why it matters | Source |
|---|---|---|---|
| `use_multi_core` | `true` | Spawns `NUM_CPU_CORES` = 4 emulation threads, plus GPU, presentation, audio, timing and scheduler threads. Off collapses to 1. | `settings.h:204`; `cpu_manager.cpp:26-33`; `hardware_properties.h:18` |
| `use_asynchronous_gpu_emulation` | **`true`** — Android defaults it to `false` | Runs the GPU on its own thread. The Android default exists for a reason; iOS misses it because the guard is `#ifdef __ANDROID__`. | `settings.h:498-504`; `video_core/video_core.cpp:54` |
| `gpu_accuracy` | **`High`** — Android defaults to `Low` | Same `#ifdef __ANDROID__` miss. This is one of the largest single GPU-side levers in the emulator, and iOS is inheriting the desktop end of it. | `settings.h:526-536` |
| `use_reactive_flushing` | **`true`** — Android defaults to `false` | Same miss. With fastmem off, its host-`Protect` half is skipped anyway (`memory.cpp:456-464`), so on iOS it changes less than it does elsewhere — but it is still the desktop default on a phone. | `settings.h:619-626` |
| `use_asynchronous_shaders` | `false` | Lets a draw proceed while its pipeline compiles, instead of stalling. MoltenVK pipeline creation goes through SPIR-V→MSL translation and a Metal compile, which is not fast. A strong candidate for `true` on iOS. | `settings.h:689`; `vk_pipeline_cache.cpp:771-786` |
| `resolution_setup` | `Res1X` | `Res1_2X` (`up_scale=1, down_shift=1`) and `Res3_4X` render below native and cost roughly quadratically less fill. The most reliable single knob for making something run at all. | `settings.h:363`; `settings.cpp:309-373` |
| `cpu_accuracy` | `Auto` | Already the fast one: `unsafe_optimizations`, `Unsafe_UnfuseFMA`, `Unsafe_IgnoreGlobalMonitor`, and `fastmem_address_space_bits = 64`. There is no faster CPU accuracy tier to switch to. | `settings.h:266`; `arm_dynarmic_64.cpp:342-348` |
| `use_speed_limit` / `speed_limit` | `true` / 100 | Caps at 100%. Irrelevant if nothing reaches 100%, but it must be off to measure a ceiling. | `settings.h:211-223` |
| `vsync_mode` | `Fifo` | On a `CAMetalLayer` this is the presentation cadence. `Mailbox`/`Immediate` change how frame pacing is measured. | `settings.h:366-374` |
| `code_cache_size` | 128 MiB/core | Smaller means more retranslation; larger means more RX mapping. Four cores ⇒ 512 MiB of executable mapping. | `arm_dynarmic_64.cpp:261-266` |

**Thread priority is on the generic POSIX path.** `Common::SetCurrentThreadPriority`
(`src/common/thread.cpp:452-505`) has branches for `_WIN32`, `__HAIKU__`, `__ANDROID__` and
`__linux__`, and an `#else` that calls `sched_get_priority_max(SCHED_OTHER)` +
`pthread_setschedparam`. iOS takes the `#else`. It does **not** use Darwin QoS classes, which
are what actually decide performance-core versus efficiency-core placement on Apple silicon.
The callers that matter are the CPU threads (`Critical`, `cpu_manager.cpp:176`), the GPU thread
(`Critical`, `gpu_thread.cpp:32`), the Vulkan scheduler (`Critical`, `vk_scheduler.cpp:272`),
presentation (`High`, `vk_present_manager.cpp:326`) and core timing (`VeryHigh`,
`core_timing.cpp:76`). The iOS bridge sets `QOS_CLASS_USER_INTERACTIVE` on the one thread it
creates itself (`src/ios/Bridge/EdenCoreBridge.m:633`) and says in a comment that this is
"a choice, not a measurement". Everything Eden spawns internally is unaffected by that.

Whether this matters is unknown and is on the measurement list.

### 2.1 NCE, and what enabling it on iOS would take

NCE (Native Code Execution) runs guest ARM64 instructions directly on the host CPU instead of
recompiling them, which is categorically faster than any JIT. It is gated off:

```cmake
if (ARCHITECTURE_arm64 AND (ANDROID OR LINUX))
    set(HAS_NCE 1)
    add_compile_definitions(HAS_NCE=1)
endif()
```
— `CMakeLists.txt:307-310`

iOS is neither, so `HAS_NCE` is undefined, `src/core/arm/nce/*` is not compiled
(`src/core/CMakeLists.txt:1225-1243`), and `cpu_backend`'s default, minimum *and* maximum all
collapse to `CpuBackend::Dynarmic` (`src/common/settings.h:252-265`) — so a config file asking
for NCE is clamped rather than honoured. That is safe, not accidental.

Turning it on would require, at minimum:

1. **A fastmem arena.** `SetNceEnabled` computes
   `is_nce_enabled = IsFastmemEnabled() && is_nce_selected && is_39bit`
   (`src/common/settings.cpp:214`) and warns "Fastmem is required to natively execute code in a
   performant manner" otherwise (`:206-208`). NCE runs guest code at guest addresses in the host
   address space; that *is* the fastmem mapping. **The 16 KiB host page size blocks this
   outright** — the same wall as §1.1, and it is not a software problem.
   Note also that `IsFastmemEnabled()` returns `true` on iOS (§1.1), so this guard would pass
   while the arena is null. That would need fixing first, or NCE would enable itself onto
   nothing.
2. **Rewriting `arm_nce.s` for Mach-O.** It uses ELF-only directives —
   `.section .text._ZN4Core6ArmNce...,"ax",%progbits` and `.type ...,%function`
   (`src/core/arm/nce/arm_nce.s:12-14`) — and unprefixed global symbols. Mach-O needs neither
   of those and does need a leading underscore.
3. **Signal handlers for SIGSEGV/SIGBUS** that survive on iOS, installed through
   `Common::SignalChain` (`src/core/arm/nce/arm_nce.cpp:10, 27-28, 294-330`), plus SVC patching
   (`src/core/arm/nce/patcher.cpp`) that writes into executable guest pages — which on iOS means
   the same `mprotect` RX↔RW dance the JIT already needs, at guest addresses.
4. **A 39-bit guest address space free in the host process**, checked at
   `src/core/loader/deconstructed_rom_directory.cpp:186-189`.

Item 1 is not solvable in software. **NCE on iOS is off the table** until Apple ships 4 KiB
pages, which will not happen.

---

## 3. Does anything in the tree quantify what fastmem is worth?

**No. Nothing measures it, and nothing estimates it numerically.** This was searched for:

- There is **no benchmark target anywhere in the tree.** `src/dynarmic/tests/` builds
  `dynarmic_tests` (Catch2 correctness), `dynarmic_print_info`, `dynarmic_test_generator` and
  `dynarmic_test_reader` (`src/dynarmic/tests/CMakeLists.txt:5, 71, 88, 109`). Fuzzing against
  Unicorn, not timing. No microbenchmark, no timing harness, no recorded numbers.
- The only in-tree instrumentation is `Core::PerfStats`
  (`src/core/perf_stats.h:15-50`), which reports `system_fps`, `average_game_fps`, `frametime`
  and `emulation_speed`, reachable via `System::GetAndResetPerfStats()`
  (`src/core/core.h:233`). That is a frame-level speedometer, not a profiler. There is no
  MicroProfile instrumentation in this tree.
- The one qualitative statement upstream makes is in dynarmic's own design notes:

  > "The main downside from this is the constant i-cache trashing and pipeline hazards
  > introduced by the VDSO signal handlers. However **on most benchmarks fastmem does perform
  > faster than without (Linux only)**."
  > — `docs/dynarmic/Design.md:612`

  No figures. And note the parenthesis: the claim is scoped to Linux. The same paragraph says
  "Many kernels however, do not support fast signal dispatching (Solaris, OpenBSD, FreeBSD).
  Only Linux and Windows support relatively 'fast' signal dispatching" (`:600`). Darwin is not
  in the fast list. Mach exception delivery is a message to another thread, not a VDSO
  round-trip — so **fastmem's measured advantage on Linux is not transferable to any Apple
  platform**, and the size of the gap we are losing is smaller here than the Linux numbers
  would suggest. How much smaller is unknown.
- `src/ios/JITProbe/README.md:144-146` states plainly that the probe "does not measure speed,
  memory, or whether a 128 MiB code cache survives jetsam". It doesn't.

So: the emulator's own documentation asserts fastmem is worth having, on Linux, without a
number, and explicitly notes that the mechanism it depends on is slow on kernels other than
Linux and Windows. **Anyone quoting a percentage for what iOS loses is making it up.**

---

## 4. What is measurable only on device

Listed because a named gap is worth more than a hidden one.

- **Everything about actual speed.** Frames per second, emulation speed, frame pacing. There is
  no local build machine (README: no Xcode, under a gigabyte of free disk) and no device has run
  this code.
- **The real cost of the page-table path on Apple silicon.** The instruction counts in §1.2 are
  exact. Their cost is not: Apple's cores have deep out-of-order windows, large load/store
  queues and aggressive prefetch, and the second load usually hits L1 or L2. The dependency
  chain may be largely hidden, or may not be. This cannot be reasoned to a number.
- **Whether the 128 MiB × 4 code cache fits.** `IOS_PORT_NOTES.md` already lists this as
  unmeasured: whether iOS jetsam counts a large RX mapping as dirty against the app's footprint
  decides whether this fits on a 6 GB device at all.
- **Whether MoltenVK's fallbacks are cheap or catastrophic.** `vulkan_device.cpp` already
  degrades gracefully for `geometryShader`, `logicOp`, `shaderCullDistance` and `wideLines`
  (per `IOS_PORT_NOTES.md`), but what a game that leans on geometry shaders costs when they are
  emulated is unknown, and nothing in this port has read MoltenVK's source.
- **SPIR-V→MSL pipeline compile times.** Every new pipeline on iOS is a shader recompile *plus*
  a MoltenVK translation *plus* a Metal compile. This is the most likely source of stutter and
  it does not exist on any platform Eden already supports.
- **Thermal behaviour.** A phone sustaining four `Critical` CPU threads plus a `Critical` GPU
  thread will throttle. When, and to what, is device- and chassis-specific.
- **P-core vs E-core placement** for threads that go through the generic
  `pthread_setschedparam` path (§2).

---

## 5. What to measure first, in order

Each of these produces a number that changes a decision. Do them in this order; each one is
cheap once the previous has passed.

1. **Does it boot at all, and where does the time go before the first frame?** Wall-clock from
   `retro_load_game` to the first `EndGameFrame`. If this is minutes, nothing else matters yet.
2. **Steady-state `PerfStatsResults` on a homebrew `.nro`.** `emulation_speed`, `frametime`,
   `average_game_fps`, via `System::GetAndResetPerfStats()` (`src/core/core.h:233`). Set
   `use_speed_limit = false` first, or the ceiling is invisible. This is the baseline number
   that every later change is compared against. Homebrew first, because it isolates CPU from
   the GPU and shader-compile costs.
3. **Peak and steady-state resident memory, and whether jetsam fires.** Specifically with the
   four 128 MiB code caches live. If the answer is "jetsam kills it", `code_cache_size`
   (`arm_dynarmic_64.cpp:261-262`) becomes the first thing to tune and everything else waits.
4. **CPU-bound vs GPU-bound, by the crudest possible experiment:** set `resolution_setup` to
   `Res1_2X` (`settings.h:363`) and re-run step 2. If the frame rate barely moves, the GPU is
   not the bottleneck and the page-table MMU cost is the story. If it moves a lot, the CPU is
   not the bottleneck and §1 matters less than this document's length implies. **This single
   measurement decides where all subsequent effort goes.**
5. **The Android-default settings, one at a time**, each against the step-2 baseline:
   `gpu_accuracy = Low`, `use_reactive_flushing = false`, `use_asynchronous_gpu_emulation = false`.
   These are the three places iOS silently inherits a desktop default through a
   `#ifdef __ANDROID__` (§2). Any of them could be worth more than everything else on this list.
6. **`use_asynchronous_shaders = true`** (`settings.h:689`), measured as *stutter* — frame-time
   99th percentile, not mean FPS. Mean FPS will hide exactly the thing this setting fixes.
7. **Thread placement.** Whether the `Critical` threads land on performance cores. If they do
   not, adding a Darwin QoS branch to `Common::SetCurrentThreadPriority`
   (`src/common/thread.cpp:452`) is a small, well-defined change with a potentially large
   payoff — and it is the only item on this list that is a code change rather than a setting.
8. **Only then, a retail title.** And only then does a comparison against Eden on Android
   hardware of a similar class mean anything — because only then is the comparison
   fastmem-vs-no-fastmem rather than everything-vs-everything.

---

## 6. A realistic expectation

The honest answer is that **nobody can say until it runs on hardware**, and this document will
not pretend otherwise. But the shape of the answer is constrained by things already known:

- The CPU side is permanently handicapped, by roughly an order of magnitude more inline
  instructions per guest memory access and one extra dependent load (§1.2). That is a real,
  structural, unfixable cost. It is **not** an order-of-magnitude slowdown overall — memory
  accesses are a fraction of guest instructions, and Apple's cores are very good at hiding
  dependent L1 hits — but it is a tax on the single most common operation in the workload, and
  it will never be paid off.
- The loss relative to a fastmem-enabled platform is **smaller than a Linux comparison would
  suggest**, because fastmem's fault-delivery mechanism is slow on Darwin (§3). This narrows
  the gap versus macOS-with-fastmem. It does not narrow the gap versus Android-with-fastmem.
- NCE — the thing that would make the CPU side genuinely fast — is permanently unavailable
  (§2.1). Not "not yet ported". Blocked by the hardware page size.
- Several desktop-tuned defaults are currently in force on a phone (§2), and those are free to
  change. It is entirely possible that the first honest measurement finds the GPU path and the
  desktop defaults dominating, with the page-table MMU a secondary concern.
- The GPU side is genuinely unknown and could easily be worse than the CPU side. MoltenVK
  feature fallbacks and Metal shader compilation are costs that no other Eden platform pays.

What to expect, stated as a claim that can be falsified rather than a number: **this will be
slower than Eden on Android hardware of comparable class, and the gap will be larger on
CPU-bound titles than on GPU-bound ones.** How much slower is not knowable from the source, and
anyone who states a percentage before step 2 of §5 is guessing.

If a homebrew `.nro` renders a frame at any speed, that is the milestone that makes this
document worth rewriting with numbers in it.
