/* Drives trace_kernel.c and prints every stamp: tasklet, iteration, and the
 * counter before the first read, after it, after the second read, and after
 * the write. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t iters = (uint32_t)atoi(getenv("ITERS") ? getenv("ITERS") : "30");
    uint32_t nt = (uint32_t)atoi(getenv("TASKLETS") ? getenv("TASKLETS") : "4");
    uint32_t size = (uint32_t)atoi(getenv("SIZE") ? getenv("SIZE") : "1024");
    uint32_t gap = (uint32_t)atoi(getenv("GAP") ? getenv("GAP") : "300");
    uint32_t reads = (uint32_t)atoi(getenv("READS") ? getenv("READS") : "1");
    uint32_t maxit = 512 / nt; /* trace_kernel.c's MAXIT */
    static uint32_t stamps[512 * 4];
    uint32_t t, i;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/trace_dpu", NULL));
    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_to(dpu, "ITERS", 0, &iters, sizeof(iters)));
        DPU_ASSERT(dpu_copy_to(dpu, "SIZE", 0, &size, sizeof(size)));
        DPU_ASSERT(dpu_copy_to(dpu, "GAP", 0, &gap, sizeof(gap)));
        DPU_ASSERT(dpu_copy_to(dpu, "READS", 0, &reads, sizeof(reads)));
    }
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_FOREACH (set, dpu)
        DPU_ASSERT(dpu_copy_from(dpu, "STAMPS", 0, stamps, (size_t)nt * maxit * 16));

    printf("tasklet\titer\tread_begin\tread_end\tread2_end\twrite_end\n");
    for (t = 0; t < nt; t++)
        for (i = 0; i < iters && i < maxit; i++) {
            uint32_t *st = &stamps[(t * maxit + i) * 4];
            printf("%u\t%u\t%u\t%u\t%u\t%u\n", t, i, st[0], st[1], st[2], st[3]);
        }

    DPU_ASSERT(dpu_free(set));
    return 0;
}
