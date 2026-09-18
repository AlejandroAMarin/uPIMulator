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

| tasklets | hardware | uPIMulator | error | before the MRAM fit |
|---|---|---|---|---|
| 1 | 7505888 | 7551984 | +0.61% | -4.86% |
| 2 | 7549264 | 7582528 | +0.44% | -4.93% |
| 4 | 7762160 | 7690064 | -0.93% | -5.29% |
| 8 | 8371376 | 8613184 | +2.89% | +1.68% |
| 16 | 8392320 | 8686496 | +3.51% | +2.54% |

Hardware run-to-run variation is 0.04%, so these are real.

## MRAM latency

The -5% the first measurement found was all in MRAM. `mram_kernel.c` isolates
it: with one tasklet a DMA blocks the only thread there is, so the stall is
fully exposed and a cycle count is the latency itself rather than something the
scheduler hid.

    runner/tests/perf/mram_sweep.sh

| bytes per DMA | hardware | before | after |
|---|---|---|---|
| 8 | 208.7 | 167.6 (-20%) | 207.6 (-0.5%) |
| 64 | 232.2 | 206.2 (-11%) | 235.3 (+1.3%) |
| 256 | 341.8 | 340.5 (-0.4%) | 329.5 (-3.6%) |
| 1024 | 714.2 | 877.5 (+23%) | 706.6 (-1.1%) |
| 2048 | 1291.3 | 1594.6 (+23%) | 1209.5 (-6.3%) |

The model was wrong in two opposite directions at once, which is why the
aggregate error looked like a modest 5%: too little fixed cost per DMA, and too
much per byte. Three things fixed it.

**The DMA engine's startup was not modelled at all.** The hardware charges
about 40 logic cycles before a transfer's first byte reaches memory, whatever
its size, and the DRAM timings do not describe that. `DmaStartupCycles`, 42.

**`t_cl` was 32 and the machine says 22.** MRAM has no datasheet to read this
off; upstream's 32 made a 2 KB `mram_read` 23% slower than the machine.

**The DMA queue never counted its timer down.** `Dma.Cycle` serviced its queues
but never cycled them. It went unnoticed while every timer was zero, and the
first non-zero startup stalled the DPU forever. Then, once cycling, only the
front entry counted down -- an issue interval, not a latency -- so the startup
serialised across tasklets and cost 5% at 16 of them. `CycleAll` fixes that:
several tasklets' transfers are in flight at once, as on the hardware.

The row buffer is left alone. Hardware shows no difference at all between
walking MRAM in order and jumping 64 KB each time, so its activation and
precharge are not observable from a DPU; the model's remaining sensitivity to
them is about 11 cycles, small beside the 42-cycle startup, and both patterns
now sit within 6%.

## DPU sweep## DPU sweep

Same workload on each DPU (4 pairs, 4 tasklets), the DPU count swept and the
input regenerated each time so the work per DPU stays fixed.

    runner/tests/perf/sweep_dpus.sh <base-pair.seq> 200 4 4 cycles 1 2 4 8 16

| DPUs | pairs | hardware | uPIMulator | error | before the MRAM fit |
|---|---|---|---|---|---|
| 1 | 4 | 1895424 | 1898816 | +0.18% | -4.88% |
| 2 | 8 | 3899728 | 3854384 | -1.16% | -5.29% |
| 4 | 16 | 7795520 | 7708768 | -1.11% | -5.48% |
| 8 | 32 | 15581664 | 15417536 | -1.05% | -5.36% |
| 16 | 64 | 31224128 | 30835072 | -1.25% | -5.45% |

The error does not grow with the DPU count. uPIMulator's per-DPU figure is
identical to the digit from 2 DPUs up and the hardware's is constant to 0.2%:
DPUs share nothing during a kernel on either side, and the model composes
accordingly.

**This does not test the channel model.** See below.

## Host transfers

Transfers used to cost nothing. `api.CopyTo` and `api.CopyFrom` wrote DPU
memory directly, and `Host.Schedule`/`Host.Check` -- which build channel
messages -- were reached only from `Simulator.Init`, the standalone
`--benchmark` path. A host-to-DPU transfer under the runner advanced the
simulator by zero cycles, so no timing result depended on it.

They are charged now. The shim calls `upim_charge_transfer` once per
`dpu_push_xfer` or `dpu_copy_*`, not once per DPU: the DPUs of a rank move
their data together, and charging each in turn would report a whole rank as
slow as one DPU repeated. The cost lands in a set-wide `transfer_cycle`
counter that `UPIM_STATS` prints, kept apart from the DPU counters because a
transfer covers several DPUs and no DPU counter sees it. Transfers and kernels
never overlap here, so the two simply add.

