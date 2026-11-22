# Handoff: hardware-vs-simulator validation study

State as of 2026-09-30. Read this before touching anything.

## Goal

Find out whether uPIMulator's timing model is trustworthy, by running the same
UPMEM workload on this server's real hardware and on the simulator and comparing.
If it holds, change the *simulated* architecture to explore designs no hardware
exists for.

To do that, uPIMulator had to become a **runner**: `upimulator ./host args` must
execute an already-compiled, unmodified UPMEM host binary with every DPU
operation served by simulation. That part works. BIMSA first, GIMSA next.

## Status

| Milestone | State |
|---|---|
| M0 hardware isolation | **Done**, proven by test |
| M1 simulator as a library | **Done** |
| M2 memory and speed | **Done** (147 MB -> 2.5 MB per DPU; 2.26x faster) |
| M3 libdpu shim | **Done**, BIMSA runs end to end |
| M4 validation study | **Validated on BIMSA, GIMSA and all 13 PrIM kernels** (the other eleven PrIM kernels: -0.62% to +1.08%, rms 0.40%, 2026-10-03). Instructions match hardware exactly; kernel cycles inside 0.6% over 1-16 tasklets on BIMSA, inside 0.2% on PrIM's histogram, inside 0.24% and 0.99% on GIMSA's two phases (0.85% together) once both sides run the same kernel build, and inside 2.3% on PrIM's vector addition except -3.4% at 2 tasklets; every BIMSA section inside 0.7% but one tiny one; MRAM reads and writes inside 2.6% from 2 to 16 tasklets; transfers inside 4% |
| M5 architecture exploration | **Cancelled** pending a decision on what to explore. See `tasks_left.md` |

## The former blocker: eleven functional bugs, all fixed

Double-precision arithmetic was wrong, which made BIMSA's own MRAM guard reject
its workload. Chasing it found ten more defects in the same layers. All are
fixed, and `runner/tests/alu/run.sh` holds them fixed by diffing 1224 results
against UPMEM's own functional simulator.

| Defect | Effect |
|---|---|
| DMA honoured the low 3 bits of both addresses; the hardware ignores them | every unaligned transfer moved a few bytes, losing the data. This alone made BIMSA report distance 0 for alignments it had computed correctly |
| `sh32`/`nsh32` were never set by any instruction | every 64-bit shift took the shift<32 path. This is what broke double precision |
| `xor` returned its second operand; `nxor` returned the complement of it | every `xor` with a non-zero first operand was wrong |
| `rol`/`ror` overwrote the rotated-in bits with ones | `rol`/`ror` behaved as `lsl1`/`lsr1` |
| the subtract carry was the borrow; the hardware reports its complement | every `c`/`nc` condition after a subtract was inverted |
| `sub`/`subc` computed signed overflow with the result's sign inverted | `xgts`/`xles` were wrong whenever the operands' signs differed |
| `subc` used the wrong borrow test when a borrow came in | wrong carry out of a multi-word subtract |
| `xgtu`/`xleu` were both set when the carry was set, and `xles` could never be set at all | 64-bit compares took the wrong branch |
| `pl`/`mi` tested the unsigned image of the result, so `mi` could never be set | every negative result looked positive |
| `ExecuteAddRrrci`/`ExecuteAddZrrci` discarded the ALU's overflow | `ov`/`nov` never fired after an add |
| `lsl1x`, `lsr1x` and `ExecuteSRrr` were not implemented | panic on use |
| `ExecuteSErri` was not implemented | panic on use. A narrow load widened to 64 bits in one instruction, written `lbu.u d2, r0, 20`. GIMSA stopped the simulator on one; BIMSA never emits it |

**None of the 13 PrIM benchmarks uses double precision**, and PrIM never hits an
unaligned DMA either, so uPIMulator's published validation never exercised any
of this. VA still passes, byte-for-byte, after every change.

### How to find the next one

The method that found all eleven, in order of cost:

1. Write the smallest kernel that shows the difference. Compile it twice: once
   with plain `dpu-upmem-dpurte-clang` for the reference, once with `-S -g0
   -DNR_THREADS=24` and uPIMulator's bundled headers for the linker.
2. Run the reference under `UPMEM_PROFILE=backend=simulator`. It is the ground
   truth for anything the ISA documentation does not state, and it touches no
   hardware. Do not guess semantics: `sh32` turned out to be bit 5 of the shift
   operand, not "shift >= 32", and a shift of 64 proves it.
3. Run the same kernel under `UPIM_TRACE=1` (instruction per cycle) or
   `UPIM_TRACE=2` (with the register file) and diff. `UPIM_TRACE` is the
   simulator's own `--verbose`, not the shim's call trace (`UPIM_VERBOSE`).
4. Reduce the level-2 trace to one line per instruction showing only the
   registers that changed. A 39K-instruction BIMSA run becomes readable, and
   that is how the lost distance was traced to a single `sdma`.
5. Compare against `dpu-objdump -d` of the reference ELF to see which branch
   the hardware would have taken.

`go run ./cmd/parsecheck <file.S>` reports the first line the linker's grammar
cannot accept. The linker itself only says "stack are not stmts" after building
the whole SDK, which is far too slow to bisect against. `hash` and
`.section .stack_sizes` are the two known gaps.

## M4: the result

BIMSA, 16 short pairs, edit distance, 1 DPU. Full method, caveats and the
calibration behind it in `runner/tests/perf/README.md`.

| tasklets | cycles, hardware | cycles, uPIMulator | error |
|---|---|---|---|
| 1 | 7503296 | 7536288 | +0.44% |
| 2 | 7550528 | 7582192 | +0.42% |
| 4 | 7754496 | 7777504 | +0.30% |
| 8 | 8375856 | 8401488 | +0.31% |
| 16 | 8371920 | 8420080 | +0.58% |

