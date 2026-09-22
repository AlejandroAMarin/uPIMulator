# Handoff: hardware-vs-simulator validation study

State as of 2026-09-17. Read this before touching anything.

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
| M4 validation study | **Validated on three workloads.** Instructions match hardware exactly; kernel cycles inside 2.2% over 1-16 tasklets on BIMSA and PrIM's vector addition, and inside 0.15% on PrIM's histogram; transfers inside 4% |
| M5 architecture exploration | Not started |

## The former blocker: eleven functional bugs, all fixed

Double-precision arithmetic was wrong, which made BIMSA's own MRAM guard reject
its workload. Chasing it found ten more defects in the same layers. All are
fixed, and `runner/tests/alu/run.sh` holds them fixed by diffing 1176 results
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
| 1 | 7503040 | 7660720 | +2.10% |
| 2 | 7556384 | 7719440 | +2.16% |
| 4 | 7777440 | 7894560 | +1.51% |
| 8 | 8408720 | 8510288 | +1.21% |
| 16 | 8449344 | 8563696 | +1.35% |

Instruction counts match exactly at 1 tasklet, +0.03% at 4, +0.12% at 16.

Sweeping DPUs instead, with the work on each held fixed at 4 pairs and 4
tasklets:

| DPUs | cycles, hardware | cycles, uPIMulator | error |
|---|---|---|---|
| 1 | 1896656 | 1931712 | +1.85% |
| 2 | 3895792 | 3960496 | +1.66% |
| 4 | 7813408 | 7920992 | +1.38% |
| 8 | 15601984 | 15841984 | +1.54% |
| 16 | 31240480 | 31683968 | +1.42% |

The error does not grow with the DPU count. uPIMulator's per-DPU figure is
identical to the digit from 2 DPUs up and the hardware's is constant to 0.2%,
so DPUs are independent during a kernel on both and the model composes.

**The timing model is trustworthy to about 2% over the whole range, and the
error no longer depends on the tasklet count.** That last part is the point.
The first fit gave -0.9% at 4 tasklets and +3.5% at 16; a model whose error
grows with the thread count is wrong in a way that a single good number hides.
Two corrections removed the trend, and they had to be made together:

- The memory controller was 15% too fast under contention. See "MRAM under
  contention" in `runner/tests/perf/README.md`.
- Register-file bank conflicts were charged to the wrong instructions and to
  the DPU rather than the thread, so one tasklet never paid them and sixteen
  paid too much. See "Register-file bank conflicts" there.

Either alone makes BIMSA look worse, because the two errors cancelled.

What is left is a flat +1.2% to +2.2%, and the MRAM measurement accounts for
it: the per-byte rate is 1.6% high because `t_cl` counts whole memory cycles
and one step of it is worth 3.3%.

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

### Three workloads, and what they say together

`runner/tests/prim/run.sh` adds two of PrIM's own kernels, taken unchanged
except for a counter probe. They are chosen to be unlike BIMSA and unlike each
other, and the result localises the one error that is left.

| workload | transfers per 1 KB of work | error over 1-16 tasklets |
|---|---|---|
| `hst`, shared histogram | one read | -0.03% to -0.13% |
| BIMSA, wavefront | short scattered reads and writes | +1.21% to +2.16% |
| `va`, vector addition | two reads and one write | +0.33% to +2.19% |

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

### What is left

1. More workloads still. Three is better than one, but `hst` and `va` are both
   block-streaming kernels over one DPU; a kernel with an irregular access
   pattern, or one that spans ranks, would test something these do not.
2. More than one rank or channel. The runner puts every DPU on one of each, so
   that topology is still untested.
3. The flat +1.6% left in MRAM throughput. `t_cl` counts whole memory cycles
   and one step is worth 3.3%, so the model cannot express the machine's rate
   exactly. Do not fix it by moving `memory_frequency`, which is a documented
   number, unless something else justifies the change.
4. Two register layouts the bank-conflict rule still gets wrong, each by one
   issue slot per loop iteration, at the boundary between one instruction and
   the next. A rule over pairs of instructions might catch them; none has been
   measured.
5. Transfers as simulated traffic rather than an analytic charge. Only needed
   for a study where transfers overlap a kernel; they do not here.
6. GIMSA.

`PERF_TIER=1` is a dead end for this workload: with edit distance its `align`,
`base` and `queue` probes record no calls and 99.6% of the time lands in
`other`. A gap-affine input would be needed.

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

    # MRAM latency at one tasklet, hardware vs the simulator
    uPIMulator/runner/tests/perf/mram_sweep.sh

    # MRAM throughput under tasklet contention
    uPIMulator/runner/tests/perf/mram_tasklet_sweep.sh

    # register-file bank conflicts
    uPIMulator/runner/tests/perf/regbank_sweep.sh

    # M4: BIMSA on hardware vs the simulator (asks for backend=hw deliberately)
    uPIMulator/runner/tests/perf/sweep.sh <input.seq> <seq-size> cycles 1 2 4 8 16
    uPIMulator/runner/tests/perf/sweep_dpus.sh <pair.seq> <seq-size> 4 4 cycles 1 2 4 8 16

    # recalibrate the transfer model against hardware (medians, cold start apart)
    XPROFILE=backend=hw UPMEM_PROFILE=backend=hw \
        uPIMulator/runner/tests/perf/xfer_host > sweep.tsv

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
