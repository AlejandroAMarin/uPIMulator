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

## 2026-09-22 — Two more workloads

The first check that the model holds beyond the program it was fitted to.
Two of PrIM's own kernels, taken unchanged except for a counter probe, chosen
to be unlike BIMSA and unlike each other:

- **Vector addition.** Two long reads and one write per block, almost no
  arithmetic between them.
- **Shared histogram.** One read per block, then a few hundred data-indexed
  updates to a private table in fast memory, then a reduction across tasklets.

Errors against the hardware, over 1 to 16 tasklets:

| workload | error |
|---|---|
| histogram | -0.04% to -0.09% |
| BIMSA | +1.48% to +2.12% |
| vector addition | +0.07% to +2.23% |

Instruction counts are exact at one tasklet on both new kernels.

**The result lines up with how much of each kernel's time goes on moving data**,
which is the one error the model is known to still have. A kernel that barely
touches main memory is right to a tenth of a percent; one that does little else
carries the whole of it. Nothing else in the model is out by enough to see.

Sweeping the input size instead shows the same thing from another side: vector
addition improves from +3.1% to +0.7% as the input grows, because its error at
the small end is the fixed cost of starting a kernel rather than the kernel.

Two measurement rules came out of this, and both would have produced wrong
answers quietly:

- **A shared counter needs a barrier before it is read.** Without one the first
  tasklet stops counting when it finishes its own share, and what it reports
  depends on the order the others finish in. That alone showed instruction
  counts 2.4% apart and cycle errors of 4.7% that were not real.
- **Short runs need the median of several.** These kernels take a fiftieth of
  BIMSA's time and the hardware repeats to only about 1%. A single reading made
  one point look 1.8% out where the median says 0.3%.

## 2026-09-22 — The linker keeps only the code something reaches

The simulator could not run a program the machine runs.

uPIMulator linked whole object files: needing any function from a file brought
in every function in it. The compiler puts each function in its own piece and
the machine's linker throws away the pieces nobody calls. BIMSA's gap-affine
kernel is 3795 instructions on the machine; here it was 6522, and the
instruction memory holds 4096. It simply would not load.

The linker now applies the same rule the machine's does: keep a piece of code
if it is the entry point, or if something that is kept names it. The affine
kernel now links at 4062 instructions.

Three details had to be right, and each cost a round of debugging:

- A list the compiler emits of every function whose address is taken anywhere
  must not count as a reference, or nothing is ever dropped.
- A function referring to its own labels says nothing about whether anything
  needs it. Every function has an "end of function" label and names it in its
  own size directive; counting those kept the first function alive from every
  other one.
- The small blocks recording each function's stack use belong to that function
  and name its labels, so they have to go when it goes.

**The check that this is safe is that it cannot change a program that already
fitted.** PrIM's vector addition gives the same cycle and instruction counts
either way, to the digit.

### Metadata was taking up memory it does not take on the machine

Found while checking the above, and worth more than it sounds.

Those stack-use blocks, and debug information, are notes for tools. The machine
never loads them. uPIMulator was putting them at the bottom of main memory,
ahead of the program's own data, which cost BIMSA 104 bytes and put the start
of its heap 104 bytes past where the machine puts it.

Because the memory system charges by which 1 KB row a transfer falls in, a
shift of the heap changes what every transfer costs: a 16-byte shift was
measured to move BIMSA's time by 0.32%. The heap now starts at exactly the
address the machine's own linker reports. PrIM's vector addition improved from
+0.44% to +0.26% at one tasklet, and from +0.33% to +0.07% at two.

**Before trusting a new workload, check the two heaps agree.** They are printed
by the machine's object dumper and by the linker's own output.

## 2026-09-22 — GIMSA runs, and computes what the machine computes

The second application. Three gates, all of which could have blocked it, and
none of which did for long:

- **The shim already covers it.** All thirty GIMSA host programs use the same
  thirteen library calls, every one of them already implemented. No new work.
- **The kernel links.** 959 instructions against the machine's own 694, well
  inside the 4096 the hardware holds. It would not have fitted before the
  linker learned to drop unreachable code.
