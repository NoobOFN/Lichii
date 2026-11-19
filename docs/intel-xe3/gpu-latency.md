# Where the frame time goes (EDEN_NX2_FRAME_LIMIT)

`EDEN_NX2_FRAME_LIMIT=1` prints two lines a second saying whether the device is really the limit,
which the Task Manager's GPU % cannot tell you on an integrated GPU: the 3D engine can read 95 %
while the GPU sits idle part of every frame.

```text
Nx2FrameLimit 1.0 s: 18.4 fps; interval p50 54.31 p95 58.90 p99 62.10 max 84.20 ms; GPU busy p50 49.10 p95 52.70 ms = 90% of the interval (2.15 ms a frame outside it); present image wait 0.31 ms and call 0.80 ms a frame; 6.1 submissions, 412 draws and 74 render passes a frame; gpu: the device is busy almost all of every frame
Nx2FrameLimit slowest submissions of the window: 21.40 ms, 9.80 ms, 6.10 ms
```

## What each figure is

- **interval** – time between two presented frames, at the present call.
- **GPU busy** – the union of the frame's queue submissions' timestamps on the device
  (`vkCmdWriteTimestamp` around each submission), i.e. the time the GPU was actually executing. It
  is the same figure the performance overlay shows as `GPU n%`, sampled every frame instead of once
  a second. Submissions that overlap are counted once, gaps between them are not counted.
- **% of the interval** – `GPU busy p50 / interval p50`. This is the real GPU utilisation.
- **outside it** – the rest of a typical frame: the GPU idle, minus what the present path spent
  waiting (see below). Large means the emulation threads, not the device, decide the frame rate.
- **present image wait** – how long `vkAcquireNextImageKHR` blocked, which is how V-Sync (FIFO) and
  a full swapchain show up.
- **present call** – how long `vkQueuePresentKHR` took.
- **submissions / draws / render passes a frame** – the work the GPU thread hands over. Thousands
  of draws or hundreds of render passes a frame on a bandwidth-shared iGPU usually cost more than
  the scene itself: every render pass break reads and writes the render targets again.

## Reading the verdict

