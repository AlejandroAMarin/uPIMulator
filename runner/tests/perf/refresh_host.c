/* Drives refresh_kernel.c and prints each read's two stamps. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t delay = (uint32_t)atoi(getenv("DELAY") ? getenv("DELAY") : "51");
    uint32_t size = (uint32_t)atoi(getenv("SIZE") ? getenv("SIZE") : "8");
    uint32_t t0[512], t1[512], i;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN"), NULL));
    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_to(dpu, "DELAY", 0, &delay, 4));
        DPU_ASSERT(dpu_copy_to(dpu, "SIZE", 0, &size, 4));
    }
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, "T0", 0, t0, sizeof(t0)));
        DPU_ASSERT(dpu_copy_from(dpu, "T1", 0, t1, sizeof(t1)));
    }
    for (i = 0; i < 512; i++) printf("%u\t%u\n", t0[i], t1[i]);
    DPU_ASSERT(dpu_free(set));
    return 0;
}
