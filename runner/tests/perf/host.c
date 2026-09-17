/* Host for the performance-counter calibration kernel. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t loops = (uint32_t)atoi(getenv("LOOPS") ? getenv("LOOPS") : "1000");
    int nt = atoi(getenv("TASKLETS") ? getenv("TASKLETS") : "1");
    uint32_t instr[24] = {0}, cycles[24] = {0}, sink[24] = {0};
    int t;

    DPU_ASSERT(dpu_alloc(1, getenv("PERF_PROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("PERF_BIN") ? getenv("PERF_BIN") : "/tmp/perf_dpu", NULL));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_to(dpu, "LOOPS", 0, &loops, sizeof(loops)));
    }

    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_INSTR", 0, instr, (size_t)nt * 4));
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_CYCLES", 0, cycles, (size_t)nt * 4));
        DPU_ASSERT(dpu_copy_from(dpu, "SINK", 0, sink, (size_t)nt * 4));
    }

    printf("loops=%u tasklets=%d\n", loops, nt);
    for (t = 0; t < nt; t++)
        printf("  tasklet %2d instr=%8u cycles=%9u sink=%u\n", t, instr[t],
               cycles[t], sink[t]);

    DPU_ASSERT(dpu_free(set));
    return 0;
}
