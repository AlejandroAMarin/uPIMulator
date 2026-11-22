# PrIM kernels: hardware against the simulator

The study's second and third workloads. BIMSA alone agreeing to 2% is not a
validated simulator, and these are chosen to be unlike it and unlike each
other.

    runner/tests/prim/run.sh [work-dir]
    KERNELS="va hst" TASKLETS_LIST="1 4 16" NELEM=16384 REPS=5 runner/tests/prim/run.sh

`run_prim.sh` runs the other eleven PrIM kernels with PrIM's own hosts; see
"The other eleven kernels" below.

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
| 1 | 1391344 | 1394944 | +0.26% | exact |
| 2 | 728976 | 729472 | +0.07% | -0.01% |
| 4 | 370720 | 371776 | +0.28% | +0.00% |
| 8 | 199664 | 201392 | +0.87% | +0.01% |
| 16 | 145808 | 149056 | +2.23% | -0.57% |

**`hst` — shared histogram**

| tasklets | cycles hw | cycles sim | error | instructions |
|---|---|---|---|---|
| 1 | 4739712 | 4735392 | -0.09% | exact |
| 2 | 2387488 | 2385328 | -0.09% | -0.00% |
| 4 | 1205600 | 1205152 | -0.04% | -0.01% |
| 8 | 617584 | 617232 | -0.06% | -0.01% |
| 16 | 467312 | 467008 | -0.07% | -0.07% |

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
| `hst` | one read | -0.04% to -0.09% |
| BIMSA | short scattered reads and writes | +1.48% to +2.12% |
| `va` | two reads and one write | +0.07% to +2.23% |

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

## Update, 2026-09-28: the numbers above are from before three model changes

The register-bank rule, the DMA engine refit and MRAM refresh (see
`runner/tests/perf/README.md`) all move these kernels. With the current model:

| `va`, elements | 1 | 2 | 4 | 8 | 16 tasklets |
|---|---|---|---|---|---|
| 16384 | -0.10% | -3.40% | +0.09% | +1.23% | +2.17% |
| 65536 | | | -0.08% | -0.23% | +0.38% |
| 262144 | | | -2.24% | -0.09% | -0.11% |

`hst` is inside 0.20% at every tasklet count.

`va` is where the model is least certain, and the reason is measured. With 2
to 8 tasklets its threads spend most of their time computing and meet in the
DMA engine only now and then. Whether two of them lock into a convoy, each
waiting for the other's transfer every iteration, decides a few percent. The
hardware breaks some convoys and keeps others: at 2 tasklets it keeps one,
which the model with refresh does not; at 8 tasklets on the largest input it
breaks one, which the model without refresh did not. See
`runner/tests/perf/trace_kernel.c` for the per-transfer trace that shows it.
Before refresh was modelled, the same table read +3.16% and +4.35% at the
worst points; now the worst is -3.40%.

**At 2 tasklets the machine itself is bistable.** Twenty runs of one binary
gave 13 readings at 728.8k to 729.2k cycles and 7 spread from 701.6k to 726.5k.
In the first state the two threads run almost in phase and wait for each
other's reads in every iteration; in the second they have drifted apart and
never meet. The refresh phase at a launch, which the machine does not fix,
decides it. The median of five is usually the first state; the model lands in
the second (703.9k), 0.2% below the smallest reading of a 9-run sweep. So the
-3.4% is the difference between the two states, not an error in either one.

`run.sh` now prints the smallest and largest hardware reading next to the
median. With REPS=9, 2026-09-29:

| tasklets | hardware median | min..max | uPIMulator |
|---|---|---|---|
| 1 | 1391552 | 1391216..1391776 | 1389728 |
| 2 | 728928 | 705104..729232 | 703936 |
| 4 | 370752 | 361840..371056 | 371152 |
| 8 | 199024 | 198352..200192 | 201120 |
| 16 | 146400 | 145616..146864 | 148944 |

