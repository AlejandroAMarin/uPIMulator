# Double-precision regression test

Same kernel source, two simulators. UPMEM's own functional simulator gets every
case right; uPIMulator gets every case wrong.

| Operation | UPMEM functional simulator | uPIMulator |
|---|---|---|
| `(uint32_t)(double)152` | 152 | 0 |
| `152 + 2.0` | 154 | 0 |
| `152 - 2.0` | 150 | 0 |
| `152 * 0.8` | 121 | 3435973836 |

This blocks BIMSA. Its kernel sizes the wavefront buffers with
`max_distance = 2 * longest_seq * MAX_ERROR`, a double multiply. The garbage
result makes the computed MRAM footprint enormous, so BIMSA's own guard
(`ma_tasks + NR_TASKLETS * QUEUE_SZ > MRAM_LIMIT`) rejects the workload and the
kernel returns before writing anything. Every buffer the host reads back is
zero, which looks like a transfer failure but is not one.

None of the 13 PrIM benchmarks uses double precision, so uPIMulator's published
validation never exercised this path.

## Running it

    # uPIMulator
    dpu-upmem-dpurte-clang -w -O2 -S -g0 -DNR_TASKLETS=1 -DNR_THREADS=24 \
        -I<sdk>/misc -I<sdk>/stdlib -I<sdk>/syslib -o task.c.o kernel.c
    uPIMulator --benchmark FPTEST --benchmark_asm task.c.o --link_only true \
        --num_tasklets 1 --bin_dirpath <dir> --root_dirpath <root>
    ./upimulator run --program-dir <dir> --tasklets 1 -- ./host_upimulator

    # reference, no hardware
    dpu-upmem-dpurte-clang -w -O2 -DNR_TASKLETS=1 -o fp_dpu kernel.c
    UPMEM_PROFILE=backend=simulator ./host_reference
