# M4: hardware vs simulator validation

Measures BIMSA on this machine's UPMEM hardware and on uPIMulator and reports
the error. This is the study the runner was built for.

    runner/tests/perf/sweep.sh <input.seq> <seq-size> <cycles|instructions> <tasklets...>

## Result

BIMSA, `inputs/n2_l129_e2.seq` repeated to 16 pairs, edit distance, 1 DPU,
`MAX_DISTANCE_THRESHOLD=50`, `PERF_TIER=3`, `DYNAMIC=0 BATCH_SIZE=0`.
Cycle counts measured 2026-09-28 with the DMA engine refitted, the read-write
turnaround added and MRAM refresh modelled, instruction counts 2026-09-17. Instruction
counts do not depend on the timing model, so they were not measured again.

**Instruction counts — the functional model.**

| tasklets | hardware | uPIMulator | error |
|---|---|---|---|
| 1 | 607552 | 607552 | exact |
| 4 | 2427616 | 2428320 | +0.03% |
| 16 | 4878128 | 4884112 | +0.12% |

**Cycle counts — the timing model.**

| tasklets | hardware | uPIMulator | error | before the DMA refit | before the bank rule | after the MRAM fit | first measurement |
|---|---|---|---|---|---|---|---|
| 1 | 7503296 | 7536288 | +0.44% | -0.72% | +2.08% | +0.61% | -4.86% |
| 2 | 7550528 | 7582192 | +0.42% | -0.71% | +2.12% | +0.44% | -4.93% |
| 4 | 7754496 | 7777504 | +0.30% | -0.99% | +1.77% | -0.93% | -5.29% |
| 8 | 8375856 | 8401488 | +0.31% | -1.30% | +1.67% | +2.89% | +1.68% |
| 16 | 8371920 | 8420080 | +0.58% | -0.56% | +1.48% | +3.51% | +2.54% |

Hardware run-to-run variation is 0.04%, so these are real.

**What matters here is the shape, not only the size.** The "after the MRAM fit"
column looks better at 1 and 4 tasklets, but its error grows from -0.9% to
+3.5% as tasklets are added, which says something in the model scales wrongly.
Two corrections since -- the memory controller's throughput and the register
file's bank conflicts -- removed that trend. That left a flat +1.5% to +2.1%.

A third correction, to which registers the bank rule counts (see
"Register-file bank conflicts"), took about 2.7% of cycles out of the model and
left -0.56% to -1.30%. The error was smaller at every tasklet count, but its
sign changed, and the per-byte MRAM rate was still 1.6% high: something else
was about 2.5% fast. It was the DMA engine: too little fixed cost when idle,
row costs the machine does not have, and one rate for idle and busy. With the
engine refitted (see "Small reads, and the DMA engine idle and busy") BIMSA is
+0.30% to +0.58%. (It was -0.03% to +0.51% before refresh was modelled.)

Both corrections had to be made together. Each on its own makes BIMSA look
worse, because the old model was fast on memory and slow on register conflicts
and the two errors cancelled.

## MRAM latency

The -5% the first measurement found was all in MRAM. `mram_kernel.c` isolates
it: with one tasklet a DMA blocks the only thread there is, so the stall is
fully exposed and a cycle count is the latency itself rather than something the
scheduler hid.

    runner/tests/perf/mram_sweep.sh

| bytes per DMA | hardware | upstream | first fit | throughput fit | now |
|---|---|---|---|---|---|
| 8 | 209.7 | 167.6 (-20%) | 207.6 (-0.5%) | 191.6 (-8.6%) | 211.2 (+0.7%) |
| 64 | 232.4 | 206.2 (-11%) | 235.3 (+1.3%) | 223.3 (-3.9%) | 242.0 (+4.1%) |
| 256 | 341.6 | 340.5 (-0.4%) | 329.5 (-3.6%) | 329.5 (-3.5%) | 342.1 (-0.0%) |
| 1024 | 714.2 | 877.5 (+23%) | 706.6 (-1.1%) | 754.6 (+5.6%) | 720.0 (+0.8%) |
| 2048 | 1367.7 | 1594.6 (+23%) | 1209.5 (-6.3%) | 1347.6 (-1.5%) | 1267.4 (see below) |

"Now" is the model fitted 2026-09-28 in "The DMA engine, idle and busy" below,
which serves one tasklet and saturation with the same constants. The 2 KB
hardware figure moves between 1280 and 1368 from run to run: the loop around a
2 KB read takes almost exactly half the 2736-cycle MRAM refresh interval, and
on the machine it locks to it.

The model was wrong in two opposite directions at once, which is why the
aggregate error looked like a modest 5%: too little fixed cost per DMA, and too
much per byte. Three things fixed it.

The throughput-fit column is worse than the first fit at four of the five
sizes, and that was a deliberate trade. The first fit read the latency alone and got a
combination of constants that suited one tasklet and no other; fitting the
throughput as well (below) costs a few percent here and removes 15% under
contention, which is what a real kernel runs into. **The machine itself carries
about 0.50 cycles per byte at one tasklet and 0.545 at saturation**, so no
single rate serves both, and that model had one rate. The model now has two;
see "The DMA engine, idle and busy".

**The DMA engine's startup was not modelled at all.** The hardware charges a
few tens of logic cycles before a transfer's first byte reaches memory, whatever
its size, and the DRAM timings do not describe that. `DmaStartupCycles`.

**`t_cl` was 32 and the machine says about 25.** MRAM has no datasheet to read
this off; upstream's 32 made a 2 KB `mram_read` 23% slower than the machine.

**The DMA queue never counted its timer down.** `Dma.Cycle` serviced its queues
but never cycled them. It went unnoticed while every timer was zero, and the
first non-zero startup stalled the DPU forever.

The row buffer was left alone here. Hardware shows no difference at all
between walking MRAM in order and jumping 64 KB each time, so its activation
and precharge are not observable from a DPU. (The simulator did charge them,
about 11 cycles a miss. They are now zero; see below.)

The numbers above are the first fit, which read the latency alone. **MRAM under
contention** below corrects the same two constants against the throughput as
well, and settles the startup's semantics: it is one engine, doing one thing at
a time.

## Section by section, and the gap-affine kernel

    runner/tests/perf/tier_sweep.sh

The tables above compare whole kernels. BIMSA's own probes split a run further,
and a model that is right overall for the wrong reasons shows up here as one
section much further out than the total.