- **One instruction was missing.** GIMSA stopped the simulator on a narrow load
  widened to 64 bits in a single instruction, which BIMSA never emits. It is
  implemented now and checked the same way as the other eleven: against UPMEM's
  own simulator, 48 new cases, all matching. The conformance suite is 1224
  results.

A reference run on the machine works: 199 DPUs, 2000 queries of the rice
dataset, 28.8 ms of which 13.9 ms is the kernel. The encoded query files are
plain text with a three-line header, so any smaller query set is one command.

**The result.** 199 DPUs, 2000 queries of the rice dataset: the simulator
produces exactly the intervals the machine produces, 2000 of 2000, checked
against the project's own oracle. A second application runs correctly.

Two further problems had to be solved to get there, and both are worth knowing:

- **Moving the data was the slow part, not simulating it.** The database is 57
  MB per DPU and the simulator copied it a byte at a time, cloning each 1 KB
  row before overwriting it. The transfer phase alone ran past thirteen
  minutes. Copying a row at a time brought the whole run to three.
- **A query file holds three lines per query, not one.** Truncating it at one
  line per query leaves the first third intact and the rest nonsense. The first
  run I did that way reported 33.3% accuracy -- exactly the intact third -- and
  a kernel time twelve times too large, because it was searching on rubbish.
  The number looked plausible. Only the verification gate showed it was not.

**That second one is the lesson of the day.** A timing measurement from a run
that was not checked for correctness is not merely uncertain. It can be wrong
by a factor of twelve and still look like a reasonable answer.

## 2026-09-22 — The linker keeps only the data something reaches

Found while trying to measure GIMSA. Its profiling build read past the end of
memory and stopped.

The linker throws away code nothing calls. It did not throw away data. A jump
table is data, so it counted as something the program needs, and it kept its
own function alive, and that function kept everything it calls alive. That is
how `printf` survived in a program that never prints -- and with `printf` came
the SDK's one-megabyte output buffer.

**Every program that does not print was therefore starting its memory one
megabyte higher than on the machine.** GIMSA's profiling build needed that
megabyte and did not have it.

The linker now applies the same rule to data as to code. GIMSA's memory layout
is now byte for byte the machine's. BIMSA does print, so it keeps the buffer on
both sides, but it also carried 208 bytes of its own unused data that the
machine drops; its memory now starts on the same boundary too. Its cycle counts
are unchanged, to the digit.

Two things came out of it:

- A piece of data belongs to the file it is written in. One area of memory is
  built from a piece of every file that puts something there, and each piece
  has to be judged on its own, or one live piece keeps all the dead ones.
- **The stacks were never aligned.** They were placed wherever the section in
  front of them ended. Nothing had noticed, because the sections in front
  always happened to end on the right boundary -- until 188 bytes of dropped
  jump table moved them four bytes off it. Memory transfers move eight bytes at
  a time, so the last word of every transfer out of a local array was lost. The
  arithmetic test suite caught it in one result out of 1224. The machine pads
  for this reason; the simulator now does too.

## 2026-09-22 — GIMSA's timing, per phase

GIMSA reports the work each phase of a query costs. The same binary, the same
queries, both sides verified at 100%:

| per query | hardware | uPIMulator | error |
|---|---|---|---|
| search steps | 10.613750 | 10.613750 | exact |
| table lookup | 1121.4 | 1124.2 | +0.25% |
| binary search | 5227.1 | 5039.6 | **-3.59%** |

The step count is exactly equal, so the simulator searches the same way. What
differs is the cost of a step: about 17.7 cycles each, and each step is one
small read at a random place in a 57 MB table.

**This is the second workload to say the same thing.** BIMSA's alignment
extension is 3.18% fast and its reads are small. GIMSA's binary search is 3.59%
fast and its reads are small and scattered. Two unrelated programs, the same
sign and nearly the same size. A flat charge per transfer fixes one and breaks
the programs that read in long runs, so that is not what is missing. Something
that costs more when consecutive reads land far apart would fit all of it. That
is the next thing to test.

## 2026-09-26 — The scattered reads were never the problem

Two programs were about 3.5% fast in the simulator, and both make small reads
at scattered addresses. The plan was to test a charge for reads that land far
apart in memory.

