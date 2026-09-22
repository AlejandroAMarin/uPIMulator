# M4: hardware vs simulator validation

Measures BIMSA on this machine's UPMEM hardware and on uPIMulator and reports
the error. This is the study the runner was built for.

    runner/tests/perf/sweep.sh <input.seq> <seq-size> <cycles|instructions> <tasklets...>

## Result

BIMSA, `inputs/n2_l129_e2.seq` repeated to 16 pairs, edit distance, 1 DPU,
`MAX_DISTANCE_THRESHOLD=50`, `PERF_TIER=3`, `DYNAMIC=0 BATCH_SIZE=0`.
Cycle counts measured 2026-09-22, instruction counts 2026-09-17. Instruction
counts do not depend on the timing model, so they were not measured again.

**Instruction counts — the functional model.**

| tasklets | hardware | uPIMulator | error |
|---|---|---|---|
| 1 | 607552 | 607552 | exact |
| 4 | 2427616 | 2428320 | +0.03% |
| 16 | 4878128 | 4884112 | +0.12% |

**Cycle counts — the timing model.**

| tasklets | hardware | uPIMulator | error | after the MRAM fit | first measurement |
|---|---|---|---|---|---|
| 1 | 7503040 | 7660720 | +2.10% | +0.61% | -4.86% |
| 2 | 7556384 | 7719440 | +2.16% | +0.44% | -4.93% |
| 4 | 7777440 | 7894560 | +1.51% | -0.93% | -5.29% |
| 8 | 8408720 | 8510288 | +1.21% | +2.89% | +1.68% |
| 16 | 8449344 | 8563696 | +1.35% | +3.51% | +2.54% |

Hardware run-to-run variation is 0.04%, so these are real.

**What matters here is the shape, not only the size.** The "after the MRAM fit"
column looks better at 1 and 4 tasklets, but its error grows from -0.9% to
+3.5% as tasklets are added, which says something in the model scales wrongly.
Two corrections since -- the memory controller's throughput and the register
file's bank conflicts -- removed that trend. What is left is a flat +1.2% to
+2.2%, and the MRAM measurement below accounts for it: the per-byte rate is
1.6% high because `t_cl` counts whole memory cycles.

Both corrections had to be made together. Each on its own makes BIMSA look
worse, because the old model was fast on memory and slow on register conflicts
and the two errors cancelled.

## MRAM latency

The -5% the first measurement found was all in MRAM. `mram_kernel.c` isolates
it: with one tasklet a DMA blocks the only thread there is, so the stall is
fully exposed and a cycle count is the latency itself rather than something the
scheduler hid.

    runner/tests/perf/mram_sweep.sh

| bytes per DMA | hardware | upstream | first fit | now |
|---|---|---|---|---|
| 8 | 209.7 | 167.6 (-20%) | 207.6 (-0.5%) | 191.6 (-8.6%) |
| 64 | 232.4 | 206.2 (-11%) | 235.3 (+1.3%) | 223.3 (-3.9%) |
| 256 | 341.6 | 340.5 (-0.4%) | 329.5 (-3.6%) | 329.5 (-3.5%) |
| 1024 | 714.2 | 877.5 (+23%) | 706.6 (-1.1%) | 754.6 (+5.6%) |
| 2048 | 1367.7 | 1594.6 (+23%) | 1209.5 (-6.3%) | 1347.6 (-1.5%) |

The model was wrong in two opposite directions at once, which is why the
aggregate error looked like a modest 5%: too little fixed cost per DMA, and too
much per byte. Three things fixed it.

The last column is worse than the one before it at four of the five sizes, and
that is a deliberate trade. The first fit read the latency alone and got a
combination of constants that suited one tasklet and no other; fitting the
throughput as well (below) costs a few percent here and removes 15% under
contention, which is what a real kernel runs into. **The machine itself carries
about 0.50 cycles per byte at one tasklet and 0.545 at saturation**, so no
single rate serves both, and the model has one rate.

**The DMA engine's startup was not modelled at all.** The hardware charges a
few tens of logic cycles before a transfer's first byte reaches memory, whatever
its size, and the DRAM timings do not describe that. `DmaStartupCycles`.

**`t_cl` was 32 and the machine says about 25.** MRAM has no datasheet to read
this off; upstream's 32 made a 2 KB `mram_read` 23% slower than the machine.

**The DMA queue never counted its timer down.** `Dma.Cycle` serviced its queues
but never cycled them. It went unnoticed while every timer was zero, and the
first non-zero startup stalled the DPU forever.

The row buffer is left alone. Hardware shows no difference at all between
walking MRAM in order and jumping 64 KB each time, so its activation and
precharge are not observable from a DPU.

The numbers above are the first fit, which read the latency alone. **MRAM under
contention** below corrects the same two constants against the throughput as
well, and settles the startup's semantics: it is one engine, doing one thing at
a time.

## Register-file bank conflicts

    runner/tests/perf/regbank_sweep.sh

This was worth a fifth of the DPU's time at 16 tasklets and nothing had ever
checked it.

The register file has an even bank and an odd bank. Each serves at most two of
one instruction's registers in a cycle, counting the destination together with
the two sources, so three registers of one parity need a second issue slot.
Read the measurement at one tasklet, where the revolver gives a thread one slot
every 11 cycles and a lost slot is unmistakable:

| instruction | registers | hardware | uPIMulator |
|---|---|---|---|
| `add r4, r4, r5` | 1 even, 1 odd | 11.02 | 11.01 |
| `add r4, r6, r8` | 3 even | 20.80 | 20.78 |
| `add r5, r7, r9` | 3 odd | 19.58 | 20.78 |
| `add r4, r5, r7` | 1 even, 2 odd | 11.02 | 11.01 |
| `add r4, r5, r6` | 2 even, 1 odd | 11.02 | 11.01 |
| `add r5, r4, r6` | 2 even, 1 odd | 12.24 | 11.01 |
| `sub r10, r6, r8` | 3 even | 20.80 | 20.78 |
| `add r6, r7, r9` | 1 even, 2 odd | 11.02 | 11.01 |