Measured 2026-09-28, after the bank rule was corrected, the DMA engine
refitted and MRAM refresh modelled (see "The bank rule, corrected", "The DMA
engine, refitted" and "MRAM refresh, and convoys" below). It was +1.48% to +2.12% before the first, and -0.56% to -1.30%
between the two.

Instruction counts match exactly at 1 tasklet, +0.03% at 4, +0.12% at 16.

Sweeping DPUs instead, with the work on each held fixed at 4 pairs and 4
tasklets:

| DPUs | cycles, hardware | cycles, uPIMulator | error |
|---|---|---|---|
| 1 | 1895584 | 1931648 | +1.90% |
| 2 | 3914576 | 3956080 | +1.06% |
| 4 | 7793488 | 7912160 | +1.52% |
| 8 | 15577120 | 15824320 | +1.59% |
| 16 | 31253920 | 31648640 | +1.26% |

The error does not grow with the DPU count. uPIMulator's per-DPU figure is
identical to the digit from 2 DPUs up and the hardware's is constant to 0.2%,
so DPUs are independent during a kernel on both and the model composes.

**The timing model is trustworthy to about 0.6% over the whole range, and the
error does not depend on the tasklet count.** That last part is the point.
The first fit gave -0.9% at 4 tasklets and +3.5% at 16; a model whose error
grows with the thread count is wrong in a way that a single good number hides.
Two corrections removed the trend, and they had to be made together:

- The memory controller was 15% too fast under contention. See "MRAM under
  contention" in `runner/tests/perf/README.md`.
- Register-file bank conflicts were charged to the wrong instructions and to
  the DPU rather than the thread, so one tasklet never paid them and sixteen
  paid too much. See "Register-file bank conflicts" there.

Either alone makes BIMSA look worse, because the two errors cancelled.

That left a flat +1.5% to +2.1%, which looked like the per-byte MRAM rate: it is
1.6% high because `t_cl` counts whole memory cycles and one step of it is worth
3.3%. A third correction, to which registers the bank rule counts, took 2.7% of
the cycles out and left -0.6% to -1.3%. The DMA refit, which also removed the
1.6%, left -0.5% to +0.4%, the read-write turnaround -0.03% to +0.51%, and
the refresh model +0.30% to +0.58%.

Hardware run-to-run variation is 0.04%, so every number here is real.

    runner/tests/perf/sweep.sh <input.seq> <seq-size> cycles 1 2 4 8 16

The sweep builds both sides from the same `make -n` command, runs each, and
gates every point on `CPU recovery for 0 pairs (0.00%)` and `Results are equal`.

### MRAM latency, where the 5% was

`runner/tests/perf/mram_sweep.sh` isolates it: with one tasklet a DMA blocks the
only thread there is, so a cycle count is the latency itself. The model was
wrong in two opposite directions, which is why the aggregate looked like a
modest 5%: an 8-byte `mram_read` came out 20% fast and a 2 KB one 23% slow.

That first fit read the latency alone and produced constants that suited one
tasklet and nothing else. Fitting the throughput as well, below, left -8.6% to
+5.6% here. **The machine carries about 0.50 cycles per byte at one tasklet and
0.545 at saturation**, so no single rate serves both and the model has one rate.

Three things fixed it, and all three are worth knowing about:

- **The DMA engine's startup was not modelled.** The hardware charges a few
  tens of logic cycles before a transfer's first byte reaches memory, whatever
  its size. `DmaStartupCycles`.
- **`t_cl` was 32 and the machine says about 25.** MRAM has no datasheet to
  read this off, so upstream's value was a guess.
- **`Dma.Cycle` never cycled its queues.** Harmless while every timer was zero;
  the first non-zero startup stalled the DPU forever.

The row buffer is left alone deliberately. Hardware shows no difference between
walking MRAM in order and jumping 64 KB each time, so activation and precharge
are not observable from a DPU.

### MRAM under contention, where the trend was

`runner/tests/perf/mram_tasklet_sweep.sh` runs the same kernel with more
tasklets, so what it reports is the controller's throughput rather than a
latency. The machine saturates at 4 tasklets and then obeys one line exactly:

    cycles per DMA = 38.1 x ceil(bytes / 1024) + 0.545 x bytes

The simulator was giving `12.2 + 0.491 x bytes`: 15% too fast and with no
per-burst term at all, which is -40% at the smallest sizes. Three findings:

- **The fixed 38 cycles is the DMA engine's setup, not a row activation.**
  Pointing every tasklet at the same MRAM row costs exactly what pointing them
  64 KB apart costs. The probe takes a `SPREAD` variable for that test.
- **A transfer larger than 1 KB pays the setup twice.** The engine works in
  bursts of that size: `DmaBurstSize`.
- **The engine does one thing at a time.** The two terms add rather than one
  hiding the other, so the setup of one transfer does not overlap the data of
  the one before it. `Dma` now hands the controller one transfer and waits, and
  the startup counts down only while the controller is idle.

Now inside 2.3% at every tasklet count for 64 bytes and up.

### Register-file bank conflicts, the other half of the trend

The file has an even bank and an odd bank, and each serves at most two of one
instruction's registers in a cycle, counting the destination with the two
sources. Three registers of one parity need a second issue slot.

uPIMulator counted this instruction's sources together with the register the
instruction *before* it wrote, and charged `floor(even/2) + floor(odd/2)`. That
fires on pairs the machine serves for free. Worse, it held the whole DPU's
pipeline instead of the thread's slot, so one tasklet never paid -- the
revolver hid it -- and sixteen paid too much. Measured, the model was 35% fast
on conflict-free code at 16 tasklets and showed no penalty at all at one where
the machine nearly doubles.

`CycleRule.ConflictTurns` now counts the current instruction's own registers,
and `Logic.ServiceThreadScheduler` spends the thread's next issue slot on the
penalty. Nine of eleven measured register layouts agree within 0.3%, at 1
tasklet and 16 alike. `runner/tests/perf/regbank_kernel.c` holds it.

### The linker keeps only the code something reaches

uPIMulator resolved symbols one object file at a time: needing any symbol an
object defines linked every function in it. The SDK compiles one function per
section and the machine's linker drops the ones nobody calls, so a kernel that
fits the machine could still overflow the 4096-instruction IRAM here. BIMSA's
gap-affine kernel is 3795 instructions on the machine and was 6522, so it could
not be run at all.

`logic.SectionPruner` applies the rule `--gc-sections` uses: a section is live
if it is not code, or it is the entry, or a live section names a symbol it
defines. The affine kernel now links at 4062 instructions.

Three things had to be right, and each of them cost a debugging round:

- **`.addrsig_sym` must not count.** That list names every symbol whose address
  is taken anywhere, so counting it keeps everything alive. The machine's
  linker ignores it for this purpose too.
- **A section naming its own symbols says nothing.** Every function has a
  `.Lfunc_end0` and names it in its own `.size` directive. Without resolving a
  reference inside its own section first, the first function to define one is
  kept alive by every other function in the program.
- **A `.stack_sizes` block belongs to the function it describes.** It names
  that function's labels, so it has to go when the function goes, or the link
  fails on a label that is no longer there.

`UPIM_NO_PRUNE=1` turns it off and `UPIM_DUMP_SECTIONS=1` names what went, which
is what to reach for if a program that links on the machine stops linking here.

The check that pruning is safe is that it cannot change a program that already
fitted: PrIM's vector addition gives the same 149936 cycles and 120704
instructions either way.

### Metadata does not belong in MRAM

Found while checking the above. `.stack_sizes` and the debug sections are
metadata; the machine's linker marks them not-allocated and they occupy no
memory there. uPIMulator laid them at the bottom of MRAM, ahead of the
program's own data, which cost BIMSA 104 bytes and put the heap at 1 MB + 104
where the machine puts it at exactly 1 MB.

That is worth more than it sounds. Every buffer the program allocates then
straddles the 1 KB wordlines differently from the machine, and a 16-byte shift
in the heap was measured to move BIMSA's cycle count by 0.32%. They are now
parked one past the end of MRAM, where `Executable.Sort` leaves them out of the
image, and `__sys_used_mram_end` matches the machine's `0x08100000` exactly.

**Check the heap addresses agree before trusting a new workload's numbers.**
`readelf -sW <dpu binary> | grep __sys_used_mram_end` against
`values.txt` in the program directory. They should differ only by
`MramOffset()`, which is 512 KB.

### Two traps when you change the model

- **`script/build.py` does not build the runner.** It builds the standalone
  binary only; `runner/scripts/build.sh` builds `libupimulator.so`, which is
  what `upimulator run` loads. Rebuild both, or you will spend an hour
  measuring code you already replaced.
- **`main.go`'s command-line defaults and `api.Config`'s must agree**, or the
  API test fails comparing one path against the other. That is the test doing
  its job; change both.

### Before trusting a number from it

- **Never sum a counter over tasklets.** One register serves the whole DPU, so
  each tasklet's reading counts every tasklet's work. Proved on hardware in
  `runner/tests/perf/README.md`. BIMSA's host prints the sum, so its
  `[PERF] total` is inflated by the live tasklet count; that cancels in a
  hardware-to-simulator ratio only while both sides have the same number live,
  which the sweep checks.
- **Both sides must run the same binary.** An early comparison here was off by
  25% purely because one build fused a loop counter into a conditional add and
  the other did not. Use `build_bimsa.sh`; do not hand-write the macro list.
- **Do not compare `PERF_S_TOTAL` against `num_instructions`.** The probe covers
  the kernel; `num_instructions` covers the whole launch, 3% more.

### Host transfers are charged now

They used to be free: `api.CopyTo` wrote DPU memory directly and the channel
model was reachable only from the standalone `--benchmark` path, so a transfer
advanced the simulator by zero cycles. The shim now calls
`upim_charge_transfer` once per `dpu_push_xfer` or `dpu_copy_*` -- once per
batch, not per DPU, because the DPUs of a rank move together -- and the cost
lands in a set-wide `transfer_cycle` that `UPIM_STATS` prints.

    cycles = setup + max( bytes / dpu_rate , bytes * dpus / rank_rate )

Calibrated against this machine: 966 MB/s to one DPU, 250 MB/s from it, 9700
and 3400 MB/s for the rank interface, 30 us of setup, and 250 us more on a
set's first transfer to bring the rank up. All seven are settable from the
environment (`UPIM_DPU_WRITE_MBPS`, `UPIM_TRANSFER_SETUP_NS`, ...) so M5 can
vary the link without rebuilding.

On BIMSA the transfers now measure 0.620 ms against the hardware's 0.646 ms,
**-4.0%**. Median error over the 98 measured points of the calibration sweep is
11%.

Two things to know before trusting a transfer number:

- **Calibrate on the median, not the best.** The first fit used best-of-300 and
  under-predicted BIMSA by half. The best is the warm path; a program waits for
  the median.
- **The first transfer costs 250 us**, eight times an ordinary one, and it is
  40% of everything BIMSA spends moving data. It is modelled explicitly.

Where the shape is wrong, both measured: the real per-DPU cost grows with chunk
size between 512 bytes and 4 KB rather than switching cleanly from setup to
rate, and a scatter of large buffers to many DPUs falls below the link because
the host has to gather them. `runner/tests/perf/README.md` has both, with the
residual at every point.

### Sets that span ranks (2026-10-03)

The runner used to put every DPU on one rank. Now the shim lays a set out as
libdpu does: ceil(N / 64) ranks, the DPUs dealt one at a time over the 8
control interfaces of every rank, so 199 DPUs lie 55, 48, 48, 48. This was
measured on the machine for 1 to 2560 DPUs. `dpu_get_nr_ranks`,
`DPU_RANK_FOREACH` and `dpu_alloc_ranks` work, and a copy, a push or a launch
can take one rank. `runner/tests/rank/run.sh` runs the same host on both
sides, and the outputs are identical at 1, 64, 65 and 199 DPUs. The Go
simulator still holds the DPUs as one rank: they share nothing while a
kernel runs. The shim tells the transfer model which ranks a transfer covers.
Kernels do not change: BIMSA at 64, 65, 128 and 256 DPUs (1, 2, 2 and 4
ranks) is -0.25%, -0.32%, -0.28% and -0.23%, with the same cycles per DPU at
every size, and GIMSA on 199 DPUs (4 ranks) gives 2000 of 2000 and the same
phase cycles as before, to the cycle.

A transfer over several ranks costs a setup for each rank after the first
(25 us), and is limited by what the host moves over all of them (11200 MB/s
writing, 4050 MB/s reading):

    cycles = setup + (ranks - 1) * rank_setup
             + max( bytes / dpu_rate , bytes * rank_dpus / rank_rate , bytes * dpus / host_rate )

On 36 measured points over 2 to 16 ranks this is 33% rms (median 24%), against
63% (median 51%) when the set was one rank. One rank costs what it did, to the
cycle. Not modelled: ranks that share a memory channel (two on one channel
are slower than one rank with the same bytes, -40% to -50% at 2 ranks),
broadcasts (charged as a write to every DPU; the machine is 0.4 to 0.8 of
that), and the launch's own host time (37 us on 1 rank to 200 us on 40).
`api/transfer_test.go` holds the 106 measured points and fails if the model
gets worse.

The test also found two shim bugs, both fixed: `dpu_error_to_string`
returned a string literal, which `DPU_ASSERT` frees, so any failing
`DPU_ASSERT` aborted; and `dpu_free` failed on a set the host never loaded.

### Section by section

`runner/tests/perf/tier_sweep.sh` splits a BIMSA run by its own probes. A model
that is right overall for the wrong reasons shows up there as one section much
further out than the total. Gap-affine, one tasklet, 16 pairs:

| section | hardware | uPIMulator | error | calls |
|---|---|---|---|---|
| align | 42757904 | 42506880 | -0.59% | 112 |
| base | 6040752 | 5992608 | -0.80% | 64 |
| queue | 52400 | 52272 | -0.24% | 96 |
| setup | 29296 | 28944 | -1.20% | 32 |
| total | 49180256 | 48875264 | -0.62% | 16 |

Everything inside 1.2%, every call count exact. Gap-affine as a whole is closer
than edit distance (-0.62% against +1.5% to +2.1%), which fits the rest: it
does far more arithmetic per byte moved.

Tier 2 moves the probes inside the aligner, and that is where the split earns
its keep:

| section | hardware | uPIMulator | error | calls |
|---|---|---|---|---|
| compute | 21149968 | 20977376 | -0.82% | 1488 |
| extend | 13825344 | 13385856 | -3.18% | 1712 |
| overlap | 6196384 | 6253248 | +0.92% | 400 |
| rings | 2302000 | 2304544 | +0.11% | 336 |
| total | 53120880 | 52486960 | -1.19% | 16 |

**`extend` is out by 3.18% where the whole kernel is out by 1.19%**, so
something specific to that phase is wrong rather than the model being uniformly
off. It walks two sequences comparing characters, so it reads the most and
computes the least, and it is *fast* here -- which the memory rate does not
explain, because that is 1.6% slow.

**Found 2026-09-26: it was the register file.** With the bank rule corrected,
`extend` is -0.72%, the tier-2 total -0.30%, and every section of both tiers is
inside 0.5%. See "The bank rule, corrected" below. With the DMA engine also
refitted (2026-09-28), `extend` is +0.09%, the tier-2 total -0.17%, and every
section of both tiers is inside 0.44%.

Compare a section against its own tier's total, never across tiers: a tier's
probes cost time and tier 2 has more of them.

**This kernel is why the linker had to learn to drop dead code.** At 4062
instructions it only fits IRAM because of that; before, it needed 6522.

### Three workloads, and what they say together

`runner/tests/prim/run.sh` adds two of PrIM's own kernels, taken unchanged
except for a counter probe. They are chosen to be unlike BIMSA and unlike each
other, and the result localises the one error that is left.

| workload | transfers per 1 KB of work | error over 1-16 tasklets |
|---|---|---|
| `hst`, shared histogram | one read | -0.03% to -0.13% |
| BIMSA, wavefront | short scattered reads and writes | +1.21% to +2.16% |
| `va`, vector addition | two reads and one write | +0.33% to +2.19% |

These are the numbers from before the bank rule was corrected. After it and
the DMA refit, 2026-09-28: `hst` -0.04% to -0.16%, `va` -0.20% to +1.85%,
BIMSA -0.03% to +0.51%.

**The residual tracks how much of the time goes on moving data**, which is what
a per-byte memory rate 1.6% above the machine's predicts. A kernel that barely
touches MRAM is right to a tenth of a percent. Nothing else in the model is off
by enough to see.

Instruction counts are exact at one tasklet on both new kernels.

Two harness rules came out of this, and both cost real time before they were
understood:

- **Close a probe's window with a barrier.** The counter is one register the
  whole DPU shares, so if tasklet 0 stops reading when it finishes its own
  share, the count depends on the order the tasklets finish in. `va` showed
  instruction counts 2.4% apart and cycle errors of 4.7% for that reason alone.
- **Take the median of several hardware runs.** These kernels are short by
  BIMSA's standard and the hardware repeats to only about 1%. One reading made
  `va` at 2 tasklets look 1.78% out when the median says 0.33%.

### GIMSA runs, and computes what the machine computes

The second application. Asian rice, 199 DPUs, 12 tasklets, 2000 queries:
**100% accuracy on both sides, 2000 of 2000, against the project's own
unbounded oracle.**

    cd gimsa && ./bin/gimsa_kernel_lut.exe -r dataset/asian_rice.fa \
        -q <queries> -k 22 -c 16384 -l 16 -d 202 -m 1536 -b 2000 -t 4 -u 8

Three things had to be built first, and each is a commit of its own: the linker
had to drop unreachable code or the kernel would not fit IRAM; `ExecuteSErri`
had to exist or the kernel could not run; and host transfers had to move a row
at a time or a run spent a quarter of an hour before the kernel started.

Four things to know before running it:

- **A query file holds three lines per query, not one.** The second header line
  says how many. Truncating at one line per query leaves the first third intact
  and the rest garbage, which is exactly what it looks like: 666 of 2000
  correct. The host says `PIM accuracy 33.3` and nothing else goes wrong.
- **A truncated query set needs its own ground truth**, or the host skips
  verification and says so in one line that is easy to miss. Slice it out of
  the full file's oracle: 64-byte header, the count is the int64 at offset 24,
  and offset 16 is K.
- **`SUBLUT_BITS` must match the dataset**, which is 8 for asian rice K22. The
  host refuses to start when its own build and `-u` disagree, but nothing
  checks the DPU kernel you linked against it.
- **199 DPUs is the floor** for this dataset; it is partitioned across that many
  and precompute decides it. Expect about 38 GB of host memory.

A run takes about three minutes of wall time for 2000 queries.

### GIMSA's timing, per phase

`gimsa_phases_profile_*_phase_profile_cycles.exe` reports the DPU cycles each
phase costs per query. The same binary, the same queries, on the machine and on
the simulator:

| per query | hardware | uPIMulator | error |
|---|---|---|---|
| `AVG_bs_steps` | 10.613750 | 10.613750 | exact |
| `AVG_consult` (LUT) | 1121.396 | 1124.164 | +0.25% |
| `AVG_bs` (binary search) | 5227.100 | 5039.552 | **-3.59%** |

Both runs verify at 100%, 2000 of 2000.

The step count is exactly equal, so the simulator does the same searching. The
cost of a step is what differs: 187.5 cycles over 10.61 steps, about 17.7
cycles a step, and each step is one 8-byte read at a random place in a 57 MB
table.

**This is the second workload to say the same thing.** BIMSA's `extend` is
3.18% fast and its reads are small; GIMSA's binary search is 3.59% fast and its
reads are small and scattered. Two unrelated kernels, the same sign and nearly
the same size. Raising `dma_startup_cycles` closes `extend` but over-charges
BIMSA's `rings` and `overlap`, which stream, so the missing cost is not a flat
per-transfer charge. A charge that grows when consecutive reads land on
different rows would fit all four measurements; nothing has tested that yet.

**Tested 2026-09-26, and it was not the row.** The machine shows no row cost
for small reads. The cause was the register-bank rule, as for `extend`. With it
corrected, `AVG_bs` is 5170.308 against 5227.100, **-1.09%**; `AVG_consult` is
1123.288, +0.17%; the step count is still exact and both sides still verify
2000 of 2000. The hardware number is the one from 2026-09-22; it was not
measured again. With the DMA engine refitted (2026-09-28), `AVG_bs` is
5259.180, +0.61%, and `AVG_consult` 1108.240, -1.17%. With the read-write
turnaround and the busy rate corrected, `AVG_bs` is 5262.356, +0.67%, and
`AVG_consult` 1111.692, -0.87%; the two together are +0.40%.

**Correction, 2026-09-29: those hardware numbers are from another build.** The
kernel binary the GIMSA host loads (`src/UPMEM-partitioner/bin/
gimsa_kernel_lut_profile_cycles`, built 2026-06-26) is not what the Makefile
builds today, and the simulator's image was compiled from today's source.
`main_kernel1` is 594 instructions in one and 564 in the other. Every GIMSA
timing comparison above compares two builds. Built alike (the Makefile's
command, `SUBLUT_BITS=8`; the hardware run pointed at it with
`runner/scripts/dpuload_override.c`), three hardware runs against the current
model: `AVG_consult` 1114.9 median against 1117.5, **+0.24%**; `AVG_bs` 5225.9
against 5277.5, **+0.99%**; together **+0.85%**. See "The same code on both
sides" in the perf README.

    # both sides, once the query file and its oracle are in place
    UPIM_STATS=<file> runner/upimulator run --program-dir <bin> \
        --tasklets 12 --dpus 199 -- \
        ./bin/gimsa_phases_profile_kernel_lut_phase_profile_cycles.exe \
        -r dataset/asian_rice.fa -q <queries> -k 22 -c 16384 -l 16 \
        -d 202 -m 1536 -b 2000 -t 4 -u 8

### The linker keeps only what the program reaches, data as well as code

The pruner used to treat every section that is not code as a root. A `.rodata`
jump table is not code, so it kept its own function alive, and that function
kept everything it calls alive. `printf` survived in GIMSA, which never calls
it, and with `printf` came the SDK's 1 MB stdout buffer in `.mram.noinit`.

The machine's linker drops both. GIMSA's own kernel binary has
`.mram.noinit = 0x408000` and the simulator had `0x508000`: **the MRAM heap
started 1 MB higher than on the machine, for every program that does not use
`printf`.** For GIMSA's profiling build that 1 MB was the difference between
fitting MRAM and reading past its end.

Now both agree exactly. BIMSA does call `printf`, so it keeps the buffer on
both sides -- but it also had 208 bytes of its own unreachable `.mram.noinit`,
which the machine drops and this used to keep. Its heap now starts on the
1 MB boundary the machine puts it on.

Two things this brought out, both worth keeping in mind:

- **A data section belongs to one object file.** `.mram.noinit` is one output
  section built from a piece of every object that puts something there. Naming
  the pieces alike makes one live piece keep every other alive, and nothing is
  ever dropped.
- **The stacks were never aligned.** `AssignWram` placed `.data.stacks` wherever
  the section in front of it ended. Nothing had noticed, because the sections in
  front happened to add up to a multiple of 8 -- until 188 bytes of dropped jump
  table moved the stacks onto a 4-byte boundary. A DMA moves 8 bytes at a time,
  so the last word of every transfer out of a local array was lost.
  `runner/tests/alu` caught it in one result out of 1224. The machine pads
  `.data.stacks` for the same reason; it does so now too.

`.data` and `.bss` are still roots. The machine drops those as well, and the
simulator carries about 530 bytes of WRAM that the machine does not, but
nothing measured depends on it and a data object that only the host reads would
go with it.

### The bank rule, corrected

Found 2026-09-26 while testing the row hypothesis for small scattered reads.

**The row hypothesis is wrong.** `runner/tests/perf/scatter_sweep.sh` reads 8,
16 and 64 bytes in order, 64 KB apart, and at random addresses in 8 MB and
32 MB, from 1 to 16 tasklets. On the machine the pattern makes no difference
to an 8 or 16-byte read. The simulator is the one that charges a row miss,
about 11 cycles at one tasklet. That charge is too large, but it makes the
model slower, not faster, and no kernel measured depends on it.

**The error was in the loop around the read.** The same loop without the read
matched the machine at one tasklet and was one cycle per iteration fast at 12
and 16. `runner/tests/perf/pipe_sweep.sh` bisected it to one pair: the loop
counter's `add r4, r4, 1` in front of `add r2, r2, r20`.

**The machine's rule.** Each bank of the register file has two ports. They are
shared between the registers an instruction reads and the register the same
thread's previous instruction writes. The instruction's own destination does
not count. Three distinct registers of one parity, counted that way, cost the
thread one issue slot. The model counted the instruction's own destination
instead. That got three measured layouts wrong and missed the most common
conflict in compiled code, a loop counter update in front of a two-source
instruction. Upstream counted the right registers; the first correction, which
fixed the charge, also changed the registers, and that part was wrong.

`CycleRule.ConflictTurns` now keeps each thread's previous instruction. All 35
register cases in `regbank_sweep.sh` and `pipe_sweep.sh` agree within 0.34%,
at 1 and 16 tasklets. The five discriminating pairs are in the perf README.

Effect, hardware against simulator:

| workload | before | after |
|---|---|---|
| BIMSA edit distance, 1-16 tasklets | +1.48% to +2.12% | -0.56% to -1.30% |
| BIMSA gap-affine, `extend` | -3.18% | -0.72% |
| BIMSA gap-affine, tier-2 total | -1.19% | -0.30% |
| GIMSA binary search | -3.59% | -1.09% |
| GIMSA table lookup | +0.25% | +0.17% |
| PrIM `va`, 1-16 tasklets | +0.33% to +2.19% | +0.25% to +2.10% |
| PrIM `hst`, 1-16 tasklets | -0.03% to -0.13% | -0.01% to -0.18% |

The ALU suite still passes, 1224 of 1224.

### The DMA engine, refitted

2026-09-28. `runner/tests/perf/scatter_sweep.sh` separates an idle engine
(1 tasklet) from a busy one (16 tasklets) and small reads from large ones, in
order and at random addresses. Four findings, all in the perf README under
"Small reads, and the DMA engine idle and busy":

- **No row cost between transfers.** The simulator charged about 11 cycles for
  a read that opened a different row; the machine charges nothing. `t_rcd`,
  `t_ras` and `t_rp` are now 0.
- **The setup is paid once for every 1 KB row a transfer touches**, not once
  per 1 KB of size. A random 1 KB read usually touches two rows and costs 38
  cycles more than an aligned one; the measurement fits that to 0.2 cycles at
  every size. `Dma.Push` counts rows now.
- **An idle engine and a busy one cost different amounts.** The machine wakes
  the thread before the engine has finished: a thread sees about 0.50 cycles a
  byte, and a saturated engine costs 0.545. A read also costs the thread one
  more issue slot when it completes, and a write costs neither that slot nor
  as much setup. The model has four new constants for this: a tail after each
  transfer (2 cycles plus 43 per KB), a resume slot for reads, and a separate
  setup (25) and acknowledgement delay (9) for writes. `t_cl` went from 25 to
  23 and the read setup from 26 to 34. The per-KB tail was 43 at first and
  is 48 now, which fits saturated reads of 128 B and more to 0.2%.
- **Changing direction costs the engine about 2.4 cycles.** A read and a
  write together cost more than each alone; reads alone and writes alone do
  not. `dma_turnaround_cycles` = 2. Found from GIMSA's table lookup; measured
  by `PROBE=mix runner/tests/perf/scatter_sweep.sh`.
- **MRAM refresh is visible**: every 1367 cycles, for about 55 to 60. It is not
  modelled. Its average is in the fitted constants.

Effect, hardware against simulator:

| workload | before the refit | after |
|---|---|---|
| BIMSA edit distance, 1-16 tasklets | -0.56% to -1.30% | -0.03% to +0.51% |
| BIMSA gap-affine, every section | inside 0.5% | inside 0.44% |
| GIMSA binary search | -1.09% | +0.67% |
| GIMSA table lookup | +0.17% | -0.87% |
| GIMSA both phases | -0.86% | +0.40% |
| PrIM `va`, 1-16 tasklets | +0.25% to +2.10% | -0.20% to +1.85% |
| PrIM `hst`, 1-16 tasklets | -0.01% to -0.18% | -0.04% to -0.16% |
| MRAM reads, 2-16 tasklets | +1.6% flat | inside 0.23% |
| MRAM writes, 2-16 tasklets | not measured | inside 0.71%, and -1.85% at 64 B on 2 tasklets |
| a read and a write together, rms | 2.62% | 1.59% |
| small reads, 1 tasklet | -6% to -9% | inside 2.6% |

The ALU suite still passes, 1224 of 1224, and the 35 register cases are still
inside 0.41%.

### MRAM refresh, and convoys

2026-09-28, from PrIM `va`. On larger inputs `va` was +2.4% to +4.4% slow at 4
and 8 tasklets. The simulator's own trace showed two threads locked in a
convoy: one's read always arrived between the other's read and write, so both
waited a whole transfer in every iteration, forever.
`runner/tests/perf/trace_kernel.c` stamps every transfer on the machine too.
The machine forms the same convoys and breaks them within 10 to 20 iterations.

The disturbance that breaks them is MRAM refresh, measured with the same probe
at one tasklet: every **2736 cycles (7.8 us)** the memory is unavailable for
about **120 cycles**. It is now modelled (`mram_refresh_interval`,
`mram_refresh_cycles`; `Dpu.Cycle` holds the memory controller), and the DMA
constants were refitted with it on: `t_cl` 22, read setup 32, tail 45 per KB,
write setup 24.

| workload | without refresh | with refresh |
|---|---|---|
| PrIM `va` 262144, 4 / 8 / 16 tasklets | +2.39% / +4.35% / -0.05% | -2.24% / -0.09% / -0.11% |
| PrIM `va` 65536, 4 / 8 / 16 | +3.16% / +0.69% / +0.62% | -0.08% / -0.23% / +0.38% |
| PrIM `va` 16384, 1 / 2 / 4 / 8 / 16 | -0.20% / -0.03% / +0.07% / +0.83% / +1.85% | -0.10% / -3.40% / +0.09% / +1.23% / +2.17% |
| BIMSA, 1-16 tasklets | -0.03% to +0.51% | +0.30% to +0.58% |
| GIMSA lookup / search / both | -0.87% / +0.67% / +0.40% | -1.32% / +0.96% / +0.56% |
| read sweep rms, 108 points | 1.18% | 0.97% |
| mixed read and write rms | 1.59% | 1.96% |

Mean error over the seven workload groups goes from 0.81% to 0.63%, and the
worst point from 4.35% to 3.40%. The model without refresh is one option
setting away (`mram_refresh_interval=0` with `t_cl` 23, read setup 34, tail 48
per KB, write setup 25).

**Refresh does not settle every convoy.** At 2 tasklets on 16384 elements the
machine is bistable: 13 of 20 runs keep the two threads in phase (729k), the
rest drift apart (701.6k to 726.5k). The model always drifts apart (704k), so
`va` reads -3.4% against the median and 0.2% below the hardware minimum. A
copy of `va` with stamps added lands both sides in phase, to 0.01%. The
mechanisms agree; which state a run falls into depends on a few cycles.
`mram_refresh_mode=1` (refresh between transfers) keeps the in-phase state but
also the convoys the machine breaks elsewhere, so it is an option, not the
default. `runner/tests/prim/run.sh` now prints the hardware minimum and maximum
beside the median, so a bistable case shows.

### The same code on both sides

2026-09-29, from PrIM `va` at 16 tasklets. Two findings, both about what the
two sides run rather than how the model times it:

- **The runtime differs.** The machine links SDK 2025.1's prebuilt runtime;
  the simulator compiles its bundled 2023 sources. `runner/scripts/rtdiff.py`
  compares every function's length between a hardware ELF and a linker run's
  `main.S`. `mem_alloc_nolock` (one instruction more on the machine, every
  call) and `perfcounter_config` (three more) now match; `__muldf3` differs in
  block layout and cannot be matched from source. A `mem_alloc` loop at one
  tasklet is now exact (14176 cycles both), where it was 5% fast.
- **GIMSA's two sides ran different builds** of the same source (see the
  correction under "GIMSA's timing, per phase"). Like for like, the phases are
  +0.24% and +0.99%.