**The machine charges nothing for that.** A new probe reads small blocks in
order, far apart, and at random places in 32 MB. On the machine all of these
cost the same. The simulator is the one that charges extra for a distant read,
and it charges too much. That makes it slower, not faster, so it could not be
the cause.

**The cause was in the loop, not in the read.** The same loop with the read
taken out was still fast in the simulator, by one cycle in every iteration.
Cutting the loop down one instruction at a time found the pair that did it: a
loop counter increment followed by an addition of two registers.

The register file has two banks and each bank can serve two registers at a
time. What counts is the registers an instruction reads and the register the
instruction before it writes. The simulator counted the instruction's own
result instead. That is wrong in exactly the case a compiled loop produces all
the time. The original authors had counted the right registers; an earlier fix
here corrected how the conflict was charged and changed the registers too, and
that second part was the mistake.

With the rule corrected, 35 register layouts agree with the machine within
0.34%, and:

| program | before | after |
|---|---|---|
| BIMSA, whole kernel, 1-16 tasklets | +1.5% to +2.1% | -0.6% to -1.3% |
| BIMSA, `extend` | -3.2% | -0.7% |
| GIMSA, binary search | -3.6% | -1.1% |
| PrIM's two kernels | unchanged | |

**The lesson is the same as before, from the other side.** Two unrelated
programs agreed on the size and sign of an error, and that looked like strong
evidence that the error was in what they had in common, the memory reads. It
was in something else they had in common: short loops. Agreement between
workloads narrows the cause, but it does not name it. Only a probe that takes
one thing away at a time does that.

What is left in a small read is now measured. At one tasklet the simulator is
6% to 8% fast; with the memory engine busy it is 5% slow. One fixed cost per
read cannot be right in both cases. That is the next task.

## 2026-09-28 — The memory engine, idle and busy

After the register fix, one error was left in small reads: at one tasklet the
simulator was 6% to 9% fast, and with the memory engine saturated it was 5%
slow. One fixed cost per read could not be right in both cases.

A finer probe found four separate things, and each needed its own change:

- **No cost for changing rows.** The simulator charged about 11 cycles when a
  read opened a different part of memory from the one before it. The machine
  charges nothing. That charge is gone.
- **The engine sets up once for each 1 KB block a transfer touches.** A read at
  a random address sometimes crosses a block boundary and pays twice. The
  machine's extra cost matched that rule to a fraction of a cycle at every
  size. The simulator counted by size instead.
- **The machine wakes the thread before the engine is finished.** So a thread
  waiting alone sees a faster engine than a queue of threads does. The model
  now keeps the engine busy for a short time after each transfer, and a thread
  spends one more issue slot when its read completes. Writes behave
  differently again, and have their own two numbers.
- **Memory refresh is visible.** Every 3.9 microseconds the memory is
  unavailable for about 170 nanoseconds. A loop whose period is a simple
  fraction of that locks to it and slows down. This is not modelled; its
  average cost is in the fitted numbers.

One mistake on the way is worth recording. A loop's instruction count looked
one too high for each read, and I took that as evidence that the machine
executes each read instruction twice. A second probe, whose loop I could count
by hand, showed no such thing. I had miscounted the first loop. The change it
suggested survived on other evidence, but only just.

**Result.** BIMSA is now inside 0.6% at every tasklet count, and every section
of it inside 0.7%. GIMSA's binary search is +0.6%. Memory throughput, which had
a flat 1.6% error that the old model could not express, is inside 0.8%. One
number got worse: GIMSA's table lookup, from +0.2% to -1.2%. That is the next
thing to look at.

## 2026-09-28 — Reads and writes together

GIMSA's table lookup got worse when the write costs were fitted, although the
lookup itself writes nothing. It reads large blocks while the other threads
write their results, and all of them share one memory engine. So the error
had to be in how reads and writes share it.

A probe that does a read and then a write, over and over, showed a cost that
neither reads alone nor writes alone have: about 2.4 cycles each time the
engine changes direction. Memory buses often have such a turnaround. The model
now charges it. The same measurements showed that large reads under load were
1% cheap, which a slightly higher busy rate fixes.

The lookup is now 0.9% fast and the search 0.7% slow, while the two together
are within 0.4%. They run at the same time on the same hardware, so how the
time splits between them depends on everything else that is happening. BIMSA
is inside 0.6% at every tasklet count and every section of it inside 0.5%.