At 8 and 16 tasklets the model is outside the machine's range, by 0.5% and
1.4%: those are real errors. At 1, 2 and 4 it is at an edge of it or inside.

## The other eleven kernels, with PrIM's own hosts (2026-10-03)

`run_prim.sh` runs every other PrIM kernel, and nothing of PrIM is edited:

    runner/tests/prim/run_prim.sh                       # all eleven, 1, 4 and 16 tasklets
    TASKLETS_LIST="1 16" REPS=3 runner/tests/prim/run_prim.sh RED SEL
    SKIP_SIM=1 runner/tests/prim/run_prim.sh TS         # the machine only, to size an input

- **The kernel** is `benchmark/<K>/dpu/task.c`, compiled with
  `-Dmain=prim_main` and linked with `probe_main.c`. That `main` starts the
  counter, runs PrIM's `main` in every tasklet, and reads the counter after a
  barrier. The window covers the whole kernel.
- **The host** is `benchmark/<K>/host/app.c`, built for 1 DPU and for the
  kernel's tasklet count, with the macros of the benchmark's Makefile. It
  checks its own results, and a point counts only if it prints that they are
  equal. `DPU_LOAD_OVERRIDE` points it at the probed kernel on both sides.
- **The count** comes from `runner/scripts/launch_probe.c` on the machine and
  from the shim in the simulator (`PROBE_OUT`, `PROBE_MODE`): after every
  launch, each DPU's `OUT_COUNT`. SCAN-RSS, SCAN-SSA and TRNS launch twice and
  MLP three times; the table gives the sum.
- PrIM does not ship HST-L's input image, so the script writes 1 Mi random
  12-bit values (seed 1) in its place. The host computes its reference from
  the same file.

| kernel | what it uses | input |
|---|---|---|
| BS | binary search: 8-byte reads at scattered addresses | 512 queries |
| GEMV | matrix-vector product: sequential reads, 32-bit multiplication in software | 64 x 512 |
| HST-L | histogram into one shared array under a mutex | 16384 elements |
| MLP | three GEMV layers with ReLU, three launches | 128 x 128 |
| RED | 64-bit sum, a handshake chain across tasklets | 65536 elements |
| SCAN-RSS | reduce, then scan, two launches, handshakes | 32768 elements |
| SCAN-SSA | scan, then add, two launches, handshakes | 32768 elements |
| SEL | filter, output offsets passed by handshake | 65536 elements |
| TRNS | in-place transpose of tiles, small transfers, mutexes, two launches | 64 x 16 x 8 |
| TS | time-series distance profile: multiplication and division | 512, query 64 |
| UNI | remove consecutive duplicates, handshakes | 65536 elements |

The result, 1 DPU, the machine taken as the median of five runs (model of
2026-10-02):

