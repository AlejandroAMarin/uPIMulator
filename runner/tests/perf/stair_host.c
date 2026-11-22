/* Drives stair_kernel.c: prints m, cycles and instructions per iteration. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NCASES 25

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t loops = 200, c[NCASES], ins[NCASES], k;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN"), NULL));
    DPU_FOREACH (set, dpu) DPU_ASSERT(dpu_copy_to(dpu, "LOOPS", 0, &loops, 4));
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_CYCLES", 0, c, sizeof(c)));
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_INSTR", 0, ins, sizeof(ins)));
    }
    for (k = 0; k < NCASES; k++) printf("%u\t%.2f\t%.2f\n", k, c[k] / (double)loops, ins[k] / (double)loops);
    DPU_ASSERT(dpu_free(set));
    return 0;
}
