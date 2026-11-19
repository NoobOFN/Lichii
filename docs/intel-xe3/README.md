# Lichii on Intel Xe3 (Windows): test build and field notes

This is a community test build of [zortos293/Lichii](https://github.com/zortos293/Lichii) for
people running **Intel Arc Xe3 graphics on Windows**, where the official builds ran Switch 2 titles at
a few frames per second. It is the result of about a week of testing on one laptop, with one game.
It is not an official release and it is not a miracle: it takes Ocarina of Time from 3-5 FPS to
about 22 FPS on an integrated Arc 140V.

Português: [README.pt-BR.md](README.pt-BR.md)

## How this was made

I am not an emulator developer. Everything here, the diagnosis, the code changes, the builds, the
measurements and this document, was done with [Claude Code](https://claude.com/claude-code) while
I tested each step in the game and reported what I saw. Treat it accordingly: the numbers come
from real logs, but the code has not been reviewed by anyone who knows this codebase well.

I am publishing it for two reasons. People with Intel Arc cards are hitting the same wall I hit,
and this gets them past it. And I believe that someone with real expertise in emulation or in
Intel's Vulkan driver, putting the same effort in, can find ways to improve performance on Intel
well beyond what is here. The section "Why it stops at about 22 FPS" is where I would start.

## What you get

| | Official build 88 | This build |
|---|---|---|
| Intro and menus | 3-5 FPS | about 60 FPS |
| Gameplay, most areas | 3-5 FPS | 22-24 FPS |
| Gameplay, heavy areas | 3-5 FPS | 18-20 FPS |
| Foliage flicker and leaf-shaped holes | present | gone |
| Closing the emulator | window hung (builds 130-150) | closes by itself in under 10 s |

Measured on the test machine below, at 1x resolution in handheld mode. Other Arc models will
differ; discrete cards (A-series, B-series) should do better, but nobody has measured this build
on one yet.

## Test machine

- MSI Prestige 16 AI C3MG-011FR, Windows 11 26H2
- Intel Core Ultra 9 Processor 386H (4 performance + 8 efficiency + 4 low power cores, no hyper-threading)
- Intel Arc 140V integrated graphics, driver 32.0.101.8724
- 32 GB RAM
- Game: The Legend of Zelda: Ocarina of Time for Switch 2 (title `0400486024DEC000`)

## What is in the build

Base: upstream `nx2` at build 150 (`17baed716d`). On top of it, six commits:

| Change | Origin | Default |
|---|---|---|
| Upscale stand-in for the game's AI upscaler, with `EDEN_NX2_TENSOR_EMULATION` | [#143](https://github.com/zortos293/Lichii/pull/143) by GonzaCass, carried forward | set to `0` in the bundled `nx2_switches.txt` |
| `EDEN_NX2_PREFLUSH` | [#259](https://github.com/zortos293/Lichii/pull/259) | off |
| Intel Windows driver version comparison, with `EDEN_NX2_INTEL_VIDS` | new | fix on |
| `EDEN_NX2_MESH_INVARIANT`, `EDEN_NX2_DEPTH_EQUAL` (diagnostics) | new | off |
| `EDEN_NX2_GUEST_ANY_CORE` (experiment) | new | off |
| Three-minute limit for the emulation thread to stop | new | on |

The executable is compiled with clang-cl 22.1.8, ThinLTO and the repository's
`lichii-windows.profdata` profile, like the official PGO release. Against the same source built
with MSVC `/O2` it measured about 8% faster.

### The upscaler stand-in (the change that matters)

Ocarina of Time passes every frame through a small neural network that the console runs on
dedicated hardware. Emulated in software on an Arc 140V it used the whole GPU: 99% load, about
190-300 ms per frame. Turning it off without a replacement (`EDEN_NX2_NO_TENSOR`) gave 60 FPS in
the menu and no 3D image at all, because the rest of the frame reads the network's output.

The stand-in replaces the network with a bilinear copy of its input (squared, because the game
stores the square root of each colour channel). Upstream enables it automatically only for Mesa's
Intel driver and the Steam Deck. On Windows you turn it on with:

```text
EDEN_NX2_TENSOR_EMULATION=0
```

The image is softer than with the real network. That is the trade.

### The Intel driver version fix

Intel's Windows driver reports its version as `major << 14 | minor`. The emulator compared that
number with `VK_MAKE_API_VERSION(27, 20, 100, 0)`, so driver 101.8991 looked older than 27.20.100
and `VK_EXT_vertex_input_dynamic_state` was disabled on every current Intel driver. The comparison
now uses Intel's format. If this causes trouble on your driver, `EDEN_NX2_INTEL_VIDS=0` restores
the old behaviour without rebuilding.

### The shutdown fix

On exit the emulator writes the driver's pipeline cache, about 1 GiB for this game. The emulation
thread had 11 seconds to stop and was then terminated mid-write: the cache was lost and the window
froze, because `QThread::terminate` emits `finished` while holding the thread's mutex and the
connected slot calls `isRunning`. The limit is now three minutes. Closing takes 7-9 seconds here.

## Installing

1. Download the zip from the releases page and extract it to its own folder. Do not extract it
   over another Lichii install: this build ships a different `nxformat.dll`.
2. Run `lichii.exe`.
3. On first start, builds from 150 on offer to import the old `%APPDATA%\eden` profile. If you
   have one, choose **Import**, then **Link Old Directory**, and leave **Clear Shader Cache**
   unticked.
4. Install your own keys and firmware and add your own game dump, as with any Lichii build. None
   of that is included here and none of it will be.

The zip already contains an `nx2_switches.txt` with the two switches this guide recommends.

## Recommended settings

These are the developer's recommended values plus what testing changed. Check them after running
any other Lichii fork against the same data folder: one of them rewrote four of these on the test
machine without asking.

| Setting | Value | Why |
|---|---|---|
| GPU accuracy | Low (shown as FAST) | High delays fences and makes the GPU front thread wait on every acquire |
| Asynchronous shader building | On | Needed for the Bounded mode below |
| Switch 2 draws with unbuilt pipelines | Bounded, 8 ms | Wait stalled draws for 4.9 s out of 10 s when entering an area |
| Asynchronous presentation | On | |
| NVDEC emulation | GPU | Only matters for videos |
| Resolution | 1x | 0.75x and below showed heavy artifacts |
| Window adapting filter | Bilinear | Cheapest |
| Add-on "Max Resolution (built-in)" | Off | About 2 FPS (20 to 22) on the 140V |
| Frame generation | Off, or Optical Flow if you stay above 20 FPS | See below |
| Console mode | Handheld | |

`nx2_switches.txt`, beside the executable:

```text
EDEN_NX2_TENSOR_EMULATION=0
EDEN_NX2_PREFLUSH=0
```

Outside the emulator:

- **Page file.** With a 2 GB page file the Windows commit limit was 33.5 GB on a 32 GB machine,
  and the game crashed in the Intel driver (`igvk64.dll`, `0xc0000005`) seconds after Windows
  logged "Virtual Memory Minimum Too Low". Letting Windows manage the page file, or setting it to
  32 GB, ended those crashes.
- **Power.** Play on AC. On battery the GPU sat at 72% with a lower frame rate; plugged in it
  dropped to about 50% for the same scene.

## What was tried and did not help

Useful if you are about to try the same thing.

| Experiment | Result |
|---|---|
| 0.25x resolution, everything at minimum, on the official build | 5.2 to 5.3 FPS. The cost was the upscaler, not the pixels |
| `EDEN_NX2_NO_TENSOR=1` | 60 FPS menu, no 3D image |
| `EDEN_NX2_FRAMES_AHEAD=0` | Stable 19-20 FPS, image almost entirely white |
| `EDEN_NX2_PREFLUSH=1` | 23 to 30 FPS in one short run, but blown-out lighting came back |
| `EDEN_NX2_ASYNC_DISCARD=1` | Fewer stalls, black frames in new areas. Bounded mode replaces it |
| GPU fence behaviour Balanced | Fixed the holes and the lighting, 15-19 FPS and slow motion |
| [#337](https://github.com/zortos293/Lichii/pull/337) together with #215 | No holes, 15 FPS |
| [#264](https://github.com/zortos293/Lichii/pull/264) | 20.3 against 20.9 FPS standing still. Dropped |
| `EDEN_NX2_IDLE_SPIN_US` at 0 and 30 (default 100) | 51 and 49 ms per frame against 45. Lower is worse |
| Reactive flushing off | No change in frame time |
| `EDEN_HYBRID_CORES=0`, `EDEN_NX2_GUEST_ANY_CORE` | No effect on this CPU: placement is already left to Windows below 8 performance logical processors |
| Frame generation, Optical Flow 2x | Runs. Helps above about 20 real FPS, falls apart below |
| Frame generation, LSFG | Unavailable: it needs 16-bit floats, which the emulator turns off on Intel ("broken float16 math") |

## What fixed the graphics bugs

All of these are upstream now; they are listed because the symptoms are easy to recognise.

| Symptom | Cause | Fix |
|---|---|---|
| Wind-moved plants and tree canopies vanishing and returning | Mesh shaders written for 32-wide subgroups ran at 16 on Intel | [#329](https://github.com/zortos293/Lichii/pull/329) |
| Leaf-shaped holes showing bright sky, worst near cave mouths and rocks | Visibility counts read before they were ready, so the prepass and colour pass drew different leaves | [#215](https://github.com/zortos293/Lichii/pull/215) |
| Slow motion below 20 FPS | Dynamic FPS capped its correction at 50 ms | [#299](https://github.com/zortos293/Lichii/pull/299) |
| Sun and lighting blown out | A race that `EDEN_NX2_PREFLUSH=1` exposes | keep `EDEN_NX2_PREFLUSH=0` |

## Why it stops at about 22 FPS

The emulator's own profiler (`EDEN_NX2_GPU_PROFILE=1`) and its sampler (`EDEN_NX2_SAMPLE`) were
run in gameplay. Nothing is saturated:

- The Vulkan worker is idle about 73% of the time and the game never waits on the host GPU fence.
- The GPU back thread waits for work 36% of the time; the front thread sleeps 27% waiting for
  commands and spends another 32% waiting for the back at semaphore acquires, which is a real
  dependency (the 3D channel waits for compute work), not overhead that can be skipped.
- Each guest CPU thread runs translated code only 15-22% of the time.

The time goes to threads waiting for each other. About ten time-critical threads share eight
cores without hyper-threading here, which makes every hand-off slower. A CPU with eight or more
performance cores should do better; that is a guess from the code's own thresholds, not a
measurement.

Measured later on the Core Ultra 9 386H with `EDEN_NX2_FRAME_LIMIT=1`, the answer is the device, not
the hand-offs: 36-46 ms of GPU busy time inside a 43-51 ms interval (83-91 %), ~2.7 k draws and ~150
render passes a frame at 2560x1440, and 4-8 ms a frame outside the GPU time. The largest single item
is what a draw costs the driver - 2.7 k draws at ~6 us is ~16 ms - and the second is the 150 render
passes at 1440p. `EDEN_NX2_DELTA_CB=0` puts ~100 of those passes back and costs ~23 ms, so it stays at
1. The figures, and the 15 % run-to-run spread of the same settings, are in
[Where the frame time goes](gpu-latency.md).

[Hybrid cores on Intel](hybrid-cores.md) has the switches used to measure thread placement on this
kind of CPU: background shader work on the E-cores (`EDEN_HYBRID_BACKGROUND`), a worker-count cap
(`EDEN_NX2_PIPELINE_WORKERS`), guest-core masks, and a skip for the one compute pass Ocarina of
Time dispatches whose result the anti-aliasing bypass never reads (`EDEN_NX2_OOT_SKIP_HEAVY`).

[Where the frame time goes](gpu-latency.md) documents `EDEN_NX2_FRAME_LIMIT=1`, which prints the
real GPU busy time (queue timestamps, not Task Manager's number), the frame interval percentiles,
the present waits and the submissions, draws and render passes per frame, with a verdict: the
device, the swapchain, or the emulation threads leaving the GPU idle.

## Known issues

- Heavy areas still drop to 18-20 FPS.
- The log shows `Uncached storage pointer classification` errors for a few pipelines at boot.
- The first visit to an area stutters while shaders build. It improves as the cache fills.
- Only Ocarina of Time was tested. Donkey Kong Bananza is excluded from the stand-in on purpose:
  its tensor programs must run.

## Building it yourself

You need Visual Studio 2022 Build Tools, CMake, Ninja, Git, Qt 6.8.3 (`msvc2022_64`, with
`qtcharts`) and LLVM 22.1.8. `tools/windows/intel-arc/` has the two scripts used for this build;
edit the paths at the top.

```bat
configure-clang.cmd
build-clang.cmd
```

The flags are the ones the official `windows-x86_64-pgo` job uses: `-flto=thin` and
`-fprofile-use=lichii-windows.profdata`. A full build took about an hour on the test laptop.

## Reporting results

If you try this on another Arc, the useful facts are: GPU model, driver version, game, the
`last 600 presents: frame N us` lines from `log/eden_log.txt` in a place you can describe, and
which settings differ from the table above.

## Credits and licence

- [zortos293](https://github.com/zortos293/Lichii) and the Lichii contributors, for the emulator
  and nearly every fix listed here.
- GonzaCass for the upscaler stand-in (#143), and the authors of #215, #259, #299 and #329.
- Eden, yuzu, Sudachi and Eglantine, which Lichii builds on.

Licensed under GPL-3.0-or-later, like the project it derives from. The source for the released
binary is the branch this file is on. This build is not affiliated with or endorsed by the Lichii
project or by Nintendo. Use your own console's keys, firmware and game dumps.