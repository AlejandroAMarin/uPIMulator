# PrIM kernels: hardware against the simulator

The study's second and third workloads. BIMSA alone agreeing to 2% is not a
validated simulator, and these are chosen to be unlike it and unlike each
other.

    runner/tests/prim/run.sh [work-dir]
    KERNELS="va hst" TASKLETS_LIST="1 4 16" NELEM=16384 REPS=5 runner/tests/prim/run.sh

| kernel | what it does | character |
|---|---|---|
| BIMSA | wavefront alignment | small working set, much arithmetic, short scattered transfers |
| `va` | vector addition | two 1 KB reads and one write per block, almost no arithmetic between them |
| `hst` | shared histogram | one 1 KB read per block, then 256 indexed WRAM updates, then a reduction across tasklets through a shared array and two barriers |

Both kernels are PrIM's own, taken from `golang/uPIMulator/benchmark/*/dpu/task.c`
with only a counter probe added. **Do not edit them further.** A workload that
is changed to suit the simulator measures the simulator, not the machine.

## Result

16384 elements, 1 DPU, hardware taken as the median of five runs.

**`va` — vector addition**

| tasklets | cycles hw | cycles sim | error | instructions |
|---|---|---|---|---|
| 1 | 1391184 | 1397248 | +0.44% | exact |
| 2 | 728976 | 731360 | +0.33% | -0.01% |
| 4 | 371216 | 372784 | +0.42% | +0.00% |
| 8 | 198864 | 202080 | +1.62% | +0.01% |
| 16 | 146720 | 149936 | +2.19% | -0.57% |

**`hst` — shared histogram**

| tasklets | cycles hw | cycles sim | error | instructions |
|---|---|---|---|---|
| 1 | 4741856 | 4735472 | -0.13% | exact |
| 2 | 2387584 | 2385744 | -0.08% | -0.00% |
| 4 | 1205600 | 1205264 | -0.03% | -0.01% |
| 8 | 617792 | 617264 | -0.09% | -0.01% |
| 16 | 467296 | 467040 | -0.05% | -0.16% |

## Input size

16 tasklets, the size swept instead. At 4096 elements each tasklet handles a
single 1 KB block, so barriers and start-up are most of the work and even the
instruction count moves about; at 65536 it is 16 blocks each and the kernel
dominates.

| kernel | elements | cycles hw | cycles sim | error | instructions |
|---|---|---|---|---|---|
| `va` | 4096 | 46768 | 48224 | +3.11% | -2.01% |
| `va` | 16384 | 146720 | 149936 | +2.19% | -0.57% |
| `va` | 65536 | 495408 | 498992 | +0.72% | -0.15% |
| `hst` | 4096 | 147344 | 147568 | +0.15% | -0.54% |
| `hst` | 16384 | 467296 | 467040 | -0.05% | -0.16% |
| `hst` | 65536 | 1747264 | 1744688 | -0.15% | -0.04% |

`va` improves as the input grows, which says its error at 16 tasklets is mostly
the fixed cost of a launch rather than the kernel. `hst` is flat.

## What the three workloads say together

**The residual error tracks how much of the time is spent moving data.**

| workload | transfers per 1 KB of work | error |
|---|---|---|
| `hst` | one read | -0.03% to -0.13% |
| BIMSA | short scattered reads and writes | +1.21% to +2.16% |
| `va` | two reads and one write | +0.33% to +2.19% |

That is what a per-byte memory rate 1.6% above the machine's predicts, and it
is the one error `runner/tests/perf` still records. A kernel that barely
touches MRAM is right to a tenth of a percent; one that does nothing else
carries the whole 1.6%. Nothing else in the model is off by enough to see.

Instruction counts are exact at one tasklet for both kernels and inside 0.01%
up to eight. The -0.16% and -0.57% at sixteen are the order the tasklets reach
the final barrier in, not a functional difference.

## Two things the harness has to do

**Close the probe's window with a barrier.** The counter is one register the
whole DPU shares. If tasklet 0 stops reading when it finishes its own share,
the count depends on the order the tasklets happen to finish in: `va` reported
instruction counts 2.4% apart between the machine and the simulator for that
reason alone, and cycle errors of 4.7% that were not real. A `barrier_wait`
before the closing read fixes it, and the barrier's own cost lands on both
sides equally.

**Take the median of several hardware runs.** These kernels are short by
BIMSA's standard -- 150 thousand cycles at 16 tasklets against 8 million -- and
the hardware repeats to only about 1%. A single reading made `va` at 2 tasklets
look 1.78% out when the median says 0.33%. `REPS` controls it; the simulator is
exact from run to run and is measured once.

## Adding another PrIM kernel

1. Copy `benchmark/<NAME>/dpu/task.c` here as `<k>_kernel.c` and add the probe:
   `PROBE_MODE`, `OUT_COUNT`, a barrier, a read after the first `barrier_wait`
   and another after a closing barrier. Mark every added line `PROBE`.
2. Write `<k>_host.c` on the pattern of `va_host.c`: fill
   `DPU_INPUT_ARGUMENTS`, copy the input to `DPU_MRAM_HEAP_POINTER_NAME`,
   launch, read the result back and check it against a CPU reference, then read
   `OUT_COUNT`. A timing comparison against a kernel that computed the wrong
   answer is worth nothing.
3. Add `<k>_bench` and `<k>_flags` to `run.sh`, taking the flags from the
   benchmark's own `dpu/CMakeLists.txt` so both sides build what PrIM builds.

`GEMV` and `MLP` will not link: they use `seqread.h`, which the 2025 SDK cannot
build. See the SDK port notes in the top-level `CLAUDE.md`.
