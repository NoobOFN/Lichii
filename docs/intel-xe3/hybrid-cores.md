# Hybrid cores and shader workers on Intel (4 P + 8 E + 4 LP E)

These switches exist to measure thread placement on hybrid Intel CPUs such as the Core Ultra 9
386H. All of them are off unless set, in `%APPDATA%\lichii\nx2_switches.txt` (or the environment);
none of them changes the emulated result.

## What the emulator does on such a CPU today

- `src/common/thread.cpp` places time-critical threads on the performance cores only when there
  are at least `MinPerformanceProcessors` (8) of them. With 4 P-cores that placement is skipped:
  Windows schedules the six guest cores, the GPU front and back, the Vulkan worker and the timing
  thread (nine time-critical threads) on their own.
- `pin_gpu_threads` needs `MinCoresForGpuThreadPinning` (6) physical performance cores
  (`src/common/core_pinning.h`), so it is skipped too. The log says so once:
  `GPU thread pinning skipped: 4 physical performance cores, needs 6`.
- Shader workers are sized `hardware_concurrency() - 1` (`GetTotalPipelineWorkers`) and are
  placed by `SetCurrentThreadToBackgroundWork()`, which did nothing on a CPU without the
  performance-core placement above: nothing ever put them on the slower cores.

## EDEN_HYBRID_BACKGROUND=1

`SetCurrentThreadToBackgroundWork()` selects the efficiency and low-power CPU sets for shader
workers and other background jobs, on any hybrid CPU, including one with too few performance
cores for the time-critical placement. Workers are low priority and not latency-sensitive, so they
cannot take a core from the guest cores or the GPU threads; what they lose is compile speed, which
shows as a longer first-visit stutter. That is why it is a switch and not the default.

Accepted by the beside-application switches file. `EDEN_HYBRID_CORES=0` still turns all of it off.

## EDEN_NX2_PIPELINE_WORKERS=<n>

Caps the shader worker count (2 to 64; without it, `hardware_concurrency() - 1`, which is 15 on the
386H). Each worker is another thread competing with the emulation. Lower it to 2-4 if shader
compiles on first visits crowd the P-cores, and compare `Runtime pipelines in the last 10 s` and
`Nx2Stutter` lines from `EDEN_NX2_STUTTER_TRACE=1`.

## EDEN_NX2_OOT_SKIP_HEAVY=1

The Legend of Zelda: Ocarina of Time (`0400486024DEC000`) dispatches a heavy compute pass between
the anti-aliasing network's prepass and the network itself, and only the network reads its result.
With "Ocarina of Time anti-aliasing: No sharpening"
(`docs/nx2/oot-network-bypass.md`) the prepass and the network are skipped, so that dispatch is
skipped too: the single dispatch that follows each skipped prepass. If the game ever reads it, the
symptom would be a missing effect in gameplay; compare a frame dump with and without.

## EDEN_NX2_CORE_MASK (existing)

Windows CPU set ids for the guest cores, e.g. `EDEN_NX2_CORE_MASK=0-3` puts all six guest cores on
the four P-cores, `EDEN_NX2_CORE_MASK=0-1,4-7` keeps two of them off the P-cores. It overrides the
automatic guest-core pinning. Read the result with `EDEN_NX2_GUEST_THREAD_STATS=1` (`Guest threads
over 10.0 s`: time on a core, ready without a core, waits by kind).

**Measured on Ocarina of Time at 2880x1544 (1x), the frame at 37-40 ms of GPU time:** `0-3` did not
help. The time outside the GPU time stayed where it was unpinned (5.4-8.3 ms a frame against
5.5-7.4 ms), with the same ~150 render passes and the same verdict (`mixed`). Leave it unset: with
the GPU at 85-87 % of the interval, six guest cores on four P-cores buys nothing and only takes
cores away from the GPU threads. Pin only when `EDEN_NX2_GUEST_THREAD_STATS=1` shows a guest thread
ready without a core, or a thread over the frame budget.

The whole value must be numbers and commas. A trailing comment on the line makes it invalid -
`ParseCpuList` refuses anything it cannot read to the end, the log warns `Ignoring invalid
EDEN_NX2_CORE_MASK value '...'`, and the guest stays unpinned. On the 386H the automatic pinning is
skipped anyway (4 performance cores, `MinPerformanceProcessors` is 8), so an invalid mask leaves the
six guest cores to Windows, on the same four P-cores as the GPU threads.

## Measuring a placement change

1. `EDEN_NX2_GPU_PROFILE=1 EDEN_NX2_GUEST_THREAD_STATS=1 EDEN_NX2_STUTTER_TRACE=1` in the switches
   file, same save and camera, 60 s per run.
2. Frame times: the `nx2:vi2 last 600 presents` line, and PresentMon for 1 % low.
3. Thread placement: Windows Performance Recorder / Analyzer, "CPU Usage (Precise)", one row per
   logical processor, to see which of the 16 share the time-critical threads.
4. Change one switch per run; keep the change only if the median average frame rate rises and p99
   frametime does not.
