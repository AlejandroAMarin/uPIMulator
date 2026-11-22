/* Drives wake_kernel.c and prints each tasklet's arrival, departure and end,
 * counted from the last arrival. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t nt = (uint32_t)atoi(getenv("TASKLETS") ? getenv("TASKLETS") : "16");
    uint32_t delay = 2000, work = (uint32_t)atoi(getenv("WORK") ? getenv("WORK") : "0");
    uint32_t arrive[24], leave[24], done[24], last = 0, t;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/wake_dpu", NULL));
    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_to(dpu, "DELAY", 0, &delay, 4));
        DPU_ASSERT(dpu_copy_to(dpu, "WORK", 0, &work, 4));
    }
    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, "ARRIVE", 0, arrive, (size_t)nt * 4));
        DPU_ASSERT(dpu_copy_from(dpu, "LEAVE", 0, leave, (size_t)nt * 4));
        DPU_ASSERT(dpu_copy_from(dpu, "DONE", 0, done, (size_t)nt * 4));
    }
    for (t = 0; t < nt; t++) if (arrive[t] > last) last = arrive[t];
    printf("tasklet\tleave\tdone\n");
    for (t = 0; t < nt; t++) printf("%u\t%d\t%d\n", t, (int)(leave[t] - last), (int)(done[t] - last));
    DPU_ASSERT(dpu_free(set));
    return 0;
}
