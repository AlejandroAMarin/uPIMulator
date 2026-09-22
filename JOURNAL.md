# uPIMulator: journal of fixes and changes

A short record of what changed in this fork, and why. One entry per commit, in
order. For the full detail see `HANDOFF.md`, the commit messages, and the
`README.md` in each test directory.

Branch: `sdk-2025-runner`. Upstream: KAIST VIA `uPIMulator` (HPCA 2024).

## Why this work exists

The goal is to use uPIMulator to explore PIM designs that no hardware builds
yet. Before that is worth doing, the simulator must agree with the hardware
that does exist. This machine has 40 real UPMEM v1A DPU ranks, so the agreement
can be measured instead of assumed.

The plan has five steps:

| Step | Content | State |
|---|---|---|
| M0 | Keep simulated runs away from the real ranks | Done |
| M1 | Make the simulator usable as a library | Done |
| M2 | Cut its memory use and make it faster | Done |
| M3 | Run unmodified UPMEM programs on it | Done |
| M4 | Compare it with the hardware and correct it | Done |
| M5 | Change the simulated architecture | Not started |

## 2026-09-16 — Build with the installed SDK

*Commit `f8f847a`.*

Upstream builds inside a Docker image that pins SDK 2023.2.0. That image is
gone, and this machine has SDK 2025.1.0 installed. The build now drives the
installed compiler directly, with no container.

Four settings are easy to undo by accident and break the build if you do:
debug information must be off, the thread count must be given to the compiler,
the CMake targets must produce assembly text and not archives, and one SDK
source file must stay excluded because the 2025 compiler dropped a builtin it
needs. Two of the thirteen sample benchmarks cannot link because of that last
point.

## 2026-09-16 — Make the simulator a library

*Commit `de0cd7d`.*

The simulator was a batch tool. It read files, ran a fixed plan, and printed a
report. A host program that drives DPUs needs the opposite: start, copy data,
launch, poll, read back, repeat. The simulator now offers exactly those calls,
in Go and in C.

Three side effects, all measured on the sample benchmark:

- **Memory: 147 MB to 2.5 MB per simulated DPU.** Memory is now created on
  first use rather than all at once. This is what makes thousands of DPUs
  possible.
- **Speed: 2.26 times faster.** The old thread pool made one thread per job and
  waited twice per simulated cycle. Decoded instructions are also cached now.
- Three latent defects found on the way, in instruction encoding and memory
  routing.

Results stay byte-identical.

## 2026-09-16 — The runner

*Commit `95794bf`.*

    upimulator run --program-dir DIR --tasklets N -- ./your_host args

This runs an **already-compiled, unmodified** UPMEM program with every DPU
operation served by the simulator. The host part runs at full native speed;
only the DPUs are simulated. Nothing in the program is recompiled or patched.

**Hardware isolation is the reason it is built this way.** The UPMEM library
picks real hardware by default when nothing tells it otherwise, and this
account can take ranks. Four independent layers stop a simulated run from
reaching a rank, and a test proves all four hold. Re-run that test after any
change to the runner.

## 2026-09-17 — Eleven functional defects

*Commit `496f793`.*

Double-precision arithmetic gave wrong answers. That made BIMSA's own safety
check reject its own workload, which blocked the whole study. The cause was one
condition flag that no instruction ever set, so every 64-bit shift took the
wrong path.

Looking for it found ten more defects in the same layers:

- Unaligned memory transfers lost data. The hardware ignores the low address
  bits; the simulator did not. This alone made BIMSA report a distance of zero.
- `xor` returned its second operand. `nxor` returned the complement of it.
- Rotate behaved as shift.
- The carry flag after a subtract was inverted.
- Signed overflow after a subtract was computed from the wrong sign.
- Four more condition flags were wrong or could never be set.
- Three instructions were not implemented at all and stopped the simulator.

**None of the thirteen sample benchmarks uses double precision, and none makes
an unaligned transfer.** That is why the published validation never found any
of this.

A new conformance test now holds them fixed. It compares 1176 results against
UPMEM's own functional simulator, which is the only reliable reference for
behaviour the documentation does not state.

## 2026-09-17 — First comparison with the hardware

*Commit `b58a291`.*

The performance counter was not implemented, so any instrumented kernel stopped
the simulator. It is implemented now, and matches the hardware's odd details:
one counter for the whole DPU, and a reading that loses its low four bits.

The clock rate constant was also never filled in, so a kernel that converted
cycles into seconds divided by zero.

First result, BIMSA on one DPU: instruction counts matched the hardware
exactly, and cycle counts were 5% fast below 8 tasklets and 2% slow above.

Two rules came out of this and still apply:

- **Never add a counter reading over tasklets.** One counter serves the whole
  DPU, so each tasklet already counts every tasklet's work.
- **Both sides must run the same binary.** An early comparison was 25% out
  purely because two builds compiled one loop differently.

## 2026-09-17 — Sweep the DPU count

*Commit `fe771c8`.*

The same test with more DPUs and the work on each held fixed. The error does
not grow with the DPU count, on either side. DPUs are independent while a
kernel runs, and the model composes correctly.

It also showed that the runner puts every DPU on one channel and one rank, so
that part of the model is still untested.

