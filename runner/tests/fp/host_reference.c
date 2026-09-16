/* Control: run the same kernel source on UPMEM's own functional simulator.
 * backend=simulator selects libdpufsim, which depends only on libc and cannot
 * reach a DPU rank. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t in = 152, c = 0, m = 0, a = 0, s = 0;

    DPU_ASSERT(dpu_alloc(1, "backend=simulator", &set));
    DPU_ASSERT(dpu_load(set, "/tmp/fptest/fp_dpu", NULL));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_to(dpu, "INPUT", 0, &in, sizeof(in)));
    }

    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_CONV", 0, &c, sizeof(c)));
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_MUL", 0, &m, sizeof(m)));
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_ADD", 0, &a, sizeof(a)));
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_SUB", 0, &s, sizeof(s)));
    }

    printf("  convert  152.0 -> %-12u expect 152 %s\n", c, c == 152 ? "" : "  WRONG");
    printf("  add    152+2.0 -> %-12u expect 154 %s\n", a, a == 154 ? "" : "  WRONG");
    printf("  sub    152-2.0 -> %-12u expect 150 %s\n", s, s == 150 ? "" : "  WRONG");
    printf("  mul    152*0.8 -> %-12u expect 121 %s\n", m, m == 121 ? "" : "  WRONG");

    DPU_ASSERT(dpu_free(set));
    return 0;
}
