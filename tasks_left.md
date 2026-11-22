# Tasks left

State: 2026-10-03. The validation study (M4) is done on BIMSA, GIMSA and all 13 PrIM
kernels.
**M5, architecture exploration, is cancelled.** It needs a decision on what to
explore before any code is written.

## A run that works, for reference

GIMSA, the largest workload validated. Both the machine and the simulator give
the same answer for every query.

| | |
|---|---|
| Dataset | *Oryza sativa* (asian rice), `dataset/asian_rice.fa`, 1.18 GB FASTA |
| Index | `asian_rice_bitwise/pim_packed/K22_packed_lut12`, K = 22 |
| DPUs | 199, one replica, 12 tasklets each |
| Queries | 2000, length 66, 3 chunks each |
| MRAM per DPU | 59,982,040 B of database -- 57.20 MiB, 89.4% full |
| Result | 100% accuracy, 2000 of 2000, on both sides |
| Wall time | about 3 minutes on the simulator |

**199 DPUs is the floor.** The index is partitioned across exactly that many
and precompute decides it. Expect about 38 GB of host memory.

    cd gimsa && runner/upimulator run --program-dir <bin> --tasklets 12 --dpus 199 -- \
        ./bin/gimsa_kernel_lut.exe -r dataset/asian_rice.fa -q <queries> \
        -k 22 -c 16384 -l 16 -d 202 -m 1536 -b 2000 -t 4 -u 8

Replace the kernel with `gimsa_phases_profile_kernel_lut_phase_profile_cycles.exe`
for the per-phase cycle counts. `HANDOFF.md` lists the four traps in preparing
the query file and the ground truth.

## Done 2026-09-26: the scattered-read error was the register file

The row hypothesis is wrong. On the machine, the address pattern makes no
difference to a small read (`runner/tests/perf/scatter_sweep.sh`). The error
was in the loops around the reads: the register-bank rule counted the wrong
registers. The machine shares each bank's two ports between the registers an
instruction reads and the register the thread's previous instruction writes
(`runner/tests/perf/pipe_sweep.sh`). `CycleRule.ConflictTurns` now does that.
This also closes the old item 3, the two register layouts the rule got wrong.

| workload | before | after |
|---|---|---|
| BIMSA edit distance, 1-16 tasklets | +1.48% to +2.12% | -0.56% to -1.30% |
| BIMSA `extend` | -3.18% | -0.72% |
| GIMSA binary search | -3.59% | -1.09% |
| PrIM `va` / `hst` | unchanged within 0.1% | |

Details in `HANDOFF.md`, "The bank rule, corrected".

## Done 2026-09-28: the DMA engine, idle and busy

`runner/tests/perf/scatter_sweep.sh` fitted the engine against 162 read points
and the write sweeps. No row cost between transfers; one setup per 1 KB row a
transfer touches; a tail after each transfer (busy engine) and a resume slot
after each read (idle thread); a cheaper setup and an acknowledgement delay for
writes. This also closes the flat +1.6% in MRAM throughput: reads are inside
0.8% from 2 to 16 tasklets, and `memory_frequency` did not move.

| workload | before | after |
|---|---|---|
| BIMSA edit distance, 1-16 tasklets | -0.56% to -1.30% | -0.52% to +0.42% |
| BIMSA sections, both tiers | inside 0.5% | inside 0.7% |
| GIMSA binary search / table lookup | -1.09% / +0.17% | +0.61% / -1.17% |
| PrIM `va` / `hst` | +2.10% / -0.18% worst | +1.90% / -0.16% worst |

Details in `HANDOFF.md`, "The DMA engine, refitted".

## Done 2026-09-28: GIMSA's table lookup, and the read-write turnaround

The lookup reads 1032 B slabs while other tasklets write their results. A new
probe (`PROBE=mix`, a read and then a write) showed that the engine pays about
2.4 cycles each time it changes direction, which no probe of reads alone or
writes alone could see: `dma_turnaround_cycles` = 2. Saturated large reads
were also 1% fast; the per-KB tail is now 48, not 43.

| | before | after |
|---|---|---|
| GIMSA table lookup / search / both | -1.17% / +0.61% / +0.30% | -0.87% / +0.67% / +0.40% |
| BIMSA edit distance, 1-16 tasklets | -0.52% to +0.42% | -0.03% to +0.51% |
| BIMSA sections, both tiers | inside 0.66% | inside 0.44% |
| MRAM reads / writes, 2-16 tasklets | 0.8% / 2.0% | 0.23% / 1.85% |
| a read and a write together, rms | 2.62% | 1.59% |

