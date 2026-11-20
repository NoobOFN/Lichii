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

The per-draw figure is an average, and `DKB gpu_work_shape` (added for this) shows the average says
nothing on its own: over 2 663-2 868 draws a frame the middle draw costs 1.0-1.5 us, the 90th
percentile 13.5-14.6 us and the 99th 60-81 us, with 10 % of draws under 0.25 us. Half the draws are
worth under 1.8 ms of the ~19 ms (9 %), so merging or batching them cannot pay: the cost is in a
minority of heavy draws, which is guest geometry and shading rather than per-draw front-end state.
The front end still owns the CPU side of a draw (two `bind stage buffers` calls, 0.5 us each), where
there is slack anyway.

One compute dispatch of a 4x4x6 grid also costs 1.5-2.1 ms, which is a serial loop behind 96 threads.

## The occlusion query copies: waiting on every query

`PresyncWrites` copies every pending query pool into a resolve buffer, 17-18 times a frame, and the
copy asked for `VK_QUERY_RESULT_WAIT_BIT` over the whole range. The AMD path already knows why that
is wrong there - "AMD's drivers wait on each query of a WAIT_BIT copy separately (3.8 us per query,
~5 ms a frame in Ocarina)" - and waits on the newest query only, then copies the rest unwaited.
Intel pays the same per-query wait - 220 us a copy for ~70 slots, 3.1 us each - so the same
wait-on-the-newest copy now runs there too.

The GPU time it saves is real. In the runs with the frame trace on, the copy site fell from 3.4-4.3 ms
a frame (17-19 calls) to 2.7-2.8 ms (17-18), and in a same-session A/B without the trace, 44 windows
each at scene 1440x810, `GPU busy p50` went from **40.49 ms to 37.79 ms**.

**It does not raise the frame rate.** The same A/B, whole-range wait against wait-on-the-newest: 21.0
fps median for the former and 20.8 for the latter, with the time outside the GPU going from 7.1 to
10.3 ms a frame. The 2.7 ms become idle, which means the frame period at ~21 fps is not GPU-bound:
whatever limits it takes the same time either way. At 84 % GPU the device was the main limit in the
trace runs; once the GPU drops to 79 % the chain outside it becomes the limit instead.

An earlier comparison of two different sessions (GPU busy 50.8-51.7 -> 42.0-44.1, fps 16.2-18.3 ->
18.8-19.9) claimed 7.7 ms and up to 2 fps from this. That comparison was between two sessions and is
withdrawn: the numbers above are the same-session A/B and they settle it.

The remaining 2.7 ms is not overhead either: the copy reads the newest query, so the GPU has to reach
it, and the guest asks for results 17-18 times a frame. Waiting later is not an option
(`vkGetQueryPoolResults` would add a host read without removing the copy, see below).

It ships for AMD and Intel unconditionally (both are the drivers where the per-query wait was
measured); other vendors keep the whole-range wait.

## The limiter is no longer the GPU

The A/B above is the first measurement in this file where the GPU time drops and the frame rate does
not follow. At ~21 fps with scene 1440x810 and `GPU busy p50` at 37.8-40.5 ms of a 47.6-48.1 ms frame,
the GPU is 79-85 % busy and the rest of the chain sets the period. Cutting GPU work further will keep
turning into idle, so the next questions are about that chain, not about the GPU:

- `EDEN_NX2_GUEST_THREAD_STATS=1`, once every 10 s: which guest thread sits on a core and for how long,
  split into JIT (game code, counter polling, hint spin, cached memory) and outside it (SVCs and
  kernel), plus its waits by kind. That answers whether the game's main thread is over the frame budget.
- The `Front waits for the back in 10 s by site` line at `HW_GPU:Info`: `acquire 2345.0 ms` and
  `list writers 1865.3 ms` per 10 s in these runs are 23 % and 19 % of the wall time, which is the
  front stalling on the split.
- `EDEN_NX2_PRESENT_FENCE_TRACE=1` and the `Async read-back per second` line, for the present path and
  the guest reads of GPU-written memory.

Whether the GPU becomes the limit again depends on the scene size: the game picks it, and at 2560x1440
the GPU work above would dominate again.

## The guest threads are not over the frame budget either