Cycles per instruction, eight copies of the instruction per iteration. Nine of
the eleven cases agree within 0.3%, at 1 tasklet and at 16 alike. Two are one
lost slot per iteration out of nine instructions away, and both involve the
boundary between the loop's own instruction and the next: a rule that looks at
one instruction at a time cannot express that.

**The old rule was wrong twice over.** It counted this instruction's sources
together with the register the instruction *before* it wrote, and charged
`floor(even/2) + floor(odd/2)`. That fires on pairs the machine serves for
free, so a plain loop paid a penalty on a third of its instructions. And it
held the whole DPU's pipeline for the penalty rather than the thread's issue
slot, so a single tasklet never paid at all -- the revolver hid it -- while
sixteen paid too much. Measured against the machine, the conflict-free case was
35% slow at 16 tasklets and the conflicting case 6% slow, and at one tasklet the
model showed no penalty where the machine nearly doubles.

**A conflict costs one issue slot of the DPU, charged to the thread.** At one
tasklet the thread's next turn is 11 cycles away, so the instruction takes 22
cycles. At sixteen the slot is one cycle another tasklet would have used. Both
follow from the same rule and both match. See `CycleRule.ConflictTurns` and the
stall in `Logic.ServiceThreadScheduler`.

## MRAM under contention

    runner/tests/perf/mram_tasklet_sweep.sh

`mram_sweep.sh` uses one tasklet, so a DMA blocks the only thread and what it
reports is a latency. The same kernel with more tasklets reports the memory
controller's throughput instead, because the revolver keeps other threads ready.

The machine saturates at 4 tasklets and then obeys one line exactly, over five
size steps:

    cycles per DMA = 38.1 x ceil(bytes / 1024) + 0.545 x bytes

Two things in it are worth stating. **The fixed 38 cycles is the DMA engine's
own setup, not a row activation.** Pointing every tasklet at the same MRAM row
costs exactly what pointing them 64 KB apart costs, so the row buffer is not
what the number measures. And **a transfer larger than 1 KB pays the setup
twice**: the engine works in bursts of that size.

**The engine does one thing at a time.** The two terms add rather than one
hiding the other, which says the setup of one transfer does not overlap the
data of the one before it. The model now reflects that: `Dma` hands the memory
controller one transfer and waits for it to finish, and the startup only counts
down while the controller is idle.

Error after fitting, cycles per DMA, sequential:

| bytes | 1 tasklet | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| 8 | -8.5% | -3.8% | -7.4% | +5.7% | +5.8% |
| 64 | -4.1% | -1.3% | +2.2% | +2.3% | +2.2% |
| 256 | -3.7% | +1.8% | +1.8% | +1.8% | +1.8% |
| 512 | +3.2% | +1.7% | +1.6% | +1.6% | +1.6% |
| 1024 | +5.7% | +1.6% | +1.6% | +1.6% | +1.6% |
| 2048 | -1.4% | +0.5% | +0.0% | -0.2% | -0.3% |

It was -16% to -40% at 4 tasklets and above before this. The residual +1.6% is
the per-byte rate: `t_cl` is an integer count of memory cycles and one step of
it is worth 3.3%, so 25 is the closest the model can get to the machine's 0.545
cycles per byte. Do not chase it by moving `memory_frequency`, which is a
documented number.

## DPU sweep

Same workload on each DPU (4 pairs, 4 tasklets), the DPU count swept and the
input regenerated each time so the work per DPU stays fixed.

    runner/tests/perf/sweep_dpus.sh <base-pair.seq> 200 4 4 cycles 1 2 4 8 16

| DPUs | pairs | hardware | uPIMulator | error | first measurement |
|---|---|---|---|---|---|
| 1 | 4 | 1896656 | 1931712 | +1.85% | -4.88% |
| 2 | 8 | 3895792 | 3960496 | +1.66% | -5.29% |
| 4 | 16 | 7813408 | 7920992 | +1.38% | -5.48% |
| 8 | 32 | 15601984 | 15841984 | +1.54% | -5.36% |
| 16 | 64 | 31240480 | 31683968 | +1.42% | -5.45% |

The error does not grow with the DPU count, and it is the same +1.4% to +1.9%
the tasklet sweep shows. uPIMulator's per-DPU figure is identical to the digit
from 2 DPUs up and the hardware's is constant to 0.3%: DPUs share nothing
during a kernel on either side, and the model composes accordingly.

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
- The rank and channel topology is still untested: the runner puts every DPU on
  one rank of one channel, so nothing here exercises more than one of either.
- Transfers are charged analytically, not simulated as traffic. That is exact
  while transfers and kernels do not overlap, which is the case here. A study
  using asynchronous transfers would need them modelled as events instead.
- `PERF_TIER=3` measures the kernel as a whole. `PERF_TIER=1` does not split
  this workload any further: with edit distance its `align`, `base` and `queue`
  probes record no calls at all and 99.6% of the time lands in `other`. Use a
  gap-affine input if a finer split is wanted.
- The two register layouts the bank-conflict model still gets wrong are each one
  lost slot per iteration, at the boundary between the loop's own instruction
  and the next. A rule that looks at one instruction at a time cannot express
  that, and a rule that looks at pairs has not been measured.
- At 16 tasklets each tasklet's probe window covers about half the launch, on
  both hardware and the simulator alike. The comparison stays sound because both
  measure the same thing, but the absolute numbers at 16 are not whole-launch
  cycles.