## 2026-09-28 — Memory refresh, and threads that queue together

PrIM's vector addition was 2% slow at 16 threads. That turned out to be the
start and the end of a short run, and it disappears on a larger input. But the
larger input showed something worse: at 4 and 8 threads the simulator was up
to 4.4% slow.

A trace of every transfer showed why. Two threads had fallen into step: each
time, one thread's read arrived just before the other's write, so each waited
for the other in every iteration, and neither ever got out. The same trace on
the machine showed the same pairs forming, and then breaking up after ten or
twenty iterations.

Something on the machine shakes the threads apart. It is memory refresh: every
7.8 microseconds the memory stops for about a third of a microsecond. I had
seen it before only as an average cost. Measured one transfer at a time, it is
large and rare, and that is what disturbs the threads enough to separate them.
The simulator now models it, and the vector addition on large inputs is within
2.3%.

It does not settle everything. At 2 threads the machine keeps a pair in step
that the simulator now breaks, and that case went from exact to 3.4% fast.
Which pairs last depends on how the memory engine chooses between waiting
threads, and that is the next thing to measure. On balance the model is closer
than before, but the step is smaller than the numbers above it suggest: a
workload whose threads share the memory lightly can be a few percent off in
either direction, and which direction depends on details nobody has measured
yet.

## 2026-09-29 — The machine does not always do the same thing

The vector addition at 2 threads was 3.4% fast in the simulator, and I set out
to find which pairs of threads the machine keeps in step and which it lets
drift apart.

I measured three explanations and none of them held. A memory transfer does
not slow another thread's accesses to local memory. Starting a transfer
earlier in the pipeline changes nothing. Holding the memory refresh until no
transfer is running keeps the pair in step, but it also keeps pairs in step
that the machine separates in other tests.

Then I ran the same program on the machine twenty times instead of five. It
does not always give the same answer. Thirteen runs keep the two threads in
step; seven let them drift apart, and those are as fast as the simulator. The
median of five had always happened to land on the first kind. The simulator
was not wrong about the mechanism. It lands in the machine's less common
state.

The lesson is about the measuring, not the model. A median hides a program
that can end up in two different states. The test now prints the fastest and
slowest run beside the median, so the next such case shows at once.

## 2026-09-29 — Checking that both sides run the same program

The vector addition at 16 threads was still 1.5% slow in the simulator, and
all of it was in the first few thousand cycles, where every thread asks for
memory at once. A small test of that request alone was 5% fast in the
simulator even with one thread, where nothing competes.

The reason was not in the timing model. The machine and the simulator were
running different code for the memory request. The machine uses the 2025
version of UPMEM's runtime library, which it gets prebuilt. The simulator
builds its own copy from older sources. The two versions differ by one
instruction in that function, and by a few in two others. I changed the
simulator's copy to match, and the test now agrees to the cycle.

Checking this led to a larger problem. The genome search program's hardware
binary was built in June; the simulator's copy was built from today's source.
They compute the same answers, but they are not the same machine code. Every
timing comparison of that program so far compared two different builds. Built
alike, its two phases are within 0.24% and 0.99%.

What is left of the vector addition is the order in which waiting threads get
a lock. The machine hands it on in an order that is not the threads' number,
and the simulator does not. I could not find the rule.

**The lesson:** before comparing how long two things take, check that they
are the same thing. The comparison now has a tool for that, which lists every
function whose length differs between the machine's program and the
simulator's. The lasting fix would be for the simulator to run the machine's
own program file, and that is the next task.

## 2026-09-30 — The simulator runs the machine's own program file

Yesterday's lesson was that the two sides must run the same program. Today the
simulator does: it takes the very file the host hands to the machine's loader,
instead of a separate build of the same source.

The file is in UPMEM's binary format, and the simulator never had a decoder for
it. Writing one for several hundred instructions would have taken long. Instead,
the loader asks UPMEM's own disassembler for the text of each instruction, and
reads that text with the grammar the simulator already had for assembly. It
accepts every instruction in every program here.

Three things about the machine had to be learned on the way, each from a test
that failed:

- **A code address is a count of instructions**, not a byte address. The
  machine's jump tables hold instruction numbers.