`EDEN_NX2_GUEST_THREAD_STATS=1` never reported before this session: its flag was an `inline const
bool` read during static initialization, before the frontend applies `nx2_switches.txt`, so a switch
set only in the file was read as unset. `Nx2Profile` has `ReadSwitch()` for exactly that reason; the
thread stats now do too, called from `Core::System::Initialize`.

First reading, Ocarina of Time, Kokiri Forest, scene 1440x810, 217 presents in 10 s, ms per frame:

| thread | on core | of it JIT | cached memory | waiting | biggest wait |
|---|---|---|---|---|---|
| t88 (busiest) | 6.52 | 5.95 (game 4.01) | 1.92 in 5 430 calls, 1.07 read-back | 39.25 | sync objects 27.58 in 3 waits |
| t155 | 6.29 | 5.81 (game 3.96) | 1.83 in 3 739 | 39.46 | address 37.64 in 24 |
| t156-t159 | 5.3-5.7 | 4.9-5.3 | 1.8-2.1 | 40.0-40.4 | address 37.6-38.6 |
| t95 (presents 1 a frame) | 0.89 | 0.50 | - | 44.54 | sleep 26.79 in 20 |

Per core: JIT 8.4-10.6 ms a frame, 7 400-10 300 JIT exits a second, all but a few hundred of them SVC,
counter polling and hint spins at zero.

So the heaviest guest thread works 6.5 ms of a 47 ms frame and the presenting thread under 1 ms:
nothing in the guest is near the 16.7 ms budget. Combined with the A/B above - the GPU at 82-85 % and
its front and back threads at 42 % and 58 % of a core - no stage is saturated, yet the frame takes
47 ms. What the guest does spend its wait on is GPU synchronisation: 27.58 ms in three waits, which is
the fence of the work it queued (double or triple buffering).

That is a latency chain, not a throughput limit, and it is why 2.7 ms of GPU time became idle instead
of frames. The next experiment tested the largest single handoff in that chain: `EDEN_NX2_SPLIT=0`
runs the GPU engines on one thread instead of the front/back pair, at the same spot and scene size.

## The GPU threads are the saturated stage

`EDEN_NX2_SPLIT=0`, same spot, same scene: **16.5-17.6 fps** against 21.1-21.6, `GPU busy p50` 45-46.3 ms
against 37.5-39.2, and the guest's sync wait grew from 27.58 to 39.55 ms. The split pays for itself;
what it costs is not the problem, and the switch is gone from `nx2_switches.txt`.

What the profile shows instead, per second of wall time:

| | GPU front | GPU back |
|---|---|---|
| host CPU busy | 418 ms | 579 ms |
| waiting on the other one | `acquire` 219 + `list writers` 173 | `split wait` 428 |
| total | ~1000 ms | ~1007 ms |

Both GPU threads are at 100 % of their own timeline: they are either working or waiting on each other,
while the device is 82 % busy. So the stage that sets the frame is the GPU *emulation* CPU plus the
coherence serialization between its two halves, not the device. That matters because, unlike device
time, CPU saved here converts to frames: the earlier 2.7 ms of device time became idle, but 2.7 ms
taken off the back thread's 579 ms/s is on the critical path.

The back thread's work, largest first (ms per second): method dispatch 139.5 (2.98 M methods/s), bind
stage buffers 66.5 (122 418 calls, 0.54 us each), draw 40.4, update buffers 32.5, compute 31.9, read
handles 30.9, configure 29.8, fill image views 13.4, push descriptors 12.4, dynamic state 11.9.

Next: `Renderer.pin_gpu_threads` (off until now) gives the front and the back a physical performance
core each and keeps the guest cores off them, which two threads at 100 % should feel.

Measured, same spot and scene, warm-up windows dropped, 36 windows without pinning against 31 with:

| | pinned off | pinned on |
|---|---|---|
| fps median (min-max) | 20.8 (19.5-21.8) | **21.4 (20.3-22.0)** |
| fps mean | 20.84 | 21.36 |
| `GPU busy p50` median | 37.76 ms | 37.64 ms |
| time outside the GPU | 9.46 ms | **8.75 ms** |

The device does the same work; what falls is the time outside it, by 0.7 ms a frame, which is what
handing each GPU thread its own core should do. That is the first change in this file that moved the
frame rate without touching the device, and it is worth about 3 %.