## Done 2026-09-28: PrIM `va`, MRAM refresh and convoys

`va`'s error at 16 tasklets is in the launch's start and end: it falls from
+1.85% to -0.05% as the input grows. Larger inputs showed +2.4% to +4.4% at 4
and 8 tasklets instead, from threads locked into convoys in the DMA engine.
`runner/tests/perf/trace_kernel.c` showed the machine breaking such convoys and
found MRAM refresh: every 2736 cycles (7.8 us), for about 120. Refresh is now
modelled and the DMA constants refitted around it. `va` on large inputs is
inside 2.3%; the worst `va` point moved from +4.35% to -3.40% (2 tasklets,
where the machine keeps a convoy the model breaks). Details in `HANDOFF.md`,
"MRAM refresh, and convoys".

## Done 2026-09-29: which convoys the machine keeps

`va` at 2 tasklets is bistable on the machine itself: 13 of 20 runs keep the
two threads in phase (729k cycles), the rest drift apart (701.6k to 726.5k).
The model always drifts apart (704k), 0.2% below the hardware minimum. A copy
of `va` with stamps added lands both sides in phase, 735.9k against 735.8k, so
the mechanisms agree and the state depends on timing to a few cycles. Three
variations were measured and not kept: WRAM sharing between a transfer and
another thread's loads (the machine shows none), starting a DMA at issue (no
effect), and holding refresh until no transfer runs (`mram_refresh_mode=1`,
kept as an option: it fixes `va` at 2 tasklets and breaks the va-like probe).
`runner/tests/prim/run.sh` now prints the hardware's min..max beside the
median.

## Done 2026-09-29: PrIM `va` at 16 tasklets, and the same code on both sides

The error is in the launch's first few thousand cycles, in the contended
`mem_alloc` after the first barrier. Tracing it found that the two sides do
not run the same code:

- The simulator's bundled runtime (2023 sources) differs from SDK 2025.1's
  prebuilt one. `mem_alloc_nolock` and `perfcounter_config` now give the 2025
  instructions; `__muldf3` still differs in layout. `runner/scripts/rtdiff.py`
  finds such differences.
- GIMSA's hardware binary (built 2026-06-26) was not what the simulator ran.
  Built alike, GIMSA is +0.24% (lookup) and +0.99% (search).
  `runner/scripts/dpuload_override.c` points a hardware run at the right ELF.

What is left of `va` (+1.5% at 16 tasklets) is the order in which a contended
lock is granted, which the simulator does not model.

## Done 2026-09-30: the simulator runs the machine's own DPU ELF

Without `--program-dir` the runner now runs the ELF the host passes to
`dpu_load`, under UPMEM's address map; every test script does so by default
(`UPIM_ELF=0` for the linker path). `golang/uPIMulator/src/elfload` decodes it
through the SDK's `llvm-objdump` and uPIMulator's own grammar. The ALU suite
passes 1224 of 1224, and the timing agrees with the linker path wherever the
code was already the same. GIMSA compares like for like: lookup 0.00%, search
+1.10%. Details in `HANDOFF.md`, "Running the machine's own ELF".

## Done 2026-09-30: the order the threads issue in

A contended lock goes to the first thread that issues after the release, so
the lock order is the scheduler's order. Two new probes read the machine's
order (`runner/tests/perf/sched_sweep.sh`), and three rules reproduce all 42
orders from 3 to 16 tasklets (`thread_policy=1`, the default now): a booted
or resumed thread issues 13 cycles after the boot or resume; such a thread
goes ahead of every thread waiting for its revolver turn; otherwise the one
ready the longest goes first. PrIM `va` and `hst` now execute exactly the
machine's instruction counts at every tasklet count. `thread_policy=0` gives
the old order.

| | before | after |
|---|---|---|
| PrIM `va`, 16 tasklets | +1.88% | -0.24% to 0.00%, inside the machine's range |
| PrIM `va`, 8 tasklets, short / long input | +0.98% / 0.00% | -1.1% to -1.5% / +0.60% |
| BIMSA edit distance, 1-16 tasklets | +0.13% to +0.69% | -0.43% to +0.41% |
| GIMSA lookup / search | 0.00% / +1.10% | -0.27% / +1.12% |
| a read and a write, 4 tasklets, rms | 2.22% | 3.31% (one bistable point) |

Details in `HANDOFF.md`, "The order the threads issue in".

## Done 2026-10-02: GIMSA's binary search, and a read's latency

