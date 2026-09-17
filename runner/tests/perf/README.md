# M4: hardware vs simulator validation

Measures BIMSA on this machine's UPMEM hardware and on uPIMulator and reports
the error. This is the study the runner was built for.

    runner/tests/perf/sweep.sh <input.seq> <seq-size> <cycles|instructions> <tasklets...>

## Result

BIMSA, `inputs/n2_l129_e2.seq` repeated to 16 pairs, edit distance, 1 DPU,
`MAX_DISTANCE_THRESHOLD=50`, `PERF_TIER=3`, `DYNAMIC=0 BATCH_SIZE=0`.
Measured 2026-09-17.

**Instruction counts — the functional model.**

| tasklets | hardware | uPIMulator | error |
|---|---|---|---|
| 1 | 607552 | 607552 | exact |
| 4 | 2427616 | 2428320 | +0.03% |
| 16 | 4878128 | 4884112 | +0.12% |

**Cycle counts — the timing model.**

| tasklets | hardware | uPIMulator | error |
|---|---|---|---|
| 1 | 7502864 | 7138112 | -4.86% |
| 2 | 7577552 | 7203856 | -4.93% |
| 4 | 7761952 | 7351264 | -5.29% |
| 8 | 8385904 | 8527168 | +1.68% |
| 16 | 8385328 | 8598000 | +2.54% |

uPIMulator is optimistic by about 5% when one or a few tasklets run, and
pessimistic by about 2.5% once the pipeline is full. The whole range is inside
±5.3%. Hardware run-to-run variation is 0.04%, so the trend is real and not
noise.

The sign change is where the model's remaining error lives. At one tasklet the
DPU issues an instruction every 11 cycles no matter what, so a cycle count is
11 x instructions plus stalls, and the stalls are almost entirely MRAM. Per
instruction, hardware spends 12.33 cycles and uPIMulator 11.57: the model
reproduces the 11-cycle issue interval exactly but only about 43% of the stall.
Once 8 or more tasklets are ready the revolver hides the stalls behind other
threads, that error disappears, and a smaller one of the opposite sign is left.

## Method

Both sides run the same kernel source with the same macros. `build_bimsa.sh`
takes the command `make -n` would use for the hardware and rewrites it for the
linker, rather than restating the 40 macros and letting them drift. This is not
fussiness: the compiler lays a loop out differently at the slightest change. An
early comparison here differed by 25% purely because one build fused a loop
counter into a conditional add and the other did not.

Measurements come from BIMSA's own perfcounter probes, not from the simulator's
statistics, so both sides measure the same span of the same code. Comparing
hardware's `PERF_S_TOTAL` against uPIMulator's `num_instructions` would not:
`num_instructions` covers the whole launch, which is 3% more than the probe.

Every data point is gated on `CPU recovery for 0 pairs (0.00%)` and
`Results are equal`. A run where the CPU finished the work, or got it wrong,
measures nothing.

### The counter is one register the whole DPU shares

Confirmed here on hardware with `kernel.c`, which has every tasklet measure the
same two-instruction loop:

| tasklets | instructions seen per tasklet | cycles per tasklet |
|---|---|---|
| 1 | 2016 | 22144 |
| 2 | 4016, 4032 | 22112, 22128 |
| 4 | 8032, 8048, 8032, 8048 | ~22140 |

Each tasklet sees every tasklet's instructions, so the readings scale with the
tasklet count while the cycles stay flat -- the barrel scheduler filling issue
slots that one tasklet leaves empty. Never sum these over tasklets. BIMSA's host
prints the sum, so its `[PERF] total` is inflated by the number of live
tasklets; the sweep compares hardware against simulator, where that factor
cancels, and checks that both sides had the same number live before it does.

### Counter semantics

`perfcounter_get()` reads the register with `time` and shifts left by 4, so the
register advances once per 16 events and the low four bits of any measurement
are lost. The calibration above shows the reconstructed value is the true count,
not a scaled one: a 1000-iteration two-instruction loop reports 2016
instructions and 22128 cycles, which is 2 instructions and 22 cycles per
iteration plus one quantum of overhead. 22 is 2 x 11, the issue interval.

## What this needed from the simulator

- `time` and `time_cfg` were not implemented, so an instrumented kernel could
  not run at all. They now model one counter per DPU with the same 16-event
  granularity.
- `CLOCKS_PER_SEC` was never filled in. On hardware libdpu writes the rank's
  frequency there at load; nothing wrote it here, so any kernel converting
  cycles to a duration divided by zero. The simulator now writes the frequency
  it models, which is 350 MHz, the same as this hardware.

## Limitations

- One workload. BIMSA at 16 short pairs is not a claim about every kernel, and
  the error is known to move with the input: the same comparison at a single
  pair gives -6.2% rather than -4.9%.
- One DPU. Nothing here tests the channel, rank or host-transfer models.
- `PERF_TIER=3` measures the kernel as a whole. Which part of it the model gets
  wrong is a tier-1 or tier-2 question, not answered here.
- At 16 tasklets each tasklet's probe window covers about half the launch, on
  both hardware and the simulator alike. The comparison stays sound because both
  measure the same thing, but the absolute numbers at 16 are not whole-launch
  cycles.
