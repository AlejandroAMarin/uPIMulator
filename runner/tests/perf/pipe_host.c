/* Drives pipe_kernel.c and prints cycles per instruction for each form. The
 * counter is DPU-wide, so tasklet 0's reading already covers every tasklet's
 * work; do not sum over tasklets. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NCASES 37

static const char *const NAMES[NCASES] = {
    "add r4,r4,1", "lsl r4,r5,13", "lsr r4,r5,17", "xor r5,r4,r5",
    "lsl r6,r5,13 + xor r5,r6,r5", "jump to next", "jeq taken, to next",
    "jneq not taken", "scatter loop, straight-line", "scatter loop, no jump",
    "add r8,r8,r10", "jneq-free jeq r4,r11 not taken", "9 without jeq r4,r11",
    "9 without add r8,r8,r10", "xorshift alone", "9 without xorshift",
    "jeq r7,0 + jneq r9,0", "move r11,-1",
    "add r5,r5,1 / add r4,r6,r8", "move r4,1 / add r5,r7,r9",
    "add r4,r4,1 / add r5,r6,r8", "lw r4 / add r5,r6,r8",
    "ld d4 / add r6,r7,r9", "acquire + release", "call + return", "lbu r4,r6,0",
    "failed acquire, falls through", "failed acquire, jumps over one",
    "mem_alloc path, no lock", "add nc over fault + lbu", "call + sd d10 + ret",
    "call + sd + ld d10 + ret", "lw heap + jeq", "mem_alloc path, with lock",
    "acquire, call, ret, release", "nolock body, r0 r1 r2", "nolock body, r4 r5 r6",
};

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t loops = (uint32_t)atoi(getenv("LOOPS") ? getenv("LOOPS") : "2000");
    uint32_t cyc[NCASES] = {0}, ins[NCASES] = {0};
    int k;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/pp_dpu", NULL));

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