The search is +0.57% now, from +1.12%. The runner's shim records one DPU's
work in a many-DPU run (`UPIM_RECORD`, `UPIM_RECORD_DPU`), and
`runner/scripts/replay_host.c` replays it on one DPU, on the machine and in
the simulator (`runner/tests/perf/gimsa_replay.sh`). That showed every
replayed DPU slow. Two new probes found why: the one slot a thread paid when
its read completed (`dma_resume_turns`) was a stand-in. One-tasklet loops on
the machine lock to the refresh period (`stair_kernel.c`), and the scatter
loops the slot was fitted on sit on such a lock. The idle read latency
(`refresh_kernel.c`) is 242.5 + 0.4985 x size. The new model has no slot, a
9-cycle read acknowledgement with the engine free, `t_cl` 23 and a per-KB
tail of 25.

| | before | after |
|---|---|---|
| GIMSA lookup / search, 199 DPUs | -0.27% / +1.12% | +0.49% / +0.57% |
| BIMSA edit distance, 1-16 tasklets | -0.43% to +0.41% | -0.74% to +0.09% |
| PrIM `va` 16384, 1-16 tasklets | -1.47% to -0.12% | -0.61% to +0.68% |
| small reads and the mix probe, rms | 1.52% | 1.40% |
| 2 KB loop at one tasklet | -7% | -3.8% |

Details in `HANDOFF.md`, "A read's latency, and refresh lock-in".

## Done 2026-10-03: sets that span ranks

The runner put every DPU on one rank. Now the shim lays a set out as libdpu
does on the machine: ceil(N / 64) ranks, the DPUs dealt over the 8 control
interfaces of each rank (199 DPUs lie 55, 48, 48, 48), measured for 1 to 2560
DPUs. `dpu_get_nr_ranks`, `DPU_RANK_FOREACH`, `dpu_alloc_ranks` and
`dpu_broadcast_to` work, and a copy, a push or a launch can take one rank
(`api.LaunchDpus`). A transfer over several ranks pays 25 us for each rank
after the first and is limited by the host, 11200 MB/s writing and 4050 MB/s
reading (`runner/tests/perf/rank_host.c`). Two shim bugs found and fixed:
`dpu_error_to_string` returned a literal that `DPU_ASSERT` frees, and
`dpu_free` failed on a set never loaded.

| | before | after |
|---|---|---|
| `runner/tests/rank/run.sh`, 1, 64, 65, 199 DPUs | -- | output identical to the machine's |
| BIMSA, 64 / 65 / 128 / 256 DPUs (1, 2, 2, 4 ranks) | -- | -0.25% / -0.32% / -0.28% / -0.23% |
| GIMSA, 199 DPUs on 4 ranks | 2000 of 2000, +0.49% / +0.57% | the same, to the cycle |
| transfers over 2 to 16 ranks, 36 points, rms | 63% | 33% |
| transfers on one rank, 70 points, rms | 20.3% | 20.3% (unchanged) |

Details in the perf README, "Sets that span ranks", and `HANDOFF.md`.

## Done 2026-10-03: the other eleven PrIM kernels

`runner/tests/prim/run_prim.sh` runs BS, GEMV, HST-L, MLP, RED, SCAN-RSS,
SCAN-SSA, SEL, TRNS, TS and UNI with PrIM's own hosts and unchanged kernels,
on the machine and in the simulator. A kernel is linked with
`runner/tests/prim/probe_main.c`, which wraps its `main` with a counter; the
count comes back through `runner/scripts/launch_probe.c` on the machine and
the shim's `PROBE_OUT` in the simulator. These kernels add what no earlier
workload used: mutexes, handshakes, 64-bit arithmetic, software
multiplication and division, and kernels launched two or three times.

| | result |
|---|---|
| results | right on both sides at all 33 points (each host checks its own) |
| cycles, 1, 4 and 16 tasklets | -0.62% to +1.08%, mean absolute 0.32%, rms 0.40% |
| outside 1% | one point: SCAN-SSA at 4 tasklets, +1.08% |
| instructions | exact or inside 0.04%, except TRNS at 16 tasklets, +1.12% (mutex spinning) |

No model change was needed. Details in `runner/tests/prim/README.md`.

## Accepted accuracy limits (decided 2026-10-02, changed 2026-10-07)

**The target is 1%. A workload is valid up to 3%** (decided 2026-10-07). A
workload inside 1% is on target. A workload from 1% to 3% is valid and its
results can be used, but it is off target and stays open work. A workload
above 3% is not valid.

