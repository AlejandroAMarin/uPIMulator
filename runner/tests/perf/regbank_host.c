/* Drives regbank_kernel.c and prints cycles per instruction for each register
 * layout. The counter is DPU-wide, so tasklet 0's reading already covers every
 * tasklet's work; do not sum over tasklets. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NCASES 17

static const char *const NAMES[NCASES] = {
    "add r4,r4,r5   read e+o",   "add r4,r6,r8   read e+e",
    "add r4,r5,r7   read o+o",   "add r4,r5,r6   read o+e, write e",
    "add r5,r4,r6   read e+e, write o", "add r4,r4,1    read e",
    "add r4,r6,1    read e, write other e", "sub r10,r6,r8  read e+e, write third e",
    "add r5,r7,r9   three odd", "add r6,r7,r9   read o+o, write e",
    "add r5,r6,r7   read e+o, write o",
    "lw  r4,r6,0    32-bit load", "ld  d4,r6,0    64-bit load",
    "ld  d8,r6,0    64-bit load, other pair", "sw  r6,0,r4    32-bit store",
    "sd  r6,0,d4    64-bit store", "lsl_add r4,r6,r8  three even",
};

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t loops = (uint32_t)atoi(getenv("LOOPS") ? getenv("LOOPS") : "2000");
    uint32_t nt = (uint32_t)atoi(getenv("TASKLETS") ? getenv("TASKLETS") : "1");
    uint32_t cyc[NCASES] = {0}, ins[NCASES] = {0};
    int k;

    (void)nt;
    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/rb_dpu", NULL));

    DPU_FOREACH (set, dpu)
        DPU_ASSERT(dpu_copy_to(dpu, "LOOPS", 0, &loops, sizeof(loops)));
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_CYCLES", 0, cyc, sizeof(cyc)));
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_INSTR", 0, ins, sizeof(ins)));
    }

    printf("case\tcyc_per_instr\tinstr\tdescription\n");
    for (k = 0; k < NCASES; k++)
        printf("%d\t%.4f\t%u\t%s\n", k, ins[k] ? (double)cyc[k] / ins[k] : 0.0,
               ins[k], NAMES[k]);

    DPU_ASSERT(dpu_free(set));
    return 0;
}
