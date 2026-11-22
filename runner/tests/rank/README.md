# Sets that span ranks

`run.sh` runs `host.c` on the machine and in the simulator and compares the two
outputs line by line. They must be identical.

    runner/tests/rank/run.sh                 # 199, 65, 64 and 1 DPUs
    SKIP_HW=1 runner/tests/rank/run.sh 199   # the simulator only

`host.c` checks these, for one set size:

- How `dpu_alloc` lays the DPUs on ranks, from `dpu_get_nr_ranks` and
  `DPU_RANK_FOREACH`, and that `DPU_FOREACH` over one rank counts from 0.
- A `dpu_copy_to` to one rank reaches that rank only.
- A `dpu_push_xfer` to one rank moves that rank's staged buffers only, also
  when every DPU of the set has a buffer staged.
- A `dpu_launch` of one rank starts that rank only: the kernel counts its
  launches, and rank 1, launched twice, must count 2 where the others count 1.
- `dpu_alloc_ranks(2)` gives 128 DPUs on 2 ranks.

## What the machine does

A set of N DPUs takes ceil(N / 64) ranks. Its DPUs are dealt one at a time
over the 8 control interfaces of every rank, rank by rank, so the ranks are as
even as the interfaces allow:

| DPUs | ranks | DPUs on each rank |
|---|---|---|
| 65 | 2 | 33 32 |
| 100 | 2 | 52 48 |
| 130 | 3 | 48 42 40 |
| 199 | 4 | 55 48 48 48 |
| 300 | 5 | 64 64 60 56 56 |
| 1000 | 16 | 64 × 13, 56 × 3 |

`runner/tests/perf/rank_host.c` measured this for sets of 1 to 2560 DPUs
(2026-10-02). The shim lays the DPUs out by the same rule.
`UPIM_DPUS_PER_RANK` changes the rank width (64 by default).

## What the simulator does not do

- **One program per set.** A `dpu_load` of part of a set fails with
  `DPU_ERR_INVALID_DPU_SET`. Use one set per kernel.
- **`dpu_free` frees a whole set**, as on the machine. Freeing part of a set
  fails.
- **Every DPU works.** The machine can disable a faulty DPU, which changes the
  layout. This machine has none disabled.