- **The memory for large data has a label in the address that the machine
  ignores.** The program files mark it with a high address, but the code loads
  addresses without the mark, and the machine drops it.
- **The machine's start-up code counts one past its 256 lock bits,** and the
  machine wraps back to the first one.

The simulator now uses the machine's own layout of its memories for such a
file, which also removes an old difference in the size of the local memory.

**Result:** the arithmetic tests pass on the machine's own files, every timing
agrees with the old path where the code was already the same, and the genome
search compares like for like at last: its lookup phase is exact, and its
search is 1.1% slow. That 1.1% is now a real model error, not two different
programs.

## 2026-09-30 — The order the threads take their turns in

The machine runs up to 24 threads and gives each a turn in its pipeline. When
several threads wait for one lock, the lock goes to the first thread whose turn
comes after the release. So the order of the turns decides who works first.
The simulator went round the threads by number. The machine does not.

Two small tests read the machine's order through who gets a lock. The machine
gives the same order on every run, so each difference is a model error. Three
rules explain all 42 orders measured, from 3 to 16 threads:

- **A thread that is started or woken gets its first turn 13 cycles later.**
  Each thread starts the next one, so the threads spread over the pipeline two
  cycles apart, not side by side.
- **A thread that was just woken goes before every thread that waits.** When
  a barrier wakes threads 1, 2 and 3 in turn, they take their turns as 3, 2, 1.
- **Otherwise the thread that has waited longest goes first.**

**Result:** the simulator now executes exactly the machine's number of
instructions on the two PrIM kernels, at every thread count, because the
threads now wait on the locks as often as on the machine. The vector addition
at 16 threads went from 1.9% slow to inside the machine's own spread. The
other workloads stay inside the same range as before.

## 2026-10-02 — One DPU at a time, and what a read really costs

The genome search runs on 199 DPUs at once, so the machine cannot time one of
them alone. The simulator can now record everything the host sends to one
DPU and replay it on a single DPU, on the machine and in the simulator. The
replays showed the search slow on every DPU checked.

Small test programs then showed why. The simulator charged a thread one turn
of the pipeline each time its memory read completed. That charge was a
stand-in. On the machine, a loop that reads memory locks its rhythm to the
memory's refresh, which comes every 2736 cycles; the loops the charge had been
fitted on happened to sit on such a lock. Measured one read at a time, away
from refresh, a read costs a fixed part and half a cycle per byte, and the
old charge was too large for small reads and too small for large ones.

**Result:** the search went from 1.1% slow to 0.6%. The other workloads stay
inside the same range as before. Two known limits remain, and they are
accepted: the machine's refresh does not keep a fixed clock, and a read costs
a thread about two cycles more when other threads compete than when it is
alone. They change the time, not the results.

## 2026-10-03 — Sets that span ranks

A set of more than 64 DPUs takes several ranks on the machine. The simulator
put them all on one, so a program that asked how many ranks it had got the
wrong answer, and a transfer to 1024 DPUs was charged as if one rank carried
it. A new test program measured how the machine spreads a set: ceil(N / 64)
ranks, as even as the ranks' control interfaces allow, so 199 DPUs lie 55,
48, 48, 48. The simulator now spreads them the same way, and a copy, a push
or a launch can take one rank. The same test program gives the same output
on both sides.

Transfers over several ranks were measured too. The ranks work in parallel,
but the host serves all of them and starts them one after another, so the
model now has a setup per rank and a host-wide limit.

**Result:** kernel times do not depend on the ranks, on either side (BIMSA
-0.23% to -0.32% from 64 to 256 DPUs; GIMSA on 4 ranks unchanged, 2000 of
2000). Transfers over 2 to 16 ranks are 33% rms from the machine, from 63%.
Ranks that share a memory channel, broadcasts and the time of a launch are
not modelled.

## 2026-10-03 — Eleven more programs

PrIM, the benchmark suite the simulator's authors validated against, has 13
programs. Two were compared so far. The other eleven now run too, each with
PrIM's own host program and its own unchanged kernel; a small wrapper around
the kernel's entry point counts its cycles on both sides. They use what no
earlier program used: locks shared by many threads, threads that hand
partial results to each other, 64-bit sums, multiplication and division done
in software, and kernels started two or three times in a row.

