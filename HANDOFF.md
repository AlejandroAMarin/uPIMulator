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
| M4 validation study | **Kernel time validated.** Instruction counts match hardware exactly; cycle counts are inside ±5.5% over 1-16 tasklets and 1-16 DPUs. The channel model is untested and unused -- see below |
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
| 1 | 7502864 | 7138112 | -4.86% |
| 2 | 7577552 | 7203856 | -4.93% |
| 4 | 7761952 | 7351264 | -5.29% |
| 8 | 8385904 | 8527168 | +1.68% |
| 16 | 8385328 | 8598000 | +2.54% |

Instruction counts match exactly at 1 tasklet, +0.03% at 4, +0.12% at 16.

Sweeping DPUs instead, with the work on each held fixed at 4 pairs and 4
tasklets:

| DPUs | cycles, hardware | cycles, uPIMulator | error |
|---|---|---|---|
| 1 | 1895792 | 1803312 | -4.88% |
| 2 | 3897072 | 3690880 | -5.29% |
| 4 | 7809728 | 7381760 | -5.48% |
| 8 | 15599200 | 14763520 | -5.36% |
| 16 | 31227904 | 29527040 | -5.45% |

The error does not grow with the DPU count. uPIMulator's per-DPU figure is
identical to the digit from 2 DPUs up and the hardware's is constant to 0.2%,
so DPUs are independent during a kernel on both and the model composes.

**The timing model is trustworthy to about 5% on this workload.** It is
optimistic when few tasklets run and pessimistic when the pipeline is full. At
one tasklet the DPU issues every 11 cycles regardless, so the cycle count is
11 x instructions plus stalls: hardware spends 12.33 cycles per instruction and
uPIMulator 11.57, which means the 11-cycle interval is exact and about 43% of
the MRAM stall is modelled. Above 8 tasklets the revolver hides the stalls and
that error goes away.

Hardware run-to-run variation is 0.04%, so the trend is real.

    runner/tests/perf/sweep.sh <input.seq> <seq-size> cycles 1 2 4 8 16

The sweep builds both sides from the same `make -n` command, runs each, and
gates every point on `CPU recovery for 0 pairs (0.00%)` and `Results are equal`.

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

### The channel model is unused, not merely untested

`api.CopyTo` and `api.CopyFrom` write DPU memory directly. `Host.Schedule` and
`Host.Check`, which build channel messages, are reached only from
`Simulator.Init`, the standalone `--benchmark` path. A host-to-DPU transfer
under the runner therefore costs **zero simulated cycles** -- confirmed by
moving 4 MB to each of 4 DPUs and back, with the simulator advancing no cycles.

Sweeping DPUs cannot test it, and no M4 number depends on it. Measured against
hardware, the model's constants are close at one DPU and wrong in shape
elsewhere: it assumes exactly 8-way interleaving, so its aggregate bandwidth is
flat to 8 DPUs and then linear, while a real rank rises smoothly and saturates
near 16 DPUs. In between it is 2 to 2.5 times pessimistic. Numbers in
`runner/tests/perf/README.md`.

Putting transfers on the channel model is a design change to the runner, not a
fix. It costs simulation speed and moves every M4 number. Do it before a study
whose conclusion depends on transfer time; skip it for kernel-time studies.

### What is left

1. More workloads and input sizes. The error moves with the input: the same
   comparison at a single pair gives -6.2% rather than -4.9%.
2. `PERF_TIER=1` or `2` to find which part of the kernel the model gets wrong.
   Tier 3 only says the total is 5% out.
3. Route the runner's transfers through the channel model, if and only if a
   study needs transfer time. Recalibrate its bandwidths against
   `runner/tests/perf/xfer_host.c` if so.
4. GIMSA.

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

    # simulator binary
    export PATH=/home/alex/go-sdk/bin:$PATH
    cd uPIMulator/golang/uPIMulator && python3 script/build.py

    # runner (Go c-shared library + C shim + test host)
    uPIMulator/runner/scripts/build.sh

    # API tests (need a completed run's bin dir)
    cd uPIMulator/golang/uPIMulator/src
    UPIM_TEST_BINDIR=/tmp/binS UPIM_TEST_TASKLETS=16 go test ./api/ -v

    # isolation proof
    uPIMulator/runner/scripts/test_isolation.sh

    # ALU / condition-code / DMA conformance against UPMEM's own simulator
    uPIMulator/runner/tests/alu/run.sh

    # M4: BIMSA on hardware vs the simulator (asks for backend=hw deliberately)
    uPIMulator/runner/tests/perf/sweep.sh <input.seq> <seq-size> cycles 1 2 4 8 16
    uPIMulator/runner/tests/perf/sweep_dpus.sh <pair.seq> <seq-size> 4 4 cycles 1 2 4 8 16

    # real host-DPU bandwidth, to calibrate the channel model against
    XDPUS=16 XBYTES_MB=4 UPMEM_PROFILE=backend=hw /tmp/xfer_host

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