It is still two sessions rather than one A/B (the setting is read at startup, so a toggle needs a
restart), so it needs one run with the setting back at `false` to show the number returns to 20.8
before it counts as proven.

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

**The GPU times in this table are not a fixed workload.** With the "Max Resolution (built-in)" add-on
off (it is off on this install: `Skipping "Max Resolution (built-in)": disabled in Add-Ons`), Ocarina
of Time runs its own dynamic resolution: the scene target moved between 864x480 and 1440x810 inside
single runs. The game changes the pixel count to hold its GPU time, so `GPU busy` is partly an output
of that controller, and none of these runs recorded the scene size per window. The pass and draw
counts do not depend on it and stand. What does not stand is any price per render pass derived from
these GPU times (an earlier version of this page gave 0.15-0.25 ms a break): it is unmeasured. The
report now prints the scene size (`game renders 2560x1440, scene 1120x630 to 1440x810`); compare GPU
times only between windows that show the same scene size, or with the add-on on.

B against C is the OoT anti-aliasing bypass, at the same pass count: "No sharpening" with
`EDEN_NX2_OOT_SKIP_HEAVY=1` is worth the other 5-10 ms. So the bypass never added passes; the
earlier suspicion that it did came from comparing it with a `DELTA_CB=0` run.

**Run to run, the same settings vary by 15 %.** D and E are C's configuration as far as the frame is
concerned (the same ~150 render passes and ~2.7 k draws a frame), and their GPU busy p50 spans 36.3 to
45.7 ms, the fastest and the slowest both being a run where nothing was changed. The pass count
repeats; the GPU time does not. The game's dynamic resolution (above) is the first suspect and was not
recorded in those runs; overdraw and the package's power state are others, none of them separated.
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

**Not conclusive.** Only the pass count is a result: about 10 fewer a frame with the barrier inside the
pass. The GPU times cannot be compared: the scene size moved between 864x480 and 1440x810 in both
runs, neither run had the GPU profile on, and which log had the switch set rests on how the two files
were labelled, since the logs do not print the switches. Whether a mid-pass barrier costs more than a
pass break on this driver is open. `EDEN_NX2_WFI_KEEP_PASS` is opt-in until an A/B at a fixed scene
size says otherwise; the present blit closes the pass either way.

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

1. **Keeping the pass open across `WaitForIdle` is untested in effect.** Recording the barrier inside
   the pass removes about 10 of the frame's ~150 breaks; what that does to the GPU time is unknown (see
   the table above). `EDEN_NX2_WFI_KEEP_PASS=1` stays opt-in and `BlitScreen::DrawToFrame` closes the
   pass first, so it is safe to A/B again at a fixed scene size, also with a narrower barrier (graphics
   and compute stages, attachment and shader access only).
2. **The two remaining transfer sites** (20 a frame at `vk_query_cache.cpp:1715`, 18 at
   `vk_buffer_cache.cpp:641`) still have to break the pass: a `vkCmdCopyBuffer`/`vkCmdCopyQueryPoolResults`
   may not run inside one, and both copies have to land before the next command that reads what they
   write. Removing them means ending the pass when that command is recorded rather than when the copy
   is, which needs per-resource tracking in the buffer and texture caches.
3. **Query values: a host read does not remove the copy.** `PresyncWrites` copies the pool into a
   resolve buffer, and `SyncValues` then copies that buffer into guest memory, which is where the game
   reads its counters - so `vkGetQueryPoolResults` would add a host read without removing a command.
   Its per-query wait was the exception and is fixed (`EDEN_NX2_QUERY_WAIT_LAST`, see above); the
   2.7 ms that stays is the GPU reaching the newest query, 17-18 times a frame.
4. **A pending fragment barrier** (`vk_graphics_pipeline.cpp:1255`) ends the pass; it is the feedback
   loop path, 5 a frame.
5. **The submission policy** is not a lever: measured, `EDEN_NX2_FLUSH_DRAWS=512` removes its 15 passes
   and 1.8 ms of GPU but loses 2 fps to the handoff. 128 stays.
6. **Merging or batching draws is settled against.** `DKB gpu_work_shape` puts the middle draw at
   1.0-1.5 us and the 99th percentile at 60-81 us, so the cheap half of a frame's draws is worth under
   1.8 ms of 19. Only the heavy minority costs, and that is guest geometry and shading. Worth
   remembering before anyone writes a draw merger.

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