### The model

    cycles = setup + max( bytes / dpu_rate , bytes * dpus / rank_rate )

A transfer pays a fixed setup and then moves its bytes, limited both by the one
DPU's link and by the rank interface every DPU on it shares. Writing to 8 DPUs
therefore costs the same as writing to one, because the rank interface is
nowhere near saturated; writing to a full rank is limited by the interface
instead. The first transfer a set makes pays an extra 250 us to bring the rank
up.

| parameter | value | what it is |
|---|---|---|
| `DpuWriteMbps` | 966 | one DPU's write link |
| `DpuReadMbps` | 250 | one DPU's read link |
| `RankWriteMbps` | 9700 | the rank interface, writing |
| `RankReadMbps` | 3400 | the rank interface, reading |
| `TransferSetupNs` | 30000 | before a byte moves |
| `TransferFirstNs` | 250000 | extra, first transfer only |

All seven are settable from the environment -- `UPIM_DPU_WRITE_MBPS`,
`UPIM_TRANSFER_SETUP_NS` and so on -- so a study can vary the link without
rebuilding. Setting a rate to zero makes transfers free again.

### Where the numbers come from

`xfer_host.c` sweeps the DPU count and the transfer size in both directions on
real hardware and reports the **median** of many repetitions, not the best. The
best is what the link can do on a warm path; the median is what a program
actually waits for, and that is what a simulator should predict. Calibrating on
the best gave a setup of 23 us and under-predicted BIMSA's transfer time by
half.

The first repetition of all is discarded and reported separately. It costs
250 us against a median of 30 -- the rank is being brought up -- and folding it
into an average would corrupt every point. It is modelled explicitly instead,
because a short run is dominated by it: it is 40% of everything BIMSA spends
moving data.

    XPROFILE=backend=hw UPMEM_PROFILE=backend=hw runner/tests/perf/xfer_host > sweep.tsv

### How close it is

Over the 98 measured points, the median error is 11% and the mean 15%. Below
256 KB per DPU, which is the range a real kernel transfers in, the median is
11% and the worst 41%.

| bytes/DPU | DPUs | dir | hardware us | model us | error |
|---|---|---|---|---|---|
| 512 | 1 | write | 29 | 31 | +7% |
| 512 | 1 | read | 33 | 32 | -2% |
| 512 | 64 | write | 42 | 33 | -21% |
| 512 | 64 | read | 67 | 40 | -41% |
| 32768 | 1 | write | 56 | 64 | +15% |
| 32768 | 1 | read | 163 | 161 | -1% |
| 32768 | 64 | write | 178 | 246 | +38% |
| 32768 | 64 | read | 638 | 647 | +1% |
| 262144 | 1 | write | 303 | 301 | -1% |
| 262144 | 1 | read | 1104 | 1079 | -2% |
| 262144 | 64 | write | 2695 | 1760 | -35% |
| 262144 | 64 | read | 6294 | 4964 | -21% |

End to end on the workload that matters, BIMSA at 16 pairs on one DPU, eleven
transfers totalling 15 KB:

| | hardware | uPIMulator | error |
|---|---|---|---|
| transfers | 0.646 ms | 0.620 ms | -4.0% |

### Where the shape is wrong

Two places, both measured rather than suspected.

The real machine's cost per DPU grows with the chunk size somewhere between 512
bytes and 4 KB, rather than switching from a fixed setup to a fixed rate. That
is why 512 bytes to 64 DPUs is under-predicted.

A scatter of large buffers to many DPUs falls below what the link can do,
because the host has to gather them: 256 KB to each of 64 DPUs measures 2695 us
against a link capability nearer 1760. That is host memory bandwidth, not the
machine, and it gets worse the larger the buffers. A study transferring
multi-megabyte chunks to a full rank should measure its own case and set
`UPIM_RANK_WRITE_MBPS` accordingly.

Neither is a two-limit shape, and no setting of these parameters reproduces
them. They are documented rather than fitted, because a curve fitted to this
machine would be worse than useless for M5, which exists to change the machine.

## Method## Method

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
- The rank and channel topology is still untested: the runner puts every DPU on
  one rank of one channel, so nothing here exercises more than one of either.
- Transfers are charged analytically, not simulated as traffic. That is exact
  while transfers and kernels do not overlap, which is the case here. A study
  using asynchronous transfers would need them modelled as events instead.
- `PERF_TIER=3` measures the kernel as a whole. Which part of it the model gets
  wrong is a tier-1 or tier-2 question, not answered here.
- At 16 tasklets each tasklet's probe window covers about half the launch, on
  both hardware and the simulator alike. The comparison stays sound because both
  measure the same thing, but the absolute numbers at 16 are not whole-launch
  cycles.