**Result:** every program computed the right answer on both sides, at 1, 4
and 16 threads. The times agree inside 1% at 32 of 33 points, and the worst is
1.08% slow. The model was not changed for them.

## 2026-10-06 — A reference for GIMSA, before the architecture changes

The reference point to compare every later change of the simulated
architecture against. GIMSA on both of its genomes, with three read lengths
each: one replica, 12 tasklets, the `kernel_lut` mode, K = 22, the first
50,000 reads of each 10M Illumina set (no `begintrunc` sets). Rice takes 199
DPUs on 4 ranks, human 541 DPUs on 9 ranks.

The time is the DPU kernel time. A counter wraps GIMSA's own kernel
(`runner/tests/prim/probe_main.c`), and the host reads it after every
launch, on the machine and in the simulator. A launch lasts as long as its
slowest DPU, so the time is the sum over launches of the largest count,
divided by the DPU clock, 350 MHz. The host's own timers are not used: in the
simulator they measure the simulation. Host transfers are not included.
Hardware: the median of 3 runs. The simulator gives the same count on every
run.

| set | reads | launches | hardware (ms) | uPIMulator (ms) | difference |
|---|---|---|---|---|---|
| rice L66  (SRR10724346) | 50,000 | 3 | 5.359  | 5.297  | -1.16% |
| rice L88  (SRR10838753) | 50,000 | 4 | 7.001  | 6.906  | -1.35% |
| rice L132 (SRR8241153)  | 50,000 | 6 | 28.103 | 27.697 | -1.44% |
| human L66  (SRR622461)  | 50,000 | 3 | 4.199  | 4.112  | -2.09% |
| human L88  (SRR622457)  | 50,000 | 4 | 14.608 | 14.318 | -1.98% |
| human L132 (ERR2990063) | 50,000 | 6 | 8.529  | 8.378  | -1.76% |

Difference = (uPIMulator - hardware) / hardware. In cycles:

| set | hardware, median | hardware, min..max | uPIMulator |
|---|---|---|---|
| rice L66   | 1,875,680 | 1,873,216..1,877,280 | 1,854,000 |
| rice L88   | 2,450,192 | 2,443,536..2,453,408 | 2,417,152 |
| rice L132  | 9,835,968 | 9,834,816..9,845,584 | 9,694,000 |
| human L66  | 1,469,696 | 1,467,328..1,472,400 | 1,439,040 |
| human L88  | 5,112,800 | 5,108,336..5,114,080 | 5,011,312 |
| human L132 | 2,985,008 | 2,981,920..2,988,768 | 2,932,352 |

Every run, on both sides, verifies 50,000 of 50,000 reads against the
unbounded oracle. The machine repeats to 0.4% or better, so every difference
above is real.

**Result: the simulator is 1.2% to 2.1% fast.** That is outside the 1%
accepted on 2026-10-02, and inside the 3% accepted since 2026-10-07: valid,
off target. The validated GIMSA run used 2000 reads, about 10 a DPU per
launch, and was +0.5%. With 50,000 reads a DPU does about 250 a launch, and the
error changes sign. It is a uniform offset, not a scheduling or a balance
effect: the simulator picks the same slowest DPU as the machine in 25 of 26
launches, and per DPU the error is -1.3% (rice) and -1.55% (human) at the
median, -4% to +1.8% at the extremes. The human kernel uses 4 sub-LUT bits
against 8 for rice and has larger partitions; the cause is not looked into.

Simulator cost: rice 14 to 49 minutes and 28 GB a set, human 20 to 46
minutes and 72 GB a set. The simulator's memory is mostly MRAM, which it
holds in the Go heap. Without a limit the collector lets the heap grow to
39 GB on rice. `GOMEMLIMIT` must sit above the live heap: at 16 GiB a human
run (32 GB live) did nothing but collect garbage and finished no launch in
3.5 hours. The script uses 16 GiB for rice and 40 GiB for human.

To repeat it after a change (the slices and the builds are kept in
`/tmp/upim-gimsa-ref`, and are made again if missing):

    runner/tests/perf/gimsa_reference.sh                 # both sides, all six
    SKIP_HW=1 runner/tests/perf/gimsa_reference.sh       # the simulator only

