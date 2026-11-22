/* Drives wram_kernel.c over foreground and background modes and prints the
 * foreground loop's cycles per instruction. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t loops = (uint32_t)atoi(getenv("LOOPS") ? getenv("LOOPS") : "20000");
    uint32_t fg, bg, cyc = 0, ins = 0, tr = 0;
    static const char *const FGN[] = {"wram", "alu"}, *const BGN[] = {"none", "read", "write"};

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/wram_dpu", NULL));
    printf("fg\tbg\tcycles_per_instr\tinstr\tbg_transfers\n");
    for (fg = 0; fg < 2; fg++)
        for (bg = 0; bg < 3; bg++) {
            DPU_FOREACH (set, dpu) {
                DPU_ASSERT(dpu_copy_to(dpu, "LOOPS", 0, &loops, sizeof(loops)));
                DPU_ASSERT(dpu_copy_to(dpu, "FG", 0, &fg, sizeof(fg)));
                DPU_ASSERT(dpu_copy_to(dpu, "BG", 0, &bg, sizeof(bg)));
            }
            DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
            DPU_FOREACH (set, dpu) {
                DPU_ASSERT(dpu_copy_from(dpu, "OUT_CYCLES", 0, &cyc, 4));
                DPU_ASSERT(dpu_copy_from(dpu, "OUT_INSTR", 0, &ins, 4));
                DPU_ASSERT(dpu_copy_from(dpu, "OUT_TRANSFERS", 0, &tr, 4));
            }
            printf("%s\t%s\t%.4f\t%u\t%u\n", FGN[fg], BGN[bg], ins ? (double)cyc / ins : 0.0, ins, tr);
        }
    DPU_ASSERT(dpu_free(set));
    return 0;
}