The timing errors below are accepted. They change
the cycle counts the simulator reports. They do not change what a program
computes: in every validation run the outputs were right (GIMSA 2000 of 2000
queries, BIMSA results equal, PrIM byte for byte, ALU 1224 of 1224). An
output can differ only if the program itself depends on timing (it prints
cycle counts, or tasklets race on shared data) or reads MRAM it never wrote
(the simulator starts MRAM as zeros, the machine holds old data).

How far a cycle count can be from the machine:

| case | error |
|---|---|
| BIMSA, GIMSA and PrIM `va`/`hst`, typical | inside 1% (GIMSA +0.49% / +0.57%, BIMSA -0.74% to +0.09%, PrIM -0.61% to +0.68%) |
| the other eleven PrIM kernels, 1, 4 and 16 tasklets (33 points) | -0.62% to +1.08%, rms 0.40%, 32 inside 1% |
| GIMSA, 50,000 reads, rice and human, L66/L88/L132 (`runner/tests/perf/gimsa_reference.sh`, 2026-10-06) | -1.16% to -2.09%: valid, off target. Open work |
| isolated probe points (one transfer size, one gap, one tasklet count) | inside about ±4.7%; exception, not a workload |
| bistable cases | +3% to +4% against the machine's median; exception, compare against the machine's min..max |

The causes, each measured and none modelled:

1. **Reads under contention.** A read costs a thread about two cycles more when
   other threads compete than when it is alone. The model uses the
   workloads' value (`dma_read_ack_cycles` 9); idle one-tasklet reads are 1
   to 4 cycles slow because of it.
2. **Refresh that moves.** The machine's refresh schedule shifts by about half
   a period at times; the model's is a fixed clock. A one-tasklet loop that
   locks to refresh on one side only is off by up to about 4% (MRAM loops of
   1 KB +4.3%, 2 KB -4.0%).
3. **Bistable cases.** The machine itself lands in one of two states from run
   to run, and the model lands in one. `va` on 65536 elements at 4 and 8
   tasklets is +3% to +4% against the median, inside the machine's range in
   some runs; `va` at 2 tasklets; the mix probe at 4 tasklets with 1 KB
   transfers; the staircase at m = 12 and 22. Compare such cases against the
   hardware's min..max (`runner/tests/prim/run.sh` prints it), not its median.
4. **Small reads with 2 to 4 tasklets queueing** are up to 4% fast.

## Open work, in priority order

No task is open by itself now. Each item below waits for a study that needs
it.

1. **Transfers as simulated traffic**, if a study needs a transfer to overlap
   a kernel.
2. **A new workload**, when a study brings one: run it the way `run_prim.sh`
   runs PrIM (its own host, `probe_main.c`, `launch_probe.c`).

## Known gaps, not yet costed

- **WRAM is not modelled faithfully in linker runs.** The linker path places
  it at 512, has 128 KB and always reserves 24 stacks of 2 KB. An ELF run
  (the default now) has the machine's 64 KB from 0 and the ELF's own stacks. The machine has 64 KB and reserves `NR_TASKLETS`
  stacks of the built size. This does not change any timing measured so far,
  but it blocks any study of WRAM capacity.
- **Transfers over many ranks, three shapes not modelled** (measured
  2026-10-02, see the perf README, "Sets that span ranks"): ranks that share a
  memory channel (-40% to -50% at 2 ranks on one channel); broadcasts,
  charged as a write to every DPU where the machine takes 0.4 to 0.8 of
  that; and a launch's host time, 37 us on 1 rank to 200 us on 40. No
  validated workload broadcasts or moves much data over many ranks.
- **`.data` and `.bss` are still linker roots.** The machine drops what nothing
  reaches; the simulator carries about 530 bytes of WRAM it should not. Nothing
  measured depends on it.

## M5, when it restarts

M5 changes the simulated architecture to explore designs no hardware exists
for. The timing constants are already command-line options. The structural
constants -- IRAM 48 KB, WRAM 128 KB, MRAM 64 MB, 24 registers, 24 tasklets --
are fixed in `golang/uPIMulator/src/misc/config_loader.go` and have to be made
adjustable first.

Four directions, each a different piece of work:

| Direction | What it needs first |
|---|---|
| Memory system: wordline size, MRAM timings, DMA engines, bandwidth | Nothing. The DMA constants are all options now; the accepted limits above sit here |
| Threads and pipeline: tasklets above 24, revolver depth, registers | The compiler assumes 24 registers. The bank rule now matches every measured layout |
| On-chip memory: WRAM and IRAM capacity | The WRAM gap above, fixed |
| Scale out: more DPUs, ranks, channels | Nothing for kernels. For transfers, the channel sharing and broadcasts above |
