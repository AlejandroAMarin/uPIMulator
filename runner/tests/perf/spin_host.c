/* Drives spin_kernel.c: for each hold length, the window in cycles and in
 * instructions, and the same for tasklet 1's loop alone. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct dpu_set_t set, dpu;
    static const uint32_t HOLDS[] = {100, 1000};
    uint32_t h, mode, spin[2], alone[2];

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/spin_dpu", NULL));
    printf("hold\twindow_cycles\twindow_instr\talone_cycles\talone_instr\n");
    for (h = 0; h < 2; h++) {
        for (mode = 0; mode < 2; mode++) {
            DPU_FOREACH (set, dpu) {
                DPU_ASSERT(dpu_copy_to(dpu, "HOLD", 0, &HOLDS[h], 4));
                DPU_ASSERT(dpu_copy_to(dpu, "MODE", 0, &mode, 4));
            }
            DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
            DPU_FOREACH (set, dpu) {
                DPU_ASSERT(dpu_copy_from(dpu, "OUT_SPIN", 0, &spin[mode], 4));
                DPU_ASSERT(dpu_copy_from(dpu, "OUT_ALONE", 0, &alone[mode], 4));
            }
        }
        printf("%u\t%u\t%u\t%u\t%u\n", HOLDS[h], spin[0], spin[1], alone[0], alone[1]);
    }
    DPU_ASSERT(dpu_free(set));
    return 0;
}