It builds gap-affine deliberately. Every tier-1 and tier-2 probe sits in the
gap-affine path, so an edit-distance build reports no calls for any of them and
99.6% of the time in `other` -- which is how this looked the first time it was
tried. Gap-affine is also the larger kernel: at 4062 instructions it only fits
the 4096-instruction IRAM because the linker now drops the code nothing
reaches.

**Tier 1, where a pair's time goes.** One tasklet, one DPU, 16 pairs.

| section | hardware | uPIMulator | error | before the DMA refit | before the bank rule | calls |
|---|---|---|---|---|---|---|
| align | 42753232 | 42656064 | -0.23% | -0.30% | -0.59% | 112 |
| base | 6040656 | 6050128 | +0.16% | -0.34% | -0.80% | 64 |
| queue | 52384 | 52224 | -0.31% | -0.21% | -0.24% | 96 |
| setup | 29056 | 29872 | +2.81% | -0.49% | -1.20% | 32 |
| total | 49175392 | 49086320 | -0.18% | -0.31% | -0.62% | 16 |

Every section is inside 1.2% and every call count is exact. The error is spread
evenly rather than concentrated, which is what a model that is right for the
right reasons looks like.

Every section is inside 0.31% except `setup`, +2.81%. `setup` is 29 thousand
cycles in 49 million, 32 calls of about 900 cycles; one refresh more or less in
it is 3%.

**Tier 2, where the aligner's time goes.** Same run, the probes moved inside
`aff_align`.

| section | hardware | uPIMulator | error | before the DMA refit | before the bank rule | calls |
|---|---|---|---|---|---|---|
| compute | 21146624 | 21057216 | -0.42% | -0.09% | -0.82% | 1488 |
| extend | 13827520 | 13848816 | +0.15% | -0.72% | -3.18% | 1712 |
| overlap | 6196368 | 6193008 | -0.05% | -0.24% | +0.92% | 400 |
| rings | 2301136 | 2316608 | +0.67% | +0.29% | +0.11% | 336 |
| total | 53120288 | 53048256 | -0.14% | -0.30% | -1.19% | 16 |

**This is what the split was for.** `extend` was out by 3.18% where the whole
kernel was out by 1.19%, so something specific to it was wrong. It is the phase
that walks two sequences comparing characters, in a short loop.

**It was the register file.** The bank rule counted the wrong registers, and a
short loop meets the case it missed on every iteration: the loop counter's
update in front of an instruction that reads two registers of the same parity.
The rest of this section is the search that came before that was found.

Two things it is **not**. The register file charges nothing extra for the
64-bit `ld` and `sd` pairs it is built from -- measured, see below -- and the
memory *rate* is 1.6% slow where this is fast.

