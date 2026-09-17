#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    struct dpu_set_t set, dpu;
    uint32_t src[2] = {0x11112222, 0x33334444}, out[16];
    uint32_t off = (uint32_t)atoi(getenv("OFF") ? getenv("OFF") : "0");
    uint32_t woff = (uint32_t)atoi(getenv("WOFF") ? getenv("WOFF") : "0");
    DPU_ASSERT(dpu_alloc(1, getenv("MP"), &set));
    DPU_ASSERT(dpu_load(set, getenv("MB") ? getenv("MB") : "/tmp/mramtest/m_dpu", NULL));
    DPU_FOREACH(set, dpu) {
        DPU_ASSERT(dpu_copy_to(dpu, "SRC", 0, src, sizeof(src)));
        DPU_ASSERT(dpu_copy_to(dpu, "OFFSET", 0, &off, sizeof(off)));
        DPU_ASSERT(dpu_copy_to(dpu, "WOFFSET", 0, &woff, sizeof(woff)));
    }
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_FOREACH(set, dpu) DPU_ASSERT(dpu_copy_from(dpu, "OUT", 0, out, sizeof(out)));
    printf("mram=%2u wram=%u:", off, woff);
    for (int i = 0; i < 8; i++) printf(" %08x", out[i]);
    printf("\n");
    DPU_ASSERT(dpu_free(set));
    return 0;
}