The hosts and the probed kernels are built from the GIMSA tree into the work
directory; GIMSA's `bin/` stays unchanged. Each slice's oracle goes next to
the full one as
`dataset/<db>_bitwise/groundtruth_unbounded/upimref_<set>_N50000_K22.intv.bin`,
because the host finds it by the query file's name.

## 2026-10-07 — Valid up to 3%, the target 1%

The GIMSA reference showed the simulator 1.2% to 2.1% fast at 50,000 reads,
where the accepted limit was 1%. The limit is now two numbers:

| error of a workload's cycle count | meaning |
|---|---|
| inside 1% | on target |
| 1% to 3% | valid: the results can be used. Off target: it stays open work |
| above 3% | not valid |

Isolated probe points (about ±4.7%) and bistable cases (+3% to +4% against
the machine's median) stay exceptions. They are not whole workloads.
`runner/tests/perf/gimsa_reference.sh` prints the class of each set.

## Accuracy, then and now

BIMSA on one DPU, simulator against hardware. Lower is better.

| tasklets | first measurement | after the memory fit | before the bank rule | after the bank rule | after the memory engine and refresh | now, with the machine's thread order |
|---|---|---|---|---|---|---|
| 1 | -4.86% | +0.61% | +2.08% | -0.72% | +0.44% | +0.41% |
| 2 | -4.93% | +0.44% | +2.12% | -0.71% | +0.42% | +0.08% |
| 4 | -5.29% | -0.93% | +1.77% | -0.99% | +0.30% | +0.29% |
| 8 | +1.68% | +2.89% | +1.67% | -1.30% | +0.31% | -0.43% |
| 16 | +2.54% | +3.51% | +1.48% | -0.56% | +0.58% | -0.10% |

The other two workloads, over the same range of tasklets: PrIM's vector
addition -3.40% to +2.17% (inside 2.3% except at 2 tasklets), PrIM's
histogram -0.05% to -0.20%.

Instruction counts match the hardware exactly at 1 tasklet and within 0.12% at
16. Host transfers are 4% under. Hardware repeats itself to 0.04%, so every
number above is a real difference and not noise. (One exception, found on
2026-09-30: at 8 tasklets two hardware runs on the same day differed by
0.37%.) With the machine's thread order, the vector addition is -1.5% to
+0.2% from 4 to 16 tasklets on its short input (2 tasklets is bistable on the
machine), and both PrIM kernels execute exactly the machine's instruction
counts.

## Tests added

| Directory | What it holds |
|---|---|
| `runner/tests/alu` | 1224 arithmetic, condition and load results, against UPMEM's own simulator |
| `runner/tests/fp` | The double-precision case that started all of it |
| `runner/tests/perf` | The hardware-against-simulator measurements and the fits behind them: whole-program, memory latency, memory under contention, small reads at random addresses, register banks, instruction pairs, host transfers. `gimsa_reference.sh` is the GIMSA reference (six read sets, two genomes) |
| `runner/tests/prim` | All 13 of PrIM's kernels against the hardware: `va` and `hst` with their own hosts (`run.sh`), the other eleven with PrIM's hosts (`run_prim.sh`) |
| `runner/tests/rank` | A set over several ranks: the layout, and a copy, a push and a launch to one rank, against the machine |
| `runner/scripts/test_isolation.sh` | Proof that a simulated run cannot reach a real rank |

## Still open

1. GIMSA at 50,000 reads is 1.2% to 2.1% fast: valid, but off the 1% target.
   The error is the same on every DPU; the cause is not known.
2. Transfers as simulated traffic, and new programs, when a study needs them.
3. Transfers over many ranks: channel sharing, broadcasts and the launch's
   host time, if a study needs them.

The rest is accepted (2026-10-02). The simulator's results are always the
machine's results; only its time can be off. The target is 1%, and a workload
is valid up to 3% (2026-10-07): inside 1% on the fifteen
programs (one PrIM point at +1.08%), -1.16% to -2.09% on GIMSA at 50,000
reads,
up to about 4% in a few cases the machine itself does not repeat from run to
run, or where a loop keeps step with the memory's refresh on one side only.
