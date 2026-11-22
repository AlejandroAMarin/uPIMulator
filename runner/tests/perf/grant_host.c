/* Drives grant_kernel.c. Prints each tasklet's barrier departure counted from
 * tasklet 0's, the order the bit was granted in, and each grant's time from
 * the release. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t nt = (uint32_t)atoi(getenv("TASKLETS") ? getenv("TASKLETS") : "16");
    uint32_t delay = (uint32_t)atoi(getenv("DELAY") ? getenv("DELAY") : "2000");
    uint32_t leave[24], grant[24], order[24], rel, t;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN"), NULL));
    DPU_FOREACH (set, dpu) DPU_ASSERT(dpu_copy_to(dpu, "DELAY", 0, &delay, 4));
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, "LEAVE", 0, leave, (size_t)nt * 4));
        DPU_ASSERT(dpu_copy_from(dpu, "GRANT", 0, grant, (size_t)nt * 4));
        DPU_ASSERT(dpu_copy_from(dpu, "ORDER", 0, order, (size_t)nt * 4));
        DPU_ASSERT(dpu_copy_from(dpu, "REL", 0, &rel, 4));
    }
    printf("leave:");
    for (t = 0; t < nt; t++) printf(" %u:%d", t, (int)(leave[t] - leave[0]));
    printf("\norder:");
    for (t = 0; t + 1 < nt; t++) printf(" %u", order[t]);
    printf("\ngrant:");
    for (t = 0; t + 1 < nt; t++) printf(" %d", (int)(grant[order[t]] - rel));
    printf("\n");
    DPU_ASSERT(dpu_free(set));
    return 0;
}
