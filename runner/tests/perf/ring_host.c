/* Drives ring_kernel.c and prints the order the bit was granted in. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t nt = (uint32_t)atoi(getenv("TASKLETS") ? getenv("TASKLETS") : "16");
    uint32_t delay = (uint32_t)atoi(getenv("DELAY") ? getenv("DELAY") : "500");
    uint32_t order[24], t;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN"), NULL));
    DPU_FOREACH (set, dpu) DPU_ASSERT(dpu_copy_to(dpu, "DELAY", 0, &delay, 4));
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_FOREACH (set, dpu) DPU_ASSERT(dpu_copy_from(dpu, "ORDER", 0, order, (size_t)nt * 4));
    printf("order:");
    for (t = 0; t + 1 < nt; t++) printf(" %u", order[t]);
    printf("\n");
    DPU_ASSERT(dpu_free(set));
    return 0;
}
