/* Drives lock_kernel.c: prints, for each CALLS value, when each tasklet
 * finished, counted from tasklet 0's start. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t nt = (uint32_t)atoi(getenv("TASKLETS") ? getenv("TASKLETS") : "16");
    static const uint32_t CALLS[] = {2, 64};
    uint32_t done[24], start, c, t;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/lock_dpu", NULL));
    printf("calls\ttasklet\tdone\n");
    for (c = 0; c < 2; c++) {
        DPU_FOREACH (set, dpu) DPU_ASSERT(dpu_copy_to(dpu, "CALLS", 0, &CALLS[c], 4));
        DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
        DPU_FOREACH (set, dpu) {
            DPU_ASSERT(dpu_copy_from(dpu, "START", 0, &start, 4));
            DPU_ASSERT(dpu_copy_from(dpu, "DONE", 0, done, (size_t)nt * 4));
        }
        for (t = 0; t < nt; t++) printf("%u\t%u\t%d\n", CALLS[c], t, (int)(done[t] - start));
    }
    DPU_ASSERT(dpu_free(set));
    return 0;
}
