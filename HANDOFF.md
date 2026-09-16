# Handoff: hardware-vs-simulator validation study

State as of 2026-09-16. Read this before touching anything.

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
| M4 validation study | **Blocked** by the bug below |
| M5 architecture exploration | Not started |

## THE BLOCKER: double-precision arithmetic is wrong

Same kernel source, two simulators, no hardware involved in either run:

| Operation | UPMEM functional simulator | uPIMulator |
|---|---|---|
| `(uint32_t)(double)152` | 152 | **0** |
| `152 + 2.0` | 154 | **0** |
| `152 - 2.0` | 150 | **0** |
| `152 * 0.8` | 121 | **3435973836** |

Reproduce both sides with `runner/tests/fp/` (see its README for the commands).

**Why it blocks BIMSA.** BIMSA's kernel sizes its wavefront buffers with
`max_distance = 2 * longest_seq * MAX_ERROR`, a double multiply. The garbage
value inflates the computed MRAM footprint, so BIMSA's own guard
(`ma_tasks + NR_TASKLETS * QUEUE_SZ > MRAM_LIMIT`, `dpu/bimsa.c:223`) rejects the
workload and the kernel returns before writing anything. Every buffer the host
reads back is zero, which looks like a broken transfer but is not one. I verified
the layout arithmetic independently: it should pass at 1,876,704 against a limit
of 64,000,000, and my replication reproduced the observed transfer offsets exactly.

**None of the 13 PrIM benchmarks uses double precision**, so uPIMulator's
published validation never exercised this path. Confirmed by grepping every
compiled PrIM kernel for `__muldf3`/`__adddf3`/`__floatunsidf`/`__fixdfsi`: zero hits.

## Next steps (the decision was: fix it properly)

Root cause is **not** pinned down. What is known:

- The soft-float routines are linked in and reachable
  (`__floatunsidf` at 395976, `__muldf3` at 397704, `__adddf3` at 401040 in a
  test image).
- `simulator/dpu/logic/logic.go` has **25 handlers that panic with
  "is not yet implemented"**, including `ExecuteSRr`, `ExecuteSRrc`,
  `ExecuteSRrci`, `ExecuteSRric`, `ExecuteSRrr`, `ExecuteSRrrc`, `ExecuteSRrrci`,
  `ExecuteSRrri`, `ExecuteSRrrici`, `ExecuteSRirc`, `ExecuteSRirci`, `ExecuteR`,
  `ExecuteRci`, `ExecuteZci`.
- The suffixed pair-register forms (`move.s`, `move.u`, `lsr.u`, `lw.s`) that
  soft-float uses map to several of those handlers.
- **But that hypothesis is not proven**: the failing run produced wrong answers
  without panicking, and PrIM emits `move.s`/`move.u` too (82 and 7 occurrences)
  while VA runs correctly. So either those instructions are not reached on the
  failing path, or something else is wrong.

Suggested approach:

1. Build the isolation kernel from `runner/tests/fp/kernel.c` and run it under
   uPIMulator with `--verbose 1`, which prints the instruction executed each
   cycle. Start with only the convert case (`OUT_CONV`), the simplest failure.
2. Run the same kernel on `backend=simulator` (reference) and diff the behaviour
   at the first divergence. The reference cannot dump a trace, so compare
   register/memory outcomes at function boundaries
   (`__floatunsidf` -> `__fixdfsi`) instead.
3. Once the divergent instruction is identified, implement or correct its handler.
   Note that `Encode`/`Decode` for a form can be right while `Execute` is missing;
   check all three.
4. Re-run `runner/tests/fp/` until all four cases match the reference, then
   re-run BIMSA.

Do not "fix" this by changing BIMSA. Changing the workload changes what the study
measures.

## After the fix: resume M4

1. Build BIMSA's kernel and link it (see "Commands" below), then run BIMSA.
   Expect correct scores and CIGARs.
2. Reference hardware runs: build with `PERF=1 PERF_TIER=3 PERF_CYCLES=1
   DYNAMIC=0 BATCH_SIZE=0`, run with `-e 1 -w 0`.
3. Compare **`max` over tasklets of `PERF_ACC[t][PERF_S_TOTAL]`**, never the sum:
   one perfcounter register is shared by all 24 hardware threads, so the sum is
   about NR_TASKLETS x the wall clock. `PERF_CLK` reads 350000000 here.
4. Simulator side: `UPIM_STATS=<file>` writes `logic_cycle`, `num_instructions`
   and `memory_cycle` per DPU at `dpu_free`.
5. Do instruction counts first (`PERF_INSTRUCTIONS=1` vs `num_instructions`):
   frequency independent, so it validates the functional model before the timing
   model.
6. **Gate every data point on `CPU recovery for 0 pairs (0.00%)`.** Also only
   `-b 0 -e 1` runs are analysable: `global_pair` in the kernel is a `.data`
   global that is never reset per launch.

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

    # link an arbitrary kernel into a program directory
    ./build/uPIMulator --root_dirpath $PWD --bin_dirpath <out> \
        --benchmark <NAME> --benchmark_asm <kernel>.c.o --link_only true \
        --num_tasklets N --num_channels 1 --num_ranks_per_channel 1 --num_dpus_per_rank 1

    # run a compiled UPMEM host binary on the simulator
    uPIMulator/runner/upimulator run --program-dir <out> --tasklets N --dpus M \
        [--verbose] [--trace-hardware] -- ./host args
    # UPIM_STATS=- prints counters; UPIM_DUMP_LOG=1 prints the DPU's own log

BIMSA's kernel flags come from `make -n` in `bimsa/upmem`; add `-S -g0
-DNR_THREADS=24` and point the includes at uPIMulator's bundled SDK
(`sdk/misc`, `sdk/stdlib`, `sdk/syslib`).

## Things already established -- do not re-derive

- BIMSA's and GIMSA's kernels compile clean against uPIMulator's bundled headers
  and use only supported directives and mnemonics. BIMSA is 3462 instructions,
  inside the 4096 IRAM limit that hardware also imposes.
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

## Fixes already made to upstream code

- `EncodeSRrr` tested `suffix != RRR` when the dispatcher only routes
  `S_RRR`/`U_RRR` there, so it could never succeed; `DecodeSRrr` rejected the
  unsigned variant. Audited every Encode/Decode pair: only these two were wrong.
- `CommandLineParser.Parse` segfaulted on an unknown or valueless `--option`.
- `api.Set.Free` crashed on a set that was never launched.
- The whole SDK 2025.1.0 port (Docker removed, `-g0`, `-DNR_THREADS=24`,
  `OBJECT` libraries, `.c.o` filter in the linker). See git history.
