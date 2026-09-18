/* Drives mram_kernel.c over a sweep of DMA sizes and access patterns. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const uint32_t SIZES[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048};
#define NSIZES (sizeof(SIZES) / sizeof(SIZES[0]))

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t iters = (uint32_t)atoi(getenv("ITERS") ? getenv("ITERS") : "2000");
    uint32_t nt = (uint32_t)atoi(getenv("TASKLETS") ? getenv("TASKLETS") : "1");
    uint32_t do_write = (uint32_t)atoi(getenv("WRITE") ? getenv("WRITE") : "0");
    uint32_t cycles[24], instr[24];
    unsigned si;
    int pattern;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/mram_dpu", NULL));

    printf("pattern\tbytes\tcycles_per_dma\tinstr_per_dma\n");

    for (pattern = 0; pattern < 2; pattern++) {
        for (si = 0; si < NSIZES; si++) {
            uint32_t size = SIZES[si];
            /* In order, or far enough apart to miss the row buffer every time. */
            uint32_t stride = pattern == 0 ? size : 65536;
            uint32_t t;
            double c = 0, n = 0;

            DPU_FOREACH (set, dpu) {
                DPU_ASSERT(dpu_copy_to(dpu, "SIZE", 0, &size, sizeof(size)));
                DPU_ASSERT(dpu_copy_to(dpu, "ITERS", 0, &iters, sizeof(iters)));
                DPU_ASSERT(dpu_copy_to(dpu, "STRIDE", 0, &stride, sizeof(stride)));
                DPU_ASSERT(dpu_copy_to(dpu, "DO_WRITE", 0, &do_write, sizeof(do_write)));
            }
            DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
            DPU_FOREACH (set, dpu) {
                DPU_ASSERT(dpu_copy_from(dpu, "OUT_CYCLES", 0, cycles, (size_t)nt * 4));
                DPU_ASSERT(dpu_copy_from(dpu, "OUT_INSTR", 0, instr, (size_t)nt * 4));
            }

            /* The counter is DPU-wide, so every tasklet reports the same span;
             * take the largest and divide by the DMAs the whole DPU issued. */
            for (t = 0; t < nt; t++) {
                if (cycles[t] > c) c = cycles[t];
                if (instr[t] > n) n = instr[t];
            }

            printf("%s\t%u\t%.3f\t%.3f\n", pattern == 0 ? "sequential" : "scattered",
                   size, c / ((double)iters * nt), n / ((double)iters * nt));
        }
    }

    DPU_ASSERT(dpu_free(set));
    return 0;
}