| kernel | tasklets | cycles hw | cycles sim | error | instructions | hw cycles min..max |
|---|---|---|---|---|---|---|
| BS | 1 | 5440272 | 5491744 | +0.95% | exact | 5439664..5442448 |
| BS | 4 | 1869056 | 1863104 | -0.32% | exact | 1861920..1874688 |
| BS | 16 | 1689328 | 1678848 | -0.62% | +0.01% | 1688992..1690144 |
| GEMV | 1 | 7668640 | 7656160 | -0.16% | exact | 7666912..7669456 |
| GEMV | 4 | 1928176 | 1925552 | -0.14% | exact | 1927968..1928976 |
| GEMV | 16 | 713264 | 712336 | -0.13% | -0.00% | 712768..713408 |
| HST-L | 1 | 5556688 | 5547440 | -0.17% | exact | 5555360..5556880 |
| HST-L | 4 | 1400496 | 1398400 | -0.15% | -0.02% | 1400144..1401648 |
| HST-L | 16 | 1602704 | 1599696 | -0.19% | -0.04% | 1602464..1603792 |
| MLP | 1 | 10951168 | 10933776 | -0.16% | +0.01% | 10948992..10951744 |
| MLP | 4 | 2769680 | 2766064 | -0.13% | exact | 2767680..2770672 |
| MLP | 16 | 1047680 | 1048976 | +0.12% | -0.00% | 1046496..1049520 |
| RED | 1 | 4039984 | 4048016 | +0.20% | exact | 4038592..4042464 |
| RED | 4 | 1013728 | 1016272 | +0.25% | exact | 1013328..1014112 |
| RED | 16 | 353008 | 352128 | -0.25% | exact | 352848..353280 |
| SCAN-RSS | 1 | 4663040 | 4677824 | +0.32% | exact | 4662480..4664288 |
| SCAN-RSS | 4 | 1410336 | 1417328 | +0.50% | exact | 1410096..1410528 |
| SCAN-RSS | 16 | 707008 | 708992 | +0.28% | -0.00% | 706960..707152 |
| SCAN-SSA | 1 | 5079248 | 5093136 | +0.27% | exact | 5077936..5081232 |
| SCAN-SSA | 4 | 1517280 | 1533616 | +1.08% | +0.00% | 1516592..1518512 |
| SCAN-SSA | 16 | 851776 | 854016 | +0.26% | exact | 851440..852016 |
| SEL | 1 | 5399968 | 5404768 | +0.09% | exact | 5397152..5402352 |
| SEL | 4 | 1725136 | 1735968 | +0.63% | exact | 1724640..1725344 |
| SEL | 16 | 822704 | 826464 | +0.46% | exact | 822560..822960 |
| TRNS | 1 | 7247984 | 7245056 | -0.04% | exact | 7245440..7250640 |
| TRNS | 4 | 1852496 | 1849136 | -0.18% | -0.03% | 1850832..1853232 |
| TRNS | 16 | 876224 | 878608 | +0.27% | +1.12% | 873136..876992 |
| TS | 1 | 9046240 | 9028800 | -0.19% | exact | 9044704..9046800 |
| TS | 4 | 2585936 | 2581936 | -0.15% | exact | 2585264..2586512 |
| TS | 16 | 1790592 | 1788304 | -0.13% | -0.00% | 1790496..1791568 |
| UNI | 1 | 4181920 | 4203936 | +0.53% | +0.00% | 4176608..4192848 |
| UNI | 4 | 1327200 | 1337968 | +0.81% | exact | 1326208..1327808 |
| UNI | 16 | 583072 | 584752 | +0.29% | exact | 582832..583504 |

"exact" means the same count; -0.00% and +0.00% differ by a few
instructions.

Over the 33 points the mean absolute error is 0.32%, the rms 0.40%, the
range -0.62% to +1.08%, and 32 points are inside 1%. Every result was right on
both sides. Instruction counts are exact or inside 0.04%, except TRNS at 16
tasklets (+1.12%), where tasklets wait on mutexes: the simulator spins a few
more times, and the cycles are still +0.27%.

Six points are at +0.5% or more. Five are kernels that pass partial results
from tasklet to tasklet by handshake: SCAN-SSA, SEL, UNI and SCAN-RSS at 4
tasklets, and UNI at 1. Why they are slow is not measured. The sixth is BS at
1 tasklet (+0.95%): one tasklet reading 8 bytes at scattered addresses, which
is cause 1 of the accepted limits in `tasks_left.md` (an idle read is 1 to 4
cycles slow in the model). One point, SCAN-SSA at 4 tasklets (+1.08%), is just
outside 1%. It is not studied further, by the decision of 2026-10-02.

## Adding another PrIM kernel to run.sh

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

`run_prim.sh` needs none of this: it uses PrIM's own host and leaves the
kernel unchanged. Add a `config` line for the kernel instead. With the
linker path (`UPIM_ELF=0`) `GEMV` and `MLP` will not link, because the runtime
copy lacks `seqread`; the ELF path, the default, runs them.