| Verdict | Meaning | Where to look next |
|---|---|---|
| `gpu: the device is busy almost all of every frame` | The device is saturated. The frame rate is what the GPU can do for this scene. | Which passes cost the time: `EDEN_NX2_GPU_PROFILE=1` (render pass ends per site, zones), Intel GPA for occupancy and bandwidth, RenderDoc for a single frame. Cutting emulator-side passes (anti-aliasing, scaler, the game's own upscale network) is what moves it. |
| `mixed: the device is the main limit` | The device is busy most of the frame but not all of it. | Compare with `EDEN_NX2_GUEST_THREAD_STATS=1`: if a guest thread is over the frame budget, the GPU starves on some frames. |
| `cpu/sync: the GPU is idle part of every frame` | The device is not the limit. Look at the threads. | `EDEN_NX2_GPU_PROFILE=1` (front/back/worker waits), `EDEN_NX2_GUEST_THREAD_STATS=1`, `EDEN_NX2_PRESENT_FENCE_TRACE=1`. |
| `present: the swapchain holds frames back` | Image waits dominate: V-Sync, a small swapchain, or a present that cannot keep up. | Try `Immediate` V-Sync or a different present mode; check the window size against the render size. |
| `no GPU sample yet` | The queue family does not report timestamps. | Nothing to do; fall back to the thread timings. |

## Two experiments that separate the cases quickly

1. **Lower the render resolution** (`0.5x`). If the interval barely moves while GPU busy drops, the
   device is not the limit; if both drop together, it is.
2. **`EDEN_NX2_FRAME_LIMIT=1` with the window small** (200x200). The present path and the scaler
   become nearly free: whatever frame time is left is scene work plus emulation.

## Render pass breaks (the cost that hid behind "GPU 95%")

Ocarina of Time on the 386H, 1440x810 scene, 17 fps, `EDEN_NX2_GPU_PROFILE=1`:

```text
Scheduler per second: 1028 submits, 224 empty flushes skipped, 3367 render passes, 1 finishes waiting 2.9 ms
Render pass ends 1118/s at vk_rasterizer.cpp:3066   WaitForIdle: a memory barrier
Render pass ends  541/s at vk_buffer_cache.cpp:621  buffer copies that need a transfer
Render pass ends  421/s at vk_query_cache.cpp:1715  query synchronisation
Render pass ends  232/s at vk_rasterizer.cpp:2275   a clear starting a pass
Render pass ends  206/s at vk_rasterizer.cpp:4006   the draw submission policy's Flush
Render pass ends  179/s at vk_query_cache.cpp:291   CopyQueryPoolResults
Render pass ends  133/s at vk_texture_cache.cpp:2645 image copies
Render pass ends  105/s at vk_graphics_pipeline.cpp:1622 a draw after a dispatch
```

3367 passes a second at 17 fps is **198 render passes a frame**, and the GPU spends its time storing
and reloading render targets: on a tiled GPU (Arc/Xe) every break writes every tile to memory and
reads it back for the next draw. At 1440x810, colour plus depth is ~9.4 MiB a break, so 198 breaks are
1.6-3.4 GiB of traffic a frame, which at the shared memory's real bandwidth is 20-45 ms — the frame.

**What did not work: `WaitForIdle`.** Keeping the pass open across it (`EDEN_NX2_WFI_KEEP_PASS=1`, opt-in, off by default) only moves the break to the query synchronisation the same call triggers (`QueryCacheBase::NotifyWFI` -> `vk_query_cache.cpp:1715`), so the total barely changes; and a pass left open when the game waits for the GPU can present tiles that are not in memory yet (black frames).

**What works: uploads into buffers whose last use has finished.** `CanReorderUpload` refused the upload command buffer whenever the destination region had been used since the last frame tick, even when that use had completed (~32 breaks a frame here). It now applies the test `TickFrame` already uses to retire a buffer's usage tracking, `scheduler.IsFree(LastUsageTick())`: a finished use means nothing in flight reads the region. `EDEN_NX2_REORDER_UPLOAD=0` restores the conservative answer.

**What did not work: emulating mesh instead of native.** `EDEN_NX2_NO_MESH_SHADER=1` hides
`VK_EXT_mesh_shader` so task/mesh programs take the compute-plus-indirect-draw path. Ocarina of Time
issues 68 native mesh draws a frame at 92 us each (6.3 ms), which looked worth attacking, but the
emulated path costs a compute dispatch *and* an indirect draw per mesh draw: render passes went from
157 to 210 a frame, GPU busy from 37 to 48 ms, fps from 21 to 17, and it produced black frames. Native
mesh stays.

## What the GPU time is made of (Ocarina of Time, 1440x810, 41.7 ms busy, 18 187 markers)

| Site | ms a frame | calls | us a call |
|---|---|---|---|
| `vk_rasterizer.cpp:1703` `vkCmdDrawIndexed` | 16.7 | 2655 | 6.3 |
| `vk_rasterizer.cpp:2513` compute dispatch | 8.1 | 147 | 55 |
| `vk_rasterizer.cpp:1695` `vkCmdDrawMeshTasksEXT` | 6.3 | 68 | 92 |
| `vk_query_cache.cpp:292` `CopyQueryPoolResults` | 2.9 | 15 | 195 |
| uploads, flushes, image copies | 1.9 | | |
| barriers, pass begin/end, present | 5.8 | | |

The per-draw figure is the one that matters: 6.3 us for a draw of a few hundred vertices is front-end
state cost (2 bind-stage-buffer calls, descriptor and dynamic-state updates per draw), not shading.
One compute dispatch of a 4x4x6 grid also costs 2.1 ms, which is a serial loop behind 96 threads.

## Comparing two runs

The line carries the window (swapchain) size and, in brackets, the size the game renders, because
both decide the GPU time: the pixels the game draws and the pixels the window path touches, plus
how many draws and passes the area of the game has. Two lines are only comparable when the size, the
scene and the settings match; compare `GPU busy p50` and `draws` / `render passes a frame`, never fps
alone.

Five runs, every line at 2880x1544 with the game rendering 2560x1440 and 2.5 k to 2.9 k draws a frame
(the size never moved; rows D and E repeat C to show the spread of the same settings):

| Run | Settings apart | Render passes a frame | GPU busy p50 | fps |
|---|---|---|---|---|
| A | `EDEN_NX2_DELTA_CB=0`, OoT anti-aliasing "No sharpening" | 244-284 | 56-62 ms | 12.6-16.4 |
| B | `EDEN_NX2_DELTA_CB=1`, OoT anti-aliasing "Game" | 146-173 | 42-49 ms | 15.8-20.5 |
| C | `EDEN_NX2_DELTA_CB=1`, "No sharpening" (`EDEN_NX2_OOT_SKIP_HEAVY=1`) | 145-161 | 36.3-39.3 ms | 22.0-23.1 |
| D | C, guest cores pinned to 0-3 | 147-168 | 38.0-40.6 ms | 20.1-22.5 |
| E | C again, nothing changed | 146-167 | 42.2-45.7 ms | 19.0-21.0 |
| F | C at 0.5x (guest still 2560x1440, window 2880x1544) | 144-150 | 30.4-32.6 ms | 24.6-25.8 |

A against C is the constant-buffer delta alone, both on "No sharpening": 254 pass ends a frame become
150, and 60 ms become 37. Sending the draws that follow a `0x446` back to the full resolve costs ~100
render passes a frame and ~23 ms here, in line with the 34 602 broken reusable draws a second the
setting exists for: the caches re-synchronise and upload per draw, and a copy or an upload has to
leave the pass, which ends it. **Keep `EDEN_NX2_DELTA_CB=1`.**

That comparison also prices a render pass break at this size: 100 more breaks cost 12-23 ms, so
**~0.15-0.25 ms each**, and 150 of them are 22-37 ms of C's 36-46 ms frame. A full store and reload of
colour plus depth at 2560x1440 is 59 MiB (14.75 MiB each, RGBA8 and D32), and on a tiled GPU every break
writes its tiles out and reads them back through the same LPDDR5X the 16 CPU cores use. That is the
frame, and it is why the CPU being idle 4-8 ms a frame changes nothing.

B against C is the OoT anti-aliasing bypass, at the same pass count: "No sharpening" with
`EDEN_NX2_OOT_SKIP_HEAVY=1` is worth the other 5-10 ms. So the bypass never added passes; the
earlier suspicion that it did came from comparing it with a `DELTA_CB=0` run.

**Run to run, the same settings vary by 15 %.** D and E are C's configuration as far as the frame is
concerned (the same ~150 render passes and ~2.7 k draws a frame), and their GPU busy p50 spans 36.3 to
45.7 ms, the fastest and the slowest both being a run where nothing was changed. The pass count
repeats; the GPU time follows the scene (the same draw count can carry different overdraw and
particles) and the device's power and clock state - `force_max_clock` starts a turbo compute loop
(`vk_turbo_mode.cpp`) that holds the clocks up, but it cannot hold a shared package power budget.
Read a difference under ~20 % as noise, confirm a change on `render passes a frame` first, and compare
whole-window ranges rather than single seconds.

**Halving the render resolution buys 24 %, not 100 %.** Row F renders at 0.5x and only removes 5-13 ms
of the GPU time, because most of the frame is per call, not per pixel: ~2.5 k draws at ~5 us is ~13 ms
and the ~146 render passes cost their state either way. It also moves the limit to the CPU: F's interval
is 39-41 ms while the GPU is busy 30-33 ms of it, so 7.5 ms a frame the device waits for the frame to be
built (5.8 ms at 1x), and a shorter frame does not make building it cheaper. That is why F tops out at
~25 fps with the GPU able to do 31, and why a smaller scale again would not help.

0.5x also costs the image: without the game's reconstruction network the frame is upscaled 2.25x with
the bilinear filter, which leaves bright edges ringing and colour fringes. "Use AMD FSR 3.1 upscaling in
Ocarina of Time" (`ocarina_fsr`, `vk_rasterizer.cpp:3240`, `Fsr3Upscaler`) is the switch for that case.

At C to E the device is at 83-91 % of the interval with 4.1-8.3 ms a frame outside it and the slowest
submissions at 4.3-8.0 ms: the GPU is no longer saturated. That outside time is not thread placement
- pinning the six guest cores to the four P-cores left it where it was (5.4-8.3 ms, same passes, same
verdict; `docs/intel-xe3/hybrid-cores.md`). What is left is the frame's own work: ~2.7 k draws a frame
at ~6 us (16 ms) and ~150 render passes at 1440p, both of which are the backend's cost per call, not
a setting.

`EDEN_NX2_GPU_PROFILE=1` names what ends each pass (`render pass ends N/s at file:line`), which is
the figure to bisect on when a setting adds passes.

## Checked upstream (Eglantine), and why it is not ported

This tree is Lichii's nx2; the Eglantine PRs below are later than it, and each was read before writing
anything new (AGENTS.md).

- **#907 "Fix/lean ssbo task mesh"** (merged 18 Nov): marks the global-memory routing helpers
  `DontInline` for every stage but task/mesh, and reports Ocarina's market at ~45 instead of ~58 FPS.
  **Not portable here.** This tree keeps those helpers inlinable and routes them with one branch per
  candidate buffer that merges back to a single return, and says why: `DontInline` with a single route
  drew Ocarina's far forest unculled and at wrong heights on NVIDIA. The test
  `global_memory_coherence.cpp:523` ("Native global routing helpers stay inlinable...") asserts every
  function control word is 0, so the port fails an existing test and would risk that bug. Only the
  compile-time half (2.4 s to 0.6 s for a program with ~30 candidate buffers) is left, and that is a
  first-visit stutter, not frame time.
- **#909 "nx2: OoT Hyrule Market performance bundle"** (merged 18 Nov): a CPU layout for X3D parts,
  `shared_mutex` in the device memory manager, and a set-bit form of the sampled descriptor table in
  the texture pass. Nothing of it is here. The descriptor part is the one to look at if the ~150
  passes and the 6 us a draw are attacked next.
- **#912 "task/mesh shader dropdown"** (merged 18 Nov): a UI choice only; the emulated mesh path is
  already measured as a regression here (`EDEN_NX2_NO_MESH_SHADER`, 157 to 210 passes).

## The pass ends, by site (Ocarina of Time, 1x, ~140 passes a frame)

`EDEN_NX2_GPU_PROFILE=1` counts them by the line that ends them; per second divided by the frame rate:

| Site | a second | a frame | What it is |
|---|---|---|---|
| `vk_rasterizer.cpp:3086` | 1195 | **52** | `WaitForIdle`: the barrier after it must run outside a pass |
| `vk_query_cache.cpp:1715` | 467 | **20** | the query barrier a `WaitForIdle` triggers (`NotifyWFI`) |
| `vk_buffer_cache.cpp:641` | 424 | **18** | an ordered stream upload: a copy may not run in a pass |
| `vk_rasterizer.cpp:4026` | 334 | 15 | the submission policy flushing (128 draws) |
| `vk_rasterizer.cpp:2275` | 289 | 13 | a clear that targets another framebuffer |
| `vk_query_cache.cpp:291` | 179 | 8 | query results copied out of the pool |
| `vk_graphics_pipeline.cpp:1622` | 117 | 5 | a draw right after a dispatch |
| rest (texture cache, `:2490`, `:3126`) | ~150 | 7 | |

Most of the frame's passes are the game's own (the same framebuffer set changes), but **95 of the ~140
are the emulator's**, and the top three share one cause: a transfer or a barrier that the spec forbids
inside a render pass instance. Removing them needs the pass to survive until the next draw that
actually needs the copy - a lazy pass end - not a cheaper barrier.

### Keeping the pass open across `WaitForIdle`, measured

| | the barrier ends the pass | the barrier inside the pass |
|---|---|---|
| render passes a frame | 155-185 | **145-170** |
| GPU busy p50 | **43.4-51.4 ms** | 50.1-57.0 ms |
| time outside the GPU | 8.6-19.9 ms | 4.3-13.3 ms |
| fps | 13.0-18.0 | 14.2-17.8 |

Fewer passes, more GPU time: the break was never the expensive half on Intel. The run that kept the
pass open carried no GPU profile while the one that broke it did (the profile only *adds* cost), so the
higher GPU time is not a profile artefact. `EDEN_NX2_WFI_KEEP_PASS` is opt-in again, and the present
blit keeps closing the pass.

### `EDEN_NX2_FLUSH_DRAWS` measured (128 against 512)

| | 128 (default) | 512 |
|---|---|---|
| submissions a frame | 25-28 | 19-22 |
| render passes a frame | 140-144 | 133-148 |
| the `:4026` site | 15 a frame | 0 |
| GPU busy p50 | 34.8-36.3 ms | 32.5-35.6 ms |
| time outside the GPU | 7.6-10.6 ms | **12.5-18.2 ms** |
| verdict | `mixed` | `cpu/sync: the GPU is idle part of every frame` |
| fps | 21.9-23.0 | **18.9-21.4** |

Bigger submissions do remove the flush's passes and 1.8 ms of GPU time, but every submission is then a
longer chain for the front to wait on (`list writers` and `acquire` waits grow with it), and the device
idles 15 ms a frame instead of 8. **Keep 128.**

## What is left, in order

Audited after the runs and the profile above, with the code in hand:

1. **Keeping the pass open across `WaitForIdle` does not pay on this GPU.** Recording the barrier
   inside the pass (the naive form of a lazy pass end) removes 8-10 of the frame's ~150 breaks - 155-185
   down to 145-170 a frame - but GPU busy *rises*, from 43-51 ms to 50-57 ms, so the frame gets slower.
   A mid-pass barrier with `ALL_COMMANDS` and full memory access makes Intel's driver flush the
   render-target tiles anyway: the store and reload are paid without a break, and the deferred clears a
   break would have realized are lost. The barrier stayed a break and `EDEN_NX2_WFI_KEEP_PASS=1` is
   opt-in again; `BlitScreen::DrawToFrame` keeps closing the pass first, so a narrower barrier (graphics
   and compute stages, attachment and shader access only) can be tried inside a pass safely. That
   measurement also corrects the estimate below the table above: a break is not 0.15-0.25 ms on its own,
   it is cheaper than the tile flush a mid-pass barrier forces.
2. **The two remaining transfer sites** (20 a frame at `vk_query_cache.cpp:1715`, 18 at
   `vk_buffer_cache.cpp:641`) still have to break the pass: a `vkCmdCopyBuffer`/`vkCmdCopyQueryPoolResults`
   may not run inside one, and both copies have to land before the next command that reads what they
   write. Removing them means ending the pass when that command is recorded rather than when the copy
   is, which needs per-resource tracking in the buffer and texture caches.
3. **Query values: a host read does not remove the copy.** `PresyncWrites` copies the pool into a
   resolve buffer, and `SyncValues` then copies that buffer into guest memory, which is where the game
   reads its counters - so `vkGetQueryPoolResults` would add a host read without removing a command.
4. **A pending fragment barrier** (`vk_graphics_pipeline.cpp:1255`) ends the pass; it is the feedback
   loop path, 5 a frame.
5. **The submission policy** is not a lever: measured, `EDEN_NX2_FLUSH_DRAWS=512` removes its 15 passes
   and 1.8 ms of GPU but loses 2 fps to the handoff. 128 stays.

Already optimal, checked and left alone: the clear path (`vk_rasterizer.cpp:2275`) only ends a pass
when the clear targets *another* framebuffer - a deferred clear becomes the next pass's load op, and a
clear on the active framebuffer is a `vkCmdClearAttachments` inside it; the `WaitForIdle` barrier
recording itself (`Scheduler::RecordMemoryBarrier`, opt-in). `Scheduler::NoteRenderPassStores` /
`RenderPassHasStores` is tracked and never read: dead code from an unfinished store/load optimization.

## Notes

- The report costs a timestamp pair per submission and a query read per frame: a few percent in
  the worst case. Turn it off (`EDEN_NX2_FRAME_LIMIT=0`) when you are done.
- A frame with more than 512 submissions is not sampled (`submissions` in the line shows it); the
  GPU busy figure then comes from the last frame that was.
- The numbers are also written by `EDEN_DKB_FRAME_TRACE=1` per frame (`DKB gpu_frame ...`), which is
  the per-frame version of the same measurement.
