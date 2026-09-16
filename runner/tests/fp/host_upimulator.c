#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
int main(void) {
    struct dpu_set_t set; uint32_t in = 152, c=0,m=0,a=0,s=0;
    DPU_ASSERT(dpu_alloc(1, NULL, &set));
    DPU_ASSERT(dpu_load(set, "x", NULL));
    DPU_ASSERT(dpu_copy_to(set, "INPUT", 0, &in, sizeof(in)));
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_ASSERT(dpu_copy_from(set, "OUT_CONV", 0, &c, sizeof(c)));
    DPU_ASSERT(dpu_copy_from(set, "OUT_MUL",  0, &m, sizeof(m)));
    DPU_ASSERT(dpu_copy_from(set, "OUT_ADD",  0, &a, sizeof(a)));
    DPU_ASSERT(dpu_copy_from(set, "OUT_SUB",  0, &s, sizeof(s)));
    printf("  convert  152.0 -> %-12u expect 152 %s\n", c, c==152?"":"  WRONG");
    printf("  add    152+2.0 -> %-12u expect 154 %s\n", a, a==154?"":"  WRONG");
    printf("  sub    152-2.0 -> %-12u expect 150 %s\n", s, s==150?"":"  WRONG");
    printf("  mul    152*0.8 -> %-12u expect 121 %s\n", m, m==121?"":"  WRONG");
    DPU_ASSERT(dpu_free(set));
    return 0;
}