What is left of `va` at 16 tasklets (+1.5%, above the machine's maximum) is in
the start: the machine grants a contended lock, and wakes a barrier's waiters,
in an order that is not the tasklets' number; the simulator's scheduler goes
round by number. A late-release model was tried and did not reproduce it.
**Done 2026-09-30, see "The order the threads issue in".**

**Done 2026-09-30, see "Running the machine's own ELF":** load the hardware's
own DPU ELF into the simulator instead of linking assembly. The simulator uses
its own instruction encoding, so this needs a decoder from UPMEM's 64-bit
instruction words to the simulator's instruction objects (or a translation of
`llvm-objdump` output into the linker's syntax). It would also remove the
runner's need for a separate linker run built from the same source.

### Running the machine's own ELF

2026-09-30. The runner's default is now the DPU ELF the host passes to
`dpu_load`, the same file the machine runs; `--program-dir` still selects a
linker image. This removes both mismatches above: the runtime is the SDK's,
and the build is the host's.

- `golang/uPIMulator/src/elfload` reads the ELF (Go's `debug/elf`), decodes
  `.text` with `/usr/bin/llvm-objdump` (`UPIM_OBJDUMP` overrides it), turns
  branch targets and hexadecimal operands into numbers, and reads the lines
  with the linker's own grammar and `InstructionAssigner`. Data sections go to
  their addresses; symbols become both symbol tables.
- `misc.UseHardwareMemoryMap(true)` switches every `ConfigLoader` to UPMEM's
  map: IRAM from 0 with one address per instruction (the machine's jump tables
  and `call` hold instruction indices), WRAM 64 KB from 0, MRAM tagged
  0x08000000. A DMA takes its WRAM address modulo 64 KB and its MRAM address
  modulo 64 MB, because the code loads MRAM addresses without the tag. An
  atomic bit number wraps at 256, because the SDK's boot code releases bit 256.
- The shim builds the set in `dpu_load` when `UPIM_PROGRAM_DIR` is unset, and
  reads `DPU_LOAD_OVERRIDE`. One process holds one kind of program.

Checked: ALU 1224 of 1224; register probe inside 0.28%; MRAM latency as the
linker path; BIMSA +0.10% to +0.70%; PrIM as the linker path; isolation and
`--trace-hardware` clean. GIMSA like for like at last: lookup 0.00% and
search +1.10% with the Makefile's build, -0.73% and +1.05% with the host's
June binary. Details in the perf README, "The same code on both sides".

This also closes the old "WRAM is not modelled faithfully" gap for ELF runs:
WRAM is 64 KB from 0 and the stacks are the ELF's own.

### The order the threads issue in

2026-09-30. A contended lock goes to the first thread that issues its acquire
after the release, so the order a lock is granted in is the scheduler's order.
Upstream uPIMulator goes round the threads by number; the machine does not.
`runner/tests/perf/sched_sweep.sh` reads the machine's order through two
probes, `ring_kernel.c` (the rotation the boot chain leaves) and
`grant_kernel.c` (the rotation after a barrier, and the order the barrier's
own bit went round). The orders are the same on every hardware run. Three
rules give all 42 of them, 3 to 16 tasklets, and are now the default
(`thread_policy=1`):

- A booted or resumed thread issues 13 cycles after the boot or resume
  (`boot_delay_cycles`, `resume_delay_cycles`). Tasklet k boots k+1 with its
  third instruction, so k+1 starts 35 cycles after k, two cycles later in the
  11-cycle revolver: after boot, 8 tasklets go round as 0 6 1 7 2 3 4 5.
- A woken thread, one that a boot or a resume made runnable and that has not
  issued since, goes ahead of every thread waiting for its revolver turn. So
  the waiters a barrier resumes as 1, 2, 3 go round as 3 2 1.
- Otherwise the thread ready the longest goes first, ties to the lowest
  number. A thread whose transfer completes is not woken in this sense; in
  PrIM `va` and `hst` the choice makes no difference.

The strongest check is the instruction count: PrIM `va` and `hst` now
execute exactly the machine's instructions at every tasklet count (`va` was
0.57% short at 16, from fewer spins on `mem_alloc`'s lock). Cycles, the same
build with `thread_policy` 0 and 1:

| | old order | machine's order |
|---|---|---|
| `va`, 16384 elements, 16 tasklets | +1.88% | -0.24% to 0.00%, inside the machine's range |
| `va`, 16384 elements, 8 tasklets | +0.98% | -1.1% to -1.5%, below the machine's range |
| `va`, 65536 elements, 8 / 16 tasklets | 0.00% / +0.61% | +0.60% / -0.15% |
| BIMSA edit distance, 1-16 tasklets | +0.13% to +0.69% | -0.43% to +0.41% |
| small reads, rms at 1 / 4 / 16 tasklets | 0.79 / 0.94 / 0.81% | 0.78 / 0.92 / 0.71% |
| a read and a write, rms at 4 tasklets | 2.22% | 3.31% (one point, 1 KB with gap 32: -0.04% to -9.54%) |
| GIMSA lookup / search, the Makefile's build | 0.00% / +1.10% | -0.27% / +1.12% |

Not kept: an acquire that tests its bit a few cycles after it issues. It gives
the machine's order at 11 tasklets or more only. The details are in the perf
README, "The same code on both sides".

### A read's latency, and refresh lock-in

2026-10-02, from GIMSA's binary search, +1.1% on the same code. Details and
tables in the perf README under the same heading.

- **GIMSA one DPU at a time.** `UPIM_RECORD=<file with %u>` with
  `UPIM_RECORD_DPU=<list>` makes the shim record every copy to a DPU, every
  copy back, and every launch; `runner/scripts/replay_host.c` replays that on
  one DPU, on the machine or in the simulator. `runner/tests/perf/
  gimsa_replay.sh` prints the phases of each replayed DPU. Six DPUs showed
  the search +0.3% to +1.0% slow on every one, with 1 or 2 queries per
  tasklet per launch.
- **The resume slot was a stand-in.** At one tasklet the machine's read loops
  lock to the refresh period: their cycles sit on 2736 / k for a range of
  loop lengths (`stair_kernel.c`). The slot (`dma_resume_turns=1`) fitted
  because the scatter loops it was fitted on sit on such a step. The idle
  read latency (`refresh_kernel.c`, exact to about a cycle) is 242.5 +
  0.4985 x size; the slot model was 6 cycles high on small reads and 2% low on
  1 KB.
- **The new model:** `dma_resume_turns=0`, a read acknowledgement of 9 cycles
  (`dma_read_ack_cycles`, the engine free meanwhile, as for writes), `t_cl`
  23, `dma_tail_cycles_per_kb` 25. Fitted on every stored point at once with
  a coordinate search: scatter and mix sweeps, the staircase and six GIMSA
  replays.

| | old model | new model |
|---|---|---|
| GIMSA lookup / search, 199 DPUs | -0.27% / +1.12% | +0.49% / +0.57% |
| BIMSA edit distance, 1-16 tasklets | -0.43% to +0.41% | -0.74% to +0.09% |
| PrIM `va` 16384 / `hst`, 1-16 tasklets | -1.47% to -0.12% / 0.15% | -0.61% to +0.68% / 0.11% |
| small reads and the mix probe, rms | 1.52% | 1.40% |
| one-tasklet MRAM loops | -7.1% to +4.1% | -4.0% to +4.3% |

An acknowledgement of 7 fits the idle reads best and gives GIMSA +0.15% /
+0.22%, but BIMSA -1.13% at 8 tasklets. So a thread under contention pays
about two cycles more per read than alone, and the model does not have that
yet.

Also found: the machine's refresh schedule is not a fixed clock. The reads
it delays come every 2736 cycles, but in some runs the schedule moves by
about half a period and back; the address does not pick which. And the
rotation after boot at 11 tasklets came out in another order in one run
(thread 1 last), so that order is not always the same on the machine.

### Accepted accuracy limits

Decided 2026-10-02: the remaining timing errors are accepted, not chased.
They change cycle counts only, never outputs. Every validation run had the
right outputs; an output can differ only if a program depends on timing
(prints cycle counts, races between tasklets) or reads MRAM it never wrote
(zeros in the simulator, old data on the machine).

Changed 2026-10-07: **the target is 1%, and a workload is valid up to 3%.**
Inside 1% is on target. From 1% to 3% is valid but off target, and stays
open work. Above 3% is not valid.

- The validated workloads: inside 1% (GIMSA at 2000 reads +0.49% / +0.57%,
  BIMSA -0.74% to +0.09%, PrIM `va`/`hst` -0.61% to +0.68%; the other eleven
  PrIM kernels -0.62% to +1.08%, one point above 1%).
- GIMSA at 50,000 reads, six read sets (`runner/tests/perf/gimsa_reference.sh`,
  2026-10-06): -1.16% to -2.09%. Valid, off target.
- Isolated probe points: inside about ±4.7%. An exception: not a workload.
- Bistable cases: +3% to +4% against the machine's median; compare them
  against its min..max.

The causes, measured and not modelled: a read costs a thread about two
cycles more under contention than alone (the model uses the contended value,
`dma_read_ack_cycles` 9); the machine's refresh schedule moves by about half
a period at times (one-tasklet loops locked on one side only are off by up to
about 4%); bistable states (`va` at 2, 4 and 8 tasklets, the mix probe at 4
tasklets with 1 KB transfers); small reads with 2 to 4 tasklets queueing, up
to 4% fast. `tasks_left.md` holds the same list.

### What is left

1. Transfers as simulated traffic, when a study needs them to overlap a
   kernel. A new workload, when a study brings one: run it the way
   `runner/tests/prim/run_prim.sh` runs PrIM.
2. If a study moves much data over many ranks: ranks that share a memory
   channel, broadcasts and the launch's host time (see "Sets that span
   ranks"). None is modelled.

`PERF_TIER=1` needs a gap-affine build, not an edit-distance one: every tier-1
and tier-2 probe sits in the gap-affine path, so edit distance reports no calls
for any of them and 99.6% of the time in `other`.
`runner/tests/perf/tier_sweep.sh` builds it that way, and every section comes
out inside 0.7% with the call counts exact (1.2% before the bank rule was
corrected).

## Safety: never touch the hardware from a simulated run

This server has 40 real DPU ranks, the account is in `plugdev`, and **libdpu
selects hardware by default** when no `backend=` key is given. The reassuring
"Fallback to SIMULATOR" message is unreachable here, so its absence proves
nothing. Profile precedence, verified by disassembly:

    UPMEM_PROFILE  >  the profile string passed to dpu_alloc()  >  UPMEM_PROFILE_BASE

`source upmem_env.sh simulator` sets only the weakest slot. Do not rely on it.

The runner applies four layers (poisoned profile, hidden hw backend, symbol
coverage check, strace). `runner/scripts/test_isolation.sh` proves they hold;
run it after any change to the runner. Reference hardware runs for the study are
the only commands allowed to ask for `backend=hw`, and they must be deliberate.

## Environment

- Go is at `~/go-sdk` (user-local, not on PATH): `export PATH=/home/alex/go-sdk/bin:$PATH`
- UPMEM SDK 2025.1.0, system-wide. No Docker, no ninja; cmake and make are present.
- 93 GB RAM, 40 cores.

## Commands

    # simulator binary AND the runner's library. Both, every time: build.py
    # does not touch libupimulator.so and `upimulator run` loads nothing else.
    export PATH=/home/alex/go-sdk/bin:$PATH
    python3 uPIMulator/golang/uPIMulator/script/build.py
    uPIMulator/runner/scripts/build.sh

    # API tests (need a completed run's bin dir)
    cd uPIMulator/golang/uPIMulator/src
    UPIM_TEST_BINDIR=/tmp/binS UPIM_TEST_TASKLETS=16 go test ./api/ -v

    # isolation proof
    uPIMulator/runner/scripts/test_isolation.sh

    # ALU / condition-code / DMA conformance against UPMEM's own simulator
    uPIMulator/runner/tests/alu/run.sh

    # PrIM kernels on hardware vs the simulator (asks for backend=hw)
    uPIMulator/runner/tests/prim/run.sh

    # BIMSA section by section, gap-affine (asks for backend=hw)
    uPIMulator/runner/tests/perf/tier_sweep.sh

    # MRAM latency at one tasklet, hardware vs the simulator
    uPIMulator/runner/tests/perf/mram_sweep.sh

    # MRAM throughput under tasklet contention
    uPIMulator/runner/tests/perf/mram_tasklet_sweep.sh

    # register-file bank conflicts
    uPIMulator/runner/tests/perf/regbank_sweep.sh

    # the order the threads issue in, read through contended locks
    uPIMulator/runner/tests/perf/sched_sweep.sh

    # a read's idle latency, and how one-tasklet loops lock to refresh
    uPIMulator/runner/tests/perf/latency_sweep.sh

    # GIMSA's phases one DPU at a time, from records (see UPIM_RECORD below)
    uPIMulator/runner/tests/perf/gimsa_replay.sh <kernel ELF> /tmp/rec/dpu_*.rec

    # M4: BIMSA on hardware vs the simulator (asks for backend=hw deliberately)
    uPIMulator/runner/tests/perf/sweep.sh <input.seq> <seq-size> cycles 1 2 4 8 16
    uPIMulator/runner/tests/perf/sweep_dpus.sh <pair.seq> <seq-size> 4 4 cycles 1 2 4 8 16

    # recalibrate the transfer model against hardware (medians, cold start apart)
    XPROFILE=backend=hw UPMEM_PROFILE=backend=hw \
        uPIMulator/runner/tests/perf/xfer_host > sweep.tsv
    # the same over 1 to 16 ranks, with the layout and the launch time
    XPROFILE=backend=hw UPMEM_PROFILE=backend=hw XBIN=/tmp/xfer_dpu \
        uPIMulator/runner/tests/perf/rank_host > ranks.tsv
    # PrIM's other eleven kernels with PrIM's own hosts (asks for backend=hw)
    uPIMulator/runner/tests/prim/run_prim.sh [BS GEMV HST-L ...]
    # sets that span ranks: layout, rank-wide copy, push and launch, both sides
    uPIMulator/runner/tests/rank/run.sh 199 65 64 1

    # build BIMSA's kernel for uPIMulator from make's own command
    uPIMulator/runner/tests/perf/build_bimsa.sh <out> NR_TASKLETS=N PERF=1 ...

    # double-precision regression
    uPIMulator/runner/tests/fp/README.md has the commands

    # first line the linker's grammar rejects in a .S file
    cd uPIMulator/golang/uPIMulator/src && go run ./cmd/parsecheck <file.S>

    # link an arbitrary kernel into a program directory
    ./build/uPIMulator --root_dirpath $PWD --bin_dirpath <out> \
        --benchmark <NAME> --benchmark_asm <kernel>.c.o --link_only true \
        --num_tasklets N --num_channels 1 --num_ranks_per_channel 1 --num_dpus_per_rank 1

    # run a compiled UPMEM host binary on the simulator
    uPIMulator/runner/upimulator run --program-dir <out> --tasklets N --dpus M \
        [--verbose] [--trace-hardware] -- ./host args
    # UPIM_STATS=<file> writes counters; UPIM_STATS=- prints them
    # UPIM_DUMP_LOG=1 prints the DPU's own log
    # UPIM_TRACE=1 traces one instruction per cycle, 2 adds the register file.
    # This is the simulator's own --verbose. --verbose / UPIM_VERBOSE is the
    # shim's trace of intercepted calls, which is a different thing.

    # record one DPU's work in a many-DPU run, then replay it on one DPU,
    # on the machine and in the simulator (see runner/scripts/replay_host.c)
    UPIM_RECORD=/tmp/rec/dpu_%u.rec UPIM_RECORD_DPU=0,40,80 \
        uPIMulator/runner/upimulator run --tasklets N --dpus M -- ./host args
    gcc -O2 -o replay_host uPIMulator/runner/scripts/replay_host.c $(dpu-pkg-config --cflags --libs dpu)
    XPROFILE=backend=hw UPMEM_PROFILE=backend=hw ./replay_host <ELF> /tmp/rec/dpu_0.rec hw.out
    uPIMulator/runner/upimulator run --tasklets N --dpus 1 -- ./replay_host <ELF> /tmp/rec/dpu_0.rec sim.out

BIMSA's kernel flags come from `make -n` in `bimsa/upmem`; add `-S -g0
-DNR_THREADS=24` and point the includes at uPIMulator's bundled SDK
(`sdk/misc`, `sdk/stdlib`, `sdk/syslib`).

## Things already established -- do not re-derive

- BIMSA's and GIMSA's kernels compile clean against uPIMulator's bundled headers
  and use only supported mnemonics. BIMSA is 3462 instructions, inside the 4096
  IRAM limit that hardware also imposes. One directive has to be stripped first;
  see `.stack_sizes` below.
- `DPU_MRAM_HEAP_POINTER_NAME` expands to `"__sys_used_mram_end"`, which the
  linker already publishes, so bulk transfers resolve through the ordinary
  constant table.
- `dpu_probe_*` does not exist in SDK 2025.1.0 at all. BIMSA guards it behind
  `ENERGY`, which is never enabled.
- Simulation speed is about 33K -> 75K cycles/second for one DPU. A 100 ms
  hardware kernel is ~35M cycles, so budget accordingly and keep runs small.
- `-g0` is mandatory when compiling any kernel for uPIMulator: DWARF sections
  carry hex literals with letter digits, which the linker's lexer cannot
  tokenize.
- `-DNR_THREADS=24` is mandatory for the bundled SDK build.
- The linker's grammar rejects `.section .stack_sizes`, which clang emits for
  larger kernels and offers no flag to suppress. Strip it before linking:
  `sed -i '/\.section\t\.stack_sizes/,+2d' <file>.c.o`. BIMSA needs this.
- The UPMEM functional simulator (`backend=simulator`) is the reference for ISA
  behaviour the documentation does not state. It links only against libc and
  cannot reach a rank, so it is safe to run here, and it is what every
  conformance test in `runner/tests/alu` diffs against.

## Fixes already made to upstream code

- Eleven functional bugs in the ALU, the condition codes and the DMA. See the
  table above and `runner/tests/alu/README.md`.
- `time` and `time_cfg` now model the performance counter: one register per DPU,
  advancing once per 16 events as the hardware's does. Without them no
  instrumented kernel could run at all, so there was no way to measure the same
  span on both sides.
- `CLOCKS_PER_SEC` is filled in when a program is installed. On hardware libdpu
  writes the rank's frequency there at load; nothing wrote it here, so a kernel
  converting cycles to a duration divided by zero.
- `cshared` reads `UPIM_TRACE` so a library run can produce an instruction
  trace. Without it the runner could only trace intercepted calls, not the
  kernel, and nothing inside a DPU was observable.
- `ExecuteRrr`'s 40-way op-code dispatch moved to `AluRrr`, shared with the new
  `ExecuteSRrr`, so the two cannot drift.
- `EncodeSRrr` tested `suffix != RRR` when the dispatcher only routes
  `S_RRR`/`U_RRR` there, so it could never succeed; `DecodeSRrr` rejected the
  unsigned variant. Audited every Encode/Decode pair: only these two were wrong.
- `CommandLineParser.Parse` segfaulted on an unknown or valueless `--option`.
- `api.Set.Free` crashed on a set that was never launched.
- The whole SDK 2025.1.0 port (Docker removed, `-g0`, `-DNR_THREADS=24`,
  `OBJECT` libraries, `.c.o` filter in the linker). See git history.