What it partly is: **the transfers are 16 bytes** (BIMSA's `SEQT` is 4), and at
one tasklet the model is 6.7% fast at that size. Charging the DMA engine 14
more cycles of startup, which is what the machine asks for at 16 bytes, gives:

| section | hardware | as built | with +14 |
|---|---|---|---|
| compute | 21149968 | -0.82% | -0.13% |
| extend | 13825344 | -3.18% | -1.93% |
| overlap | 6196384 | +0.92% | +2.20% |
| rings | 2302000 | +0.11% | +2.85% |
| total | 53120880 | -1.19% | -0.26% |

So about 40% of `extend`'s error is the small-transfer latency, and the rest is
still unaccounted for. **The change is not a fix**: it over-charges `rings` and
`overlap`, which make fewer and larger transfers, and that is the same wall the
MRAM fit hit. The machine's cost per transfer at one tasklet is not one fixed
cost plus one rate, so no single pair of constants serves every section. A
model that charged the engine differently when it is idle from when it is busy
could; none has been measured. (The corrected bank rule closed most of it,
and the DMA refit, which does charge an idle engine differently from a busy
one, closed the rest: `extend` is +0.15%.)

The two totals differ because a tier's own probes cost time and tier 2 has more
of them. Compare a section against the same tier's total, not across tiers.

## Register-file bank conflicts

    runner/tests/perf/regbank_sweep.sh

This was worth a fifth of the DPU's time at 16 tasklets and nothing had ever
checked it.

    runner/tests/perf/pipe_sweep.sh

The register file has an even bank and an odd bank. Each serves at most two
registers in a cycle. **Those two ports are shared between the registers an
instruction reads and the register the same thread's previous instruction
writes.** The instruction's own destination does not count. So three distinct
registers of one parity, counted that way, need a second issue slot.

Read the measurement at one tasklet, where the revolver gives a thread one slot
every 11 cycles and a lost slot is unmistakable. Each case is eight copies of
the instruction; the loop's own `add r0, r0, -1, nz` writes r0 in front of the
first copy.

| instruction | hardware | uPIMulator | slots lost per iteration |
|---|---|---|---|
| `add r4, r4, r5` | 11.02 | 11.01 | 0 |
| `add r4, r6, r8` | 20.80 | 20.78 | 8: reads two even after an even write |
| `add r5, r7, r9` | 19.59 | 19.56 | 7: the first copy follows r0, which is even |
| `add r4, r5, r7` | 11.03 | 11.01 | 0 |
| `add r4, r5, r6` | 11.02 | 11.01 | 0 |
| `add r5, r4, r6` | 12.25 | 12.23 | 1: only the first copy, after r0 |
| `sub r10, r6, r8` | 20.81 | 20.78 | 8 |
| `add r6, r7, r9` | 11.03 | 11.01 | 0: reads two odd after an even write |

`pipe_kernel.c` separates the two candidate rules with pairs of instructions:

| pair, four times per iteration | hardware | uPIMulator | old rule predicts |
|---|---|---|---|
| `add r5, r5, 1` / `add r4, r6, r8` | 11.02 | 11.00 | 4 slots lost |
| `move r4, 1` / `add r5, r7, r9` | 11.03 | 11.00 | 4 slots lost |
| `add r4, r4, 1` / `add r5, r6, r8` | 15.91 | 15.89 | none |
| `lw r4, ...` / `add r5, r6, r8` | 15.92 | 15.89 | none |
| `ld d4, ...` / `add r6, r7, r9` | 15.92 | 15.89 | none |

A load's destination counts as the previous write, and so do both halves of a
64-bit pair. `add r8, r8, r10` loses a slot after `add r4, r4, 1` and none
after itself, because r8 is then one register. All 35 cases in the two probes
agree within 0.34%, at 1 tasklet and at 16.

**The rule used to count the instruction's own destination with its sources.**
That gets three of the cases above wrong and misses the most common conflict
in a compiled loop: the loop counter's update in front of an instruction that
reads two registers of the same parity. `extend` in BIMSA and the binary search
in GIMSA are both short loops, and this was most of the error in both.

Six more cases cover the forms BIMSA's `extend` phase is built from, which the
`add` forms above do not reach. A 64-bit `ld` or `sd` touches four registers
where a 32-bit one touches two, so it was the obvious suspect when `extend`
turned out to be the worst section in the kernel. It is not:

| instruction | hardware | uPIMulator |
|---|---|---|
| `lw r4, r6, 0` | 11.03 | 11.01 |
| `ld d4, r6, 0` | 11.03 | 11.01 |
| `ld d8, r6, 0` | 11.03 | 11.01 |
| `sw r6, 0, r4` | 11.03 | 11.01 |
| `sd r6, 0, d4` | 11.03 | 11.01 |
| `lsl_add r4, r6, r8, 2` | 20.81 | 20.78 |

The machine charges nothing extra for a 64-bit pair, and charges `lsl_add` the
same as any other three-register instruction. Everything agrees within 0.22%.

**The upstream rule counted the right registers and charged them wrongly.** It
counted this instruction's sources together with the register the instruction
*before* it wrote -- the set the machine uses -- but charged
`floor(even/2) + floor(odd/2)`. That fires on pairs the machine serves for
free, so a plain loop paid a penalty on a third of its instructions. And it
held the whole DPU's pipeline for the penalty rather than the thread's issue
slot, so a single tasklet never paid at all -- the revolver hid it -- while
sixteen paid too much. Measured against the machine, the conflict-free case was
35% slow at 16 tasklets and the conflicting case 6% slow, and at one tasklet the
model showed no penalty where the machine nearly doubles. The first correction
fixed the charge and moved it to the thread, and also replaced the previous
write with the instruction's own destination. That last part was wrong. It is
what the second correction reverses.

**A conflict costs one issue slot of the DPU, charged to the thread.** At one
tasklet the thread's next turn is 11 cycles away, so the instruction takes 22
cycles. At sixteen the slot is one cycle another tasklet would have used. Both
follow from the same rule and both match. See `CycleRule.ConflictTurns` and the
stall in `Logic.ServiceThreadScheduler`.

## Small reads, and the DMA engine idle and busy

    runner/tests/perf/scatter_sweep.sh
    XMODES=seq,none XGAPS=0,4,8,16,32,64,128 runner/tests/perf/scatter_sweep.sh

`scatter_kernel.c` reads 8 B to 2 KB in order, 64 KB apart, and at random
addresses in 8 MB and in 32 MB, from 1 to 16 tasklets, with a variable number
of empty loop iterations (the gap) between reads. Every mode runs the same
instructions, so a difference between modes is the memory. `NODMA=1` runs the
same loop without the read, so the loop's own cost can be taken out.

GIMSA's binary search and BIMSA's `extend` were both about 3.5% fast, and both
make small reads. This probe found four things.

**1. The machine has no cost between reads that land on different rows.** At
one tasklet an 8-byte read costs the same in all four address modes to 0.3%.
The simulator charged precharge and activation, about 11 cycles a miss. Its
`t_rcd`, `t_ras` and `t_rp` are now 0.

**2. The engine sets up once for every 1 KB row a transfer touches.** A random
read of 128 B or more costs more than one in order, and the extra is 38 cycles
times the chance that the read crosses a 1 KB boundary:

| bytes | chance to cross | predicted | measured, 16 tasklets |
|---|---|---|---|
| 64 | 0.055 | +2.1 | +2.1 |
| 128 | 0.117 | +4.4 | +4.3 |
| 256 | 0.24 | +9.1 | +8.9 |
| 512 | 0.49 | +18.8 | +18.8 |
| 1024 | 0.99 | +37.7 | +37.9 |
| 2048 | three rows, not two | +38 | +37.9 |

The model used to charge `ceil(bytes / 1024)` setups. `Dma.Push` now counts the
1 KB rows the MRAM range touches. This is the row effect, but it is inside one
transfer, not between two.

**3. Most of the 3.5% was in the loop, not in the read.** The loop without the
read matched the machine at one tasklet and was one cycle per iteration fast at
12 and 16. `pipe_kernel.c` bisected it to the register-bank rule; see
"Register-file bank conflicts".

**4. What was left in the read: one constant startup cannot serve an idle
engine and a busy one.** With the loop's cost taken out:

| | 8 B | 64 B | 256 B |
|---|---|---|---|
| hardware, thread stall, idle engine (1 tasklet) | 53 | 80.5 | 173 |
| hardware, cost per read, saturated engine | 43.3 | 73.5 | 178.0 |

With the resume slot and the pipeline taken out of the stall, the idle latency
is shorter than the busy engine time by about 4 + 0.04 x bytes. So the machine
wakes the thread before the engine is done with a transfer, and the engine
stays busy for a tail that grows with the size. With the engine idle a thread
sees about 0.50 cycles a byte; saturated, each read costs 0.545. A write
occupies the engine about 10 cycles less than a read, and a thread waits for
it about one issue slot less.

**The model**, fitted against 162 points of this probe (read),
`mram_tasklet_sweep.sh` (write) and `PROBE=mix` (a read and a write together):

| constant | value | what it is |
|---|---|---|
| `t_rcd`, `t_ras`, `t_rp` | 0 (was 32, 78, 32) | no row cost between transfers |
| `t_cl` | 23 (22 until 2026-10-02) | sets the idle per-byte rate, 0.4985 |
| `dma_startup_cycles` | 32 (was 26) | a read's setup, once per 1 KB row touched |
| `dma_tail_cycles` | 2 | engine busy after it wakes the thread |
| `dma_tail_cycles_per_kb` | 25 (45 until 2026-10-02) | and that much more per KB moved: busy rate 0.545 |
| `dma_resume_turns` | 0 (1 until 2026-10-02) | issue slots a thread spends when its read completes |
| `dma_read_ack_cycles` | 9 (new 2026-10-02) | from the engine finishing a read to the thread waking; the engine is free |
| `dma_write_startup_cycles` | 24 | a write's setup |
| `dma_write_ack_cycles` | 9 | from the engine finishing a write to the thread waking; the engine is free |
| `dma_turnaround_cycles` | 2 | engine cost of a transfer that goes the other way from the one before it |
| `mram_refresh_interval` | 2736 | MRAM refreshes every 7.8 us |
| `mram_refresh_cycles` | 120 | and is unavailable for this long each time |

Without refresh (`mram_refresh_interval=0`) the fit is `t_cl` 23, read setup
34, tail 48 per KB, write setup 25, with the rest unchanged. That set is 0.1 to
0.3 points closer on BIMSA and GIMSA and up to 4.4 points further on `va`.

Largest error in each group, reads, all sizes 8 B to 2 KB:

| regime | before | now |
|---|---|---|
| regime | before | without refresh | with refresh |
|---|---|---|---|
| 1 tasklet, no gap | 6.1% | 2.6% | 2.9% |
| 1 tasklet, gap of 32 | 4.2% | 1.4% | 1.0% |
| 4 tasklets, no gap | 5.9% | 4.2% | 2.8% |
| 16 tasklets, no gap | 5.4% | 1.4% | 2.6% |
| 16 tasklets, gap of 32 | 4.5% | 0.4% | 1.2% |
| rms, all 108 points | | 1.18% | 0.97% |

The 4-tasklet group is the weakest. There each thread waits for the others in
the engine's queue, and small reads are 2% to 4% fast. One point, 512 B at 4
tasklets with a gap of 32, is -6.4%, and it was -5.5% before; it is probably
the refresh, below.

**Reads and writes together cost more than each alone.**

    PROBE=mix XMODES=rand XSIZES=8,64,1032 XWSIZES=0,32,256 XGAPS=0,32 \
        TASKLETS_LIST="1 4 12 16" runner/tests/perf/scatter_sweep.sh

`mix_kernel.c` is `scatter_kernel.c` with a write of `WSIZE` bytes after each
read. At 16 tasklets, with the error of the read alone taken out, a small read
and a small write together cost the machine 4.2 to 5.1 cycles more than the
model charged. A stream of reads alone or of writes alone fits, so the cost is
in changing direction: two changes in each pair, about 2.4 cycles each. That is
a bus turnaround. `dma_turnaround_cycles` charges it when a transfer goes the
other way from the one before it. The rms error over the 54 mixed points went
from 2.62% to 1.59%. With 1032 B reads the flat charge is about 3 cycles too
much, which is inside the error those reads have anyway. One point, an 8 B read
and a 32 B write at 12 tasklets with a gap of 32, is -7% with or without it.

This came out of GIMSA's table lookup, which reads 1032 B slabs while other
tasklets write their results. The lookup moved from +0.17% to -1.17% when the
write constants were fitted, because cheaper writes empty the engine's queue
sooner. The turnaround and the busy rate of 48 cycles per KB together take it
to -0.87%.

**MRAM refresh is visible, and modelled.**

    TASKLETS=1 SIZE=8 GAP=0 ITERS=512 runner/tests/perf/trace_sweep.sh

`trace_kernel.c` stamps the counter before and after every read and write.
With one tasklet, about 7% of transfers take up to 90 (read) or 120 (write)
cycles longer than the rest, and they recur in a pattern that repeats every
8208 cycles: three refreshes of 2736 cycles, one of which falls where no
transfer runs. 2736 cycles is 7.8 us at 350 MHz, the usual DRAM refresh
interval, and 120 cycles is about 340 ns, a usual refresh time. The loop that
locked at 1367 cycles in the gap sweep was locked at half the interval.

`Dpu.Cycle` does not advance the memory controller for `mram_refresh_cycles` of
every `mram_refresh_interval` logic cycles; the DMA engine's own setup goes on.
The average cost, 4.4% of memory time, was already inside the fitted
constants, so they were refitted with refresh on.

**Refresh matters most where threads share the engine lightly.** With 4 or 8
tasklets that mostly compute, the model without refresh locked two threads
into a convoy: one thread's read always arrived between the other's read and
write, so both waited a whole transfer every iteration, forever. A trace of the
same probe on the machine shows the same convoys forming, and then breaking
within 10 to 20 iterations, with conflict times that vary by about 120 cycles.
With refresh the model's convoys break too. PrIM `va` on the largest input at 8
tasklets went from +4.35% to -0.09%.

Refresh does not settle every case. At 2 tasklets `va` is bistable on the
machine itself: 13 of 20 runs keep the threads in phase, waiting for each
other (729k cycles), and the rest have them drift apart (701.6k to 726.5k).
The model always drifts apart there (704k), whatever the refresh phase (four
phases give 699k to 712k). A copy of `va` with a few stamping instructions
added lands both sides in the in-phase state, 735.9k against 735.8k. So the
mechanisms agree; which state a run falls into depends on timing to a few
cycles.

Two variations were tried and not kept. `mram_refresh_mode=1` holds a due
refresh until no transfer is in progress, as a DRAM controller does. It keeps
`va`'s in-phase state (726k) but also keeps the convoys the machine breaks in
the va-like probe (+5.6% at 4 tasklets), and it would need its own refit
(saturated transfers come out 2.4% cheap). Starting a DMA at issue rather than
at the end of the pipeline, with the same latency, changed nothing. `mram_refresh_phase` shifts the schedule for such a test. Serving the
lowest-numbered thread first (`dma_policy=1`) instead of the oldest transfer
does not keep the machine's convoys either, and with the engine saturated it
starves the high-numbered threads (up to +36%), so the default stays FIFO.

## A read's latency, and refresh lock-in

    runner/tests/perf/latency_sweep.sh
    runner/tests/perf/gimsa_replay.sh <kernel ELF> <record>...

2026-10-02, from GIMSA's binary search, +1.1% on the same code. The search
is about 18 dependent 16-byte reads per tasklet per launch, so it is a test
of one small read's latency more than of the engine's throughput.

**The resume slot was a stand-in.** The model charged a thread one issue
slot when its read completed (`dma_resume_turns=1`: 11 cycles at one
tasklet, about one at sixteen). A loop of random 16 B reads with m more
instructions in it (`stair_kernel.c`) shows why that fitted: the machine's
cycles per iteration do not grow by 11 per instruction but in steps, and the
steps sit on 2736 / k, the refresh period divided by a whole number (195.4,
210.5, 228, 248.7, 273.6, 304, 342, 391, 456 cycles). A loop whose natural
period is a little short of 2736 / k has one read in each refresh period
caught by the refresh, and the delay stretches the loop to exactly 2736 / k.
The scatter loops that fitted the slot sit on such a step on the machine, so
the slot made the model right there for the wrong reason. The same loop with
one more instruction was one slot slow in the model and no slower on the
machine; extra instructions cost the same before the read as after it.

**The latency itself.** `refresh_kernel.c` reads one block at a time and
takes the mean latency of the reads refresh did not delay. Its stamps advance
in steps of 16, but the reads fall at every point of a step, so the mean is
exact to about a cycle. On the machine a read, stamping included, takes
242.5 + 0.4985 x size cycles:

| bytes | machine | slot model | no slot, ack 7 | no slot, ack 9 (default) |
|---|---|---|---|---|
| 8 | 246.4 | 252.0 | 248.0 | 248.1 |
| 16 | 250.7 | 256.0 | 252.0 | 254.2 |
| 64 | 274.4 | 280.0 | 276.2 | 275.8 |
| 256 | 370.9 | 372.0 | 372.0 | 374.3 |
| 1024 | 755.2 | 741.2 | 757.4 | 757.3 |

The slot was 11 cycles where the machine pays about 6, and the per-byte rate
was 4% fast, which the slot hid at small sizes. The new model has no slot; a
read's completion reaches its thread `dma_read_ack_cycles` cycles after the
engine is done with it, with the engine free meanwhile (as for writes);
`t_cl` is 23, and the busy engine's per-KB tail is 25 so that a saturated
transfer still costs what it did. A latency right to a few cycles matters
because it decides where a loop locks to refresh.

The idle reads fit best with an acknowledgement of 7. The workloads with
several tasklets fit best with 9, and 9 is the default:

| | old model | ack 7 | ack 8 | ack 9 |
|---|---|---|---|---|
| GIMSA lookup / search, 199 DPUs | -0.27% / +1.12% | +0.15% / +0.22% | +0.71% / +0.27% | +0.49% / +0.57% |
| BIMSA edit distance, 1-16 tasklets | -0.43% to +0.41% | -1.13% to -0.18% | -0.80% to -0.08% | -0.74% to +0.09% |
| small reads and the mix probe, rms | 1.52% | 1.57% | 1.52% | 1.40% |
| staircase, rms | 6.4 cycles | 4.3 | 4.1 | 5.2 |

So something costs a thread about two cycles more under contention than
when it is alone, and the model does not have it yet. The GIMSA lookup does
not move in one direction from 7 to 9; at that level the phases depend on
where loops lock to refresh, which moves a few tenths of a percent.

Everything else, with the default:

| workload | old model | new model |
|---|---|---|
| PrIM `va` 16384, 1-16 tasklets | -1.47% to -0.12% (2 bistable) | -0.61% to +0.68% |
| PrIM `va` 65536, 4 / 8 / 16 | -2.20% / +0.60% / -0.15% | +4.16% / +3.32% / -0.06% |
| PrIM `hst`, 1-16 | inside 0.15% | inside 0.11% |
| BIMSA per section, both tiers | worst +3.60% (`setup`) | worst +1.34% (`rings`), totals +0.12% |
| MRAM under contention, 2-16 tasklets | inside 0.3% | inside 0.6% |
| MRAM one tasklet, 8 B to 2 KB | -7.1% to +4.1% | -4.0% to +4.3% |
| 2 KB loop at one tasklet | -7% | -3.8% |

`va` on the long input at 4 and 8 tasklets is bistable on the machine: its
4-tasklet runs go from 1406720 to 1463872 cycles, and the model lands in the
slow state. One-tasklet MRAM loops are off where a loop locks to refresh on
one side and not on the other.

**Refresh is not on a fixed clock.** The reads refresh delays come every
2736 cycles, and a read that meets one waits up to about 120 cycles, as
measured before. But in some runs the schedule moves by about half a period
and later moves back, and the address does not pick which. The model keeps
the fixed schedule. Two points of the staircase (m = 12 and 22) are just
short of a step, and the machine lands on the step in some runs and not in
others.

**GIMSA one DPU at a time.** GIMSA needs 199 DPUs, so the machine cannot
time one of them alone. `UPIM_RECORD=<file with %u>` with
`UPIM_RECORD_DPU=<list>` makes the runner's shim record every copy a DPU
gets, every copy the host reads back, and every launch;
`runner/scripts/replay_host.c` replays that on one DPU, on the machine or
in the simulator, and writes what the host reads back so the two can be
compared byte for byte. The results the host reads agree; the rest of the
buffers differ only where the machine's MRAM holds old data. Each DPU runs
12 to 28 queries per launch, one or two per tasklet.

## The same code on both sides

    python3 runner/scripts/rtdiff.py <hardware ELF> <uPIMulator bin>/main.S
    TASKLETS_LIST="1 2 16" runner/tests/perf/lock_sweep.sh
    # wake_kernel.c (16 tasklets) and spin_kernel.c (2 tasklets) have hosts
    # but no sweep script; build and link them the way lock_sweep.sh does.

A comparison of cycles is only a comparison of models if both sides run the
same instructions. Two ways that fails were found on 2026-09-29.

**The runtime library.** The machine links SDK 2025.1's prebuilt
`librt_v1A.a`. The simulator compiles its own copy of the runtime sources in
`golang/uPIMulator/sdk/syslib`, which come from the 2023 SDK. Disassembling the
hardware ELF (`/usr/bin/llvm-objdump -d`, the UPMEM one) and counting each
function's instructions against the simulator's linked `main.S` found:

| function | machine | simulator | effect |
|---|---|---|---|
| `mem_alloc_nolock` | 11 | 10 | one slot per `mem_alloc`: 5% of a loop of them |
| `perfcounter_config` | 10 | 7 | three instructions per probe start |
| `__muldf3` | 270 | 278 | the same code, laid out differently |

The first two are now written so the simulator's copy gives the 2025
instructions: `mem_alloc_nolock` checks the new heap pointer against the end
of WRAM instead of probing it with a load (the simulator's limit is its own
WRAM end, 131584, because it places WRAM at 512), and `perfcounter_config`
rejects a source of 6 or 7 first. `lock_kernel.c` at one tasklet, 64 calls
of `mem_alloc`: 14176 cycles on both sides, where the simulator was 13472.
At 16 tasklets it went from 8% fast to within the machine's spread.
`__muldf3` cannot be matched this way: the difference is block placement from
another compiler build. Only running the machine's own ELF would remove it.

**The kernel build.** GIMSA's host loads its kernel from a path compiled into
it. The binary at that path was built on 2026-06-26 and is not what the
Makefile builds today (`main_kernel1` 594 instructions against 564, with a
`memset` call the current build does not make). The simulator's image had been
compiled from the current source, so every GIMSA comparison before this date
compared two builds. `runner/scripts/dpuload_override.c` points a hardware run
at another ELF without touching the file at the host's path. With both sides
built from the Makefile's command (`SUBLUT_BITS=8`), three hardware runs:

| per query | machine, median of 3 | machine range | uPIMulator | error |
|---|---|---|---|---|
| table lookup | 1114.9 | 1113.8..1116.9 | 1117.5 | +0.24% |
| binary search | 5225.9 | 5224.8..5229.3 | 5277.5 | +0.99% |
| both | 6340.8 | | 6395.0 | +0.85% |

The earlier "lookup -1.3%, search +1.0%" was largely the two builds.

**The order the threads issue in (2026-09-30).** A contended lock goes to the
first thread that issues its acquire after the release, so the order a lock is
granted in is the order the scheduler issues the threads in. Upstream
uPIMulator goes round the threads by number, and the machine does not:

    runner/tests/perf/sched_sweep.sh

`ring_kernel.c` reads the rotation the boot chain leaves, `grant_kernel.c` the
rotation after a barrier resumes its waiters, and the order the barrier's own
bit went round during the boot. The machine gives the same orders on every
run. Three rules reproduce all of them, from 3 to 16 tasklets
(`thread_policy=1`, now the default):

- **A booted or resumed thread issues 13 cycles after the boot or resume**
  (`boot_delay_cycles`, `resume_delay_cycles`). Tasklet k boots k+1 with its
  third instruction, so k+1 starts 35 cycles after k did, two cycles later in
  the 11-cycle revolver. With 8 tasklets the rotation after boot is
  0 6 1 7 2 3 4 5; the old model gave 0 1 2 ... 7.
- **A woken thread goes first.** A thread that a boot or a resume made
  runnable, and that has not issued since, goes ahead of every thread waiting
  for its revolver turn. So a barrier's waiters, resumed 1, 2, 3 in turn, pack
  into the rotation as 3 2 1: with 4 tasklets the machine grants the next lock
  to 3, 2, 1, and the old model to 1, 2, 3.
- **Otherwise the thread ready the longest goes first**; ties go to the
  lowest number. A thread whose transfer completes is not woken in this
  sense. In PrIM `va` and `hst` the two choices give the same cycle counts.

With these, the simulator runs the machine's own instructions in the machine's
order, and it shows: PrIM `va` and `hst` now execute exactly the machine's
instruction count at every tasklet count, where `va` was 0.57% short at 16
tasklets (fewer spins on `mem_alloc`'s lock). `wake_kernel.c` now leaves the
barrier in the machine's order, each tasklet within 16 cycles of it.

| | old order | machine's order |
|---|---|---|
| PrIM `va`, 16384 elements, 16 tasklets | +1.88% | -0.24% to +0.00%, inside the machine's range |
| PrIM `va`, 16384 elements, 8 tasklets | +0.98% | -1.1% to -1.5%, below the machine's range |
| PrIM `va`, 65536 elements, 8 / 16 tasklets | +0.00% / +0.61% | +0.60% / -0.15% |
| PrIM `hst`, 1-16 tasklets | inside 0.2% | inside 0.15% |
| BIMSA edit distance, 1-16 tasklets | +0.13% to +0.69% | -0.43% to +0.41% |
| a read and a write (`PROBE=mix`), rms at 1 / 4 / 12 / 16 tasklets | 1.81 / 2.22 / 1.66 / 0.79% | 1.81 / 3.31 / 1.58 / 0.77% |
| small reads (`scatter_sweep.sh`), rms at 1 / 4 / 16 tasklets | 0.79 / 0.94 / 0.81% | 0.78 / 0.92 / 0.71% |
| GIMSA lookup / search, the Makefile's build | 0.00% / +1.10% | -0.27% / +1.12% |

The mix probe is the same point for point but one: 1 KB reads and writes
with a gap of 32 at 4 tasklets, -0.04% before and -9.54% now. Four threads
and 1 KB transfers are where the machine keeps or breaks a convoy on timing
of a few cycles (see the refresh and convoy part of "Small reads, and the DMA
engine idle and busy"), and the new order lands
this point in the other state. The BIMSA hardware numbers moved by 0.37% at 8
tasklets between two runs on the same day, more than the 0.04% measured
before, so that point may be bistable too.

`va` at 8 tasklets on the short input is now fast by about 1%; on the long
input it is inside the machine's range, so the difference is in the start or
the end of the launch. An acquire that tests its bit a few cycles after it
issues was tried as well: it gives the machine's order only with 11 tasklets
or more and was not kept. `spin_kernel.c` shows that a thread waiting on a
lock is modelled right: the holder's loop takes 23088 cycles on the machine and
23024 in the simulator.

**Running the machine's own ELF (2026-09-30).** The runner now runs, by
default, the DPU ELF the host passes to `dpu_load`: the very file the machine
runs, with the SDK's own runtime. `golang/uPIMulator/src/elfload` reads its
sections and symbols, decodes `.text` with the SDK's `/usr/bin/llvm-objdump`,
and reads the result back with uPIMulator's own assembly grammar, which
accepts every instruction in BIMSA, GIMSA, PrIM and every probe here. The
simulator then uses UPMEM's address map (`misc.UseHardwareMemoryMap`): a code
address is an instruction index, as the machine's jump tables and `call`
store it; WRAM is 64 KB from 0; MRAM is tagged 0x08000000 but a DMA takes its
address modulo 64 MB, as the machine does (a kernel's first MRAM variable is
loaded as address 0). `--program-dir` still runs a linker image instead, and
every script here takes `UPIM_ELF=0` for that.

Checked in ELF mode: the ALU suite, 1224 of 1224 results; the register-bank
probe, 34 cases inside 0.28%; MRAM latency the same as the linker path to 0.1%;
BIMSA +0.10% to +0.70% at 1 to 16 tasklets; PrIM `va` and `hst` the same as
the linker path within 0.3%; `--trace-hardware` clean. GIMSA, which could not
be compared like for like before, now runs its own kernel on both sides:

| GIMSA per query | machine | uPIMulator, the same ELF | error |
|---|---|---|---|
| lookup, the host's June binary | 1121.1 | 1112.9 | -0.73% |
| search, the host's June binary | 5227.0 | 5281.8 | +1.05% |
| lookup, the Makefile's build | 1114.9 (median of 3) | 1114.9 | 0.00% |
| search, the Makefile's build | 5225.9 (median of 3) | 5283.5 | +1.10% |

`DPU_LOAD_OVERRIDE=<ELF>` points a run at another kernel: the shim honours it
in the simulator, and `runner/scripts/dpuload_override.c` does the same for a
hardware run.

`pipe_kernel.c` cases 23 to 36 cover what this needed: acquire and release,
failed acquires, calls and returns, and `mem_alloc`'s path in pieces. All
agree within 0.3%. One case found that the simulator places WRAM at 512: an
absolute address below that is WRAM on the machine and a fault in the
simulator. Compiled programs never use one, because the linker places every
WRAM symbol above 512.

## MRAM under contention

    runner/tests/perf/mram_tasklet_sweep.sh

`mram_sweep.sh` uses one tasklet, so a DMA blocks the only thread and what it
reports is a latency. The same kernel with more tasklets reports the memory
controller's throughput instead, because the revolver keeps other threads ready.

The machine saturates at 4 tasklets and then obeys one line exactly, over five
size steps:

    cycles per DMA = 38.1 x (1 KB rows touched) + 0.545 x bytes

(These reads are aligned, so rows touched is `ceil(bytes / 1024)`; that is how
it was first written.)

Two things in it are worth stating. **The fixed 38 cycles is the DMA engine's
own setup, not a row activation.** Pointing every tasklet at the same MRAM row
costs exactly what pointing them 64 KB apart costs, so the row buffer is not
what the number measures. And **a transfer that touches two 1 KB rows pays the
setup twice**. See "Small reads, and the DMA engine idle and busy".

**The engine does one thing at a time.** The two terms add rather than one
hiding the other, which says the setup of one transfer does not overlap the
data of the one before it. The model reflects that: `Dma` hands the memory
controller one transfer and waits for it to finish, the startup only counts
down while the controller is idle, and after a transfer the engine stays busy
for its tail.

Error, cycles per DMA, sequential, with the 2026-09-28 model:

| bytes | 1 tasklet | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| read 64 | +3.9% | +2.6% | -0.3% | -0.1% | -0.2% |
| read 512 | +0.4% | +0.3% | +0.2% | +0.2% | +0.2% |
| read 2048 | refresh lock | -0.6% | -0.7% | -0.7% | -0.7% |
| write 64 | +1.1% | +0.5% | +0.6% | +1.7% | +1.6% |
| write 512 | -0.3% | +0.2% | +0.2% | +0.2% | +0.2% |
| write 2048 | refresh lock | -0.6% | -0.6% | -0.6% | -0.6% |

"Refresh lock": a 2 KB transfer at one tasklet runs a loop of almost exactly
half the refresh interval, and on the machine it locks there (1357 to 1368
cycles). The model does not lock and is 7% fast. This is the one case where
the model with refresh is still visibly wrong about refresh.

It was -16% to -40% at 4 tasklets and above before the first fit, and a flat
+1.6% after it. That +1.6% was the per-byte rate: `t_cl` counts whole memory
cycles and one step is worth 3.3%, so no value of it could state the machine's
0.545. With the tail carrying part of the busy rate, `t_cl` sets only the idle
rate, and 22 plus 45 cycles per KB of tail plus refresh comes to the
machine's figure.
`memory_frequency` did not move.

## DPU sweep

Same workload on each DPU (4 pairs, 4 tasklets), the DPU count swept and the
input regenerated each time so the work per DPU stays fixed.

    runner/tests/perf/sweep_dpus.sh <base-pair.seq> 200 4 4 cycles 1 2 4 8 16

| DPUs | pairs | hardware | uPIMulator | error | first measurement |
|---|---|---|---|---|---|
| 1 | 4 | 1895584 | 1931648 | +1.90% | -4.88% |
| 2 | 8 | 3914576 | 3956080 | +1.06% | -5.29% |
| 4 | 16 | 7793488 | 7912160 | +1.52% | -5.48% |
| 8 | 32 | 15577120 | 15824320 | +1.59% | -5.36% |
| 16 | 64 | 31253920 | 31648640 | +1.26% | -5.45% |

The error does not grow with the DPU count, and it is the same +1.1% to +1.9%
the tasklet sweep shows. uPIMulator's per-DPU figure is identical to the digit
from 2 DPUs up and the hardware's is constant to 0.3%: DPUs share nothing
during a kernel on either side, and the model composes accordingly.

Beyond one rank (2026-10-03, the model of that date, 4 tasklets, 4 pairs per
DPU; 65 DPUs lie 33 and 32 on two ranks, 256 on four):

    runner/tests/perf/sweep_dpus.sh inputs/n2_l129_e2.seq 200 4 4 cycles 64 65 128 256

| DPUs | ranks | hardware | uPIMulator | error | hardware per DPU-tasklet | uPIMulator per DPU-tasklet |
|---|---|---|---|---|---|---|
| 64 | 1 | 124839248 | 124533248 | -0.25% | 487653 | 486458 |
| 65 | 2 | 126837312 | 126428928 | -0.32% | 487836 | 486265 |
| 128 | 2 | 249757920 | 249066496 | -0.28% | 487808 | 486458 |
| 256 | 4 | 499285744 | 498132992 | -0.23% | 487584 | 486458 |

The ranks change nothing in a kernel, on either side: the machine's figure per
DPU is constant to 0.05% from 1 rank to 4, and the simulator's is the same
number at 64, 128 and 256 DPUs (65 DPUs differ by 0.04%, on both sides alike). Transfers are another matter; see "Sets that span ranks".

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

### Sets that span ranks (2026-10-03)

Until this date the runner put every DPU on one rank. A set of more than 64
DPUs was charged as one rank of that width, and `dpu_get_nr_ranks` said 1. Now
the shim lays a set out as libdpu does on the machine: ceil(N / 64) ranks, the
DPUs dealt one at a time over the 8 control interfaces of every rank, so 199
DPUs lie 55, 48, 48, 48 (`runner/tests/rank/README.md` has the rule and the
measured layouts). A copy, a push, a launch and `DPU_FOREACH` can take one
rank, and `runner/tests/rank/run.sh` checks each against the machine: the
outputs are identical at 1, 64, 65 and 199 DPUs.

A transfer over several ranks gets two more terms:

    cycles = setup + (ranks - 1) * rank_setup
             + max( bytes / dpu_rate , bytes * rank_dpus / rank_rate , bytes * dpus / host_rate )

`rank_dpus` is the fullest rank of the transfer. The ranks move in parallel,
so the rank limit applies to one rank only, but the host gathers or scatters
the data of all of them, and that is what limits a large transfer over many
ranks. The host also starts the ranks one after another, so each rank after the
first adds a setup.

| parameter | value | what it is |
|---|---|---|
| `TransferRankSetupNs` | 25000 | extra setup, each rank after the first |
| `HostWriteMbps` | 11200 | what the host writes, over every rank of a transfer |
| `HostReadMbps` | 4050 | what the host reads, over every rank of a transfer |

`UPIM_TRANSFER_RANK_SETUP_NS`, `UPIM_HOST_WRITE_MBPS` and
`UPIM_HOST_READ_MBPS` set them. A host rate of 0 turns that limit off. A
transfer on one rank costs what it did before, to the cycle.

`rank_host.c` measured it: `dpu_push_xfer` to and from 64 to 1024 DPUs on 1 to
16 ranks, at 512 B, 32 KB and 256 KB per DPU, three runs, medians. The fit
uses those 36 points with the 70 one-rank points of `xfer_host.c` from the
same day; `golang/uPIMulator/src/api/transfer_test.go` holds all 106 and fails
if the model gets worse than this:

| points | before | now |
|---|---|---|
| one rank, 1 to 64 DPUs, 70 points | rms 20.3% | rms 20.3% |
| 2 to 16 ranks, 36 points | rms 63%, median 51% | rms 33%, median 24% |

| DPUs | ranks | dir | bytes/DPU | hardware us | model us | error | before |
|---|---|---|---|---|---|---|---|
| 65 | 2 | write | 512 | 76 | 58 | -24% | -56% |
| 65 | 2 | write | 262144 | 3184 | 1576 | -50% | -44% |
| 128 | 2 | write | 512 | 80 | 61 | -24% | -54% |
| 128 | 2 | write | 262144 | 4990 | 3051 | -39% | -30% |
| 199 | 4 | write | 512 | 103 | 114 | +11% | -61% |
| 199 | 4 | write | 262144 | 5457 | 4763 | -13% | -1% |
| 512 | 8 | write | 512 | 203 | 228 | +13% | -72% |
| 512 | 8 | write | 262144 | 13817 | 12189 | -12% | +0% |
| 1024 | 16 | write | 512 | 316 | 452 | +43% | -73% |
| 1024 | 16 | write | 262144 | 19363 | 24372 | +26% | +43% |
| 65 | 2 | read | 512 | 112 | 63 | -44% | -64% |
| 65 | 2 | read | 262144 | 8467 | 4262 | -50% | -40% |
| 128 | 2 | read | 512 | 135 | 71 | -47% | -63% |
| 128 | 2 | read | 262144 | 11222 | 8340 | -26% | -12% |
| 199 | 4 | read | 512 | 128 | 130 | +2% | -53% |
| 199 | 4 | read | 262144 | 12068 | 12986 | +8% | +27% |
| 512 | 8 | read | 512 | 272 | 270 | -1% | -61% |
| 512 | 8 | read | 262144 | 36912 | 33345 | -10% | +7% |
| 1024 | 16 | read | 512 | 447 | 534 | +20% | -59% |
| 1024 | 16 | read | 262144 | 44861 | 66685 | +49% | +76% |

    XPROFILE=backend=hw UPMEM_PROFILE=backend=hw XBIN=/tmp/xfer_dpu runner/tests/perf/rank_host > ranks.tsv

What it does not model, all measured:

- **Ranks that share a channel.** Two ranks on one memory channel are slower
  than one rank with the same bytes: 65 DPUs on two ranks write 256 KB each in
  3184 us, 64 DPUs on one rank in 2008 us. This is the -40% to -50% at 2 ranks.
  Which ranks a set gets, and so how many share a channel, depends on the
  machine's allocator and on what other processes hold (here 4 ranks were two
  on channel 1 and two on channel 3; 8 ranks put four on channel 4). A model
  of it fits 18% to 20% rms on these points but needs the allocator's choice,
  so it is left out.
- **Broadcasts** (`dpu_broadcast_to`, `dpu_copy_to` on a set) are charged as a
  write to every DPU. The machine sends the bytes once per rank and takes 0.4
  to 0.8 times that: up to 3 times less on 16 ranks. BIMSA and GIMSA copy to
  one DPU at a time and never broadcast.
- **A launch** costs the host time the simulator does not charge: an empty
  kernel takes 37 us on 1 rank, 47 to 54 us on 2 to 4, 77 us on 8, 106 us on
  16 and 200 us on 40.
- **The first transfer** pays `TransferFirstNs` once per set. On the machine a
  rank already brought up by an earlier set of the same process does not pay
  it again, and new ranks pay it partly in parallel.

The kernel itself does not depend on the ranks: a DPU's cycles are the same in
a set of 1 DPU and in a set of 256 (see "DPU sweep").

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

**Timing only, never outputs.** Everything below changes the cycle counts the
simulator reports, not what a program computes. In every run in this
directory the outputs were right: GIMSA 2000 of 2000 queries, BIMSA's results
equal, PrIM byte for byte, the ALU suite 1224 of 1224. An output can differ
only if the program depends on timing (it prints cycle counts, or its tasklets
race on shared data) or reads MRAM it never wrote (the simulator starts MRAM
as zeros, the machine holds old data). The target is 1%, and a workload is
valid up to 3% (2026-10-07). The timing errors listed here are
accepted (2026-10-02): the workloads come out inside 1% (one PrIM point
at +1.08%; GIMSA at 50,000 reads -1.16% to -2.09%, valid but off target,
`gimsa_reference.sh`), isolated probe
points inside about ±4.7%, bistable cases +3% to +4% against the machine's
median.

- A read costs a thread about two cycles more under contention than alone.
  The model uses the contended value (`dma_read_ack_cycles` 9), so idle
  one-tasklet reads are 1 to 4 cycles slow. See "A read's latency, and
  refresh lock-in".
- Fifteen workloads: BIMSA, GIMSA and the 13 PrIM kernels, each on one input
  (`runner/tests/prim`). That is not a claim about every kernel, and the error
  is known to move with the input: an early BIMSA
  comparison at a single pair gave -6.2% where 16 pairs gave -4.9%.
- MRAM refresh is modelled as one fixed schedule. The machine's schedule
  moves by about half a period at times. One-tasklet loops lock to refresh
  (their period sits on 2736 / k); where one side locks and the other does
  not, the loop is off by up to about 4% (MRAM loops of 1 KB +4.3%, 2 KB
  -4.0%).
- Threads that share the DMA engine lightly can lock into convoys. The machine
  breaks some and keeps others, and the model does not always agree which.
  PrIM `va` at 2 tasklets, and on 65536 elements at 4 and 8 tasklets (+3% to
  +4% against the median), is bistable on the machine itself; compare such
  cases against the hardware's min..max, which `runner/tests/prim/run.sh`
  prints.
- With 2 to 4 tasklets queueing for the DMA engine, small reads are up to 4%
  fast.
- A transfer over several ranks is charged with a host limit and a setup per
  rank (see "Sets that span ranks"): 33% rms over 2 to 16 ranks. Ranks that
  share a memory channel, broadcasts and the launch's own host time are not
  modelled.
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