## 2026-09-18 — Charge the host transfers

*Commit `392116d`.*

Moving data between the host and the DPUs used to cost zero simulated time. The
simulator now charges it, using a cost model fitted to this machine: a setup
cost, a per-DPU rate, and a rank-wide rate, whichever is slower.

On BIMSA the transfer time is now 4% under the hardware's.

Two findings worth keeping:

- **Fit to the median, not the best.** The first fit used the fastest of 300
  runs and under-predicted by half. A program waits for the typical case.
- **The first transfer costs eight times an ordinary one**, because the rank
  has to wake up. On BIMSA that single transfer is 40% of all data movement.

All seven parameters can be set from the environment, so M5 can vary the link
without rebuilding.

## 2026-09-18 — Fit the memory latency

*Commit `22393d2`.*

This removed the last large error. A test with one tasklet exposes a memory
transfer's latency directly, because the transfer blocks the only thread there
is. Against that test the model was wrong in two opposite directions, which is
why the total looked like a modest 5%: small transfers were 20% too fast and
large ones 23% too slow.

Three causes:

- The transfer engine's fixed startup cost was not modelled at all.
- One memory timing constant was a guess and was 45% too large.
- The transfer queue never counted its own timers down. Harmless while every
  timer was zero, and a permanent stall as soon as one was not.

After the fit, BIMSA agrees with the hardware inside 1.3% over 1 to 16 DPUs,
and inside 3.5% over 1 to 16 tasklets.

## 2026-09-22 — Remove the error that grew with the thread count

The model agreed with the hardware to about 1% at one to four tasklets and was
3.5% out at sixteen. A model whose error grows with the thread count is wrong in
a way a single good number hides, so this looked for the mechanism instead of
tuning the total.

Two mechanisms, both found by writing a small test for one thing at a time:

- **Memory was 15% too fast once several tasklets used it.** The machine costs
  38 cycles to set a transfer up and then about half a cycle per byte, and it
  does the two one after the other, not at the same time. A transfer larger
  than 1 KB pays the setup twice. The model had a third of the setup cost and
  no second charge at all. Error over sizes and tasklet counts: −40%…−14%
  before, −8%…+6% after, and inside 2.3% for every size of 64 bytes and up.
- **Register-bank conflicts were charged to the wrong instructions.** The
  register file has two banks; an instruction whose three registers all fall in
  one bank needs a second issue slot. The model counted the *previous*
  instruction's destination as well, which made ordinary code pay a penalty the
  machine does not charge, and it charged the penalty to the whole DPU instead
  of to the thread, so one tasklet never paid and sixteen paid too much. Eleven
  register layouts were measured on the machine; nine now agree within 0.3%, at
  one tasklet and at sixteen alike. Before, the conflict-free case was 35% out.

**The two had to be fixed together.** Each alone makes the result worse,
because the old model was fast on memory and slow on registers and the two
errors cancelled. That is the reason to test one mechanism at a time.

| tasklets | before | after |
|---|---|---|
| 1 | +0.61% | +2.10% |
| 2 | +0.44% | +2.16% |
| 4 | −0.93% | +1.51% |
| 8 | +2.89% | +1.21% |
| 16 | +3.51% | +1.35% |

The worst case improves from 3.5% to 2.2%, but the point is that the error is
now flat. What remains is one known cause: the per-byte memory rate is 1.6%
high because the model counts whole memory cycles and one of them is worth
3.3%.

Also this day: a probe that resets a shared counter while other tasklets are
still reading it was corrected, a 436 MB hardware trace log was removed from
the working tree, and a trap was written down — building the simulator does not
build the runner's library, and an hour was spent measuring replaced code
because of it.

## Accuracy, then and now

BIMSA on one DPU, simulator against hardware. Lower is better.

| tasklets | first measurement | after the memory fit | now |
|---|---|---|---|
| 1 | -4.86% | +0.61% | +2.10% |
| 2 | -4.93% | +0.44% | +2.16% |
| 4 | -5.29% | -0.93% | +1.51% |
| 8 | +1.68% | +2.89% | +1.21% |
| 16 | +2.54% | +3.51% | +1.35% |

Instruction counts match the hardware exactly at 1 tasklet and within 0.12% at
16. Host transfers are 4% under. Hardware repeats itself to 0.04%, so every
number above is a real difference and not noise.

## Tests added

| Directory | What it holds |
|---|---|
| `runner/tests/alu` | 1176 arithmetic and condition results, against UPMEM's own simulator |
| `runner/tests/fp` | The double-precision case that started all of it |
| `runner/tests/perf` | The hardware-against-simulator measurements and the fits behind them: whole-program, memory latency, memory under contention, register banks, host transfers |
| `runner/scripts/test_isolation.sh` | Proof that a simulated run cannot reach a real rank |

## Still open

1. **More programs and more input sizes.** One program tuned to 2% is not a
   validated simulator. Write a probe for each mechanism first; that is what
   found both of the corrections above.
2. More than one rank or channel. The runner uses one of each.
3. The flat 1.6% left in memory throughput, which the model cannot express
   exactly because its timing constants are whole cycles.
4. GIMSA.
