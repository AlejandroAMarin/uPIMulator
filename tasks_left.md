# Tasks left

State: 2026-09-23. The validation study (M4) is done on four workloads.
**M5, architecture exploration, is cancelled.** It needs a decision on what to
explore before any code is written.

## Open work, in priority order

1. **The cost of a scattered MRAM read.** The simulator is about 3.5% fast on
   small reads at random addresses. Two unrelated kernels agree: BIMSA's
   `extend` (-3.18%) and GIMSA's binary search (-3.59%). Raising the DMA
   startup fixes one and over-charges the kernels that read in long runs, so
   the missing cost is not flat. Test a charge that depends on whether
   consecutive reads land on the same row. **This is the next task.**

2. **A flat +1.6% in MRAM throughput.** `t_cl` counts whole memory cycles and
   one step is worth 3.3%, so the model cannot state the machine's rate
   exactly. Do not move `memory_frequency` to hide it; that is a documented
   number.

3. **Two register layouts the bank-conflict rule gets wrong**, each by one
   issue slot per loop iteration, at the boundary between one instruction and
   the next. A rule over pairs of instructions might catch them.

4. **More than one rank or channel.** The runner puts every DPU on one of
   each. That topology is untested.

5. **More workloads.** Four is better than one, but a kernel that spans ranks
   would test something these do not.

6. **Transfers as simulated traffic** instead of an analytic charge. Only
   needed for a study where a transfer overlaps a kernel. It does not here.

## Known gaps, not yet costed

- **WRAM is not modelled faithfully.** The simulator has 128 KB and always
  reserves 24 stacks of 2 KB. The machine has 64 KB and reserves `NR_TASKLETS`
  stacks of the built size. This does not change any timing measured so far,
  but it blocks any study of WRAM capacity.
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
| Memory system: wordline size, MRAM timings, DMA engines, bandwidth | Nothing. Item 1 above sits here |
| Threads and pipeline: tasklets above 24, revolver depth, registers | The bank-conflict rule generalised; the compiler assumes 24 registers |
| On-chip memory: WRAM and IRAM capacity | The WRAM gap above, fixed |
| Scale out: more DPUs, ranks, channels | Item 4 above |
