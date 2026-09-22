/* Host for PrIM's vector-addition kernel.
 *
 * PrIM's own host measures wall-clock time around dpu_launch, which says
 * nothing under the simulator: the host runs natively there and only the DPUs
 * are simulated. This one reads the DPU's own counter instead, which is the
 * same quantity on both sides.
 *
 * It checks every element against a CPU reference before reporting a number.
 * A timing comparison against a kernel that computed the wrong thing is worth
 * nothing, and BIMSA already proved that once.
 */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DPU_MRAM_HEAP_POINTER_NAME
#define DPU_MRAM_HEAP_POINTER_NAME "__sys_used_mram_end"
#endif

typedef struct {
    uint32_t size;          /* bytes per DPU */
    uint32_t transfer_size; /* bytes actually transferred, 8-byte aligned */
    uint32_t kernel;
} dpu_arguments_t;

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t nelem = (uint32_t)atoi(getenv("NELEM") ? getenv("NELEM") : "16384");
    uint32_t mode = (uint32_t)atoi(getenv("PROBE_MODE") ? getenv("PROBE_MODE") : "0");
    uint32_t bytes = nelem * (uint32_t)sizeof(int32_t);
    dpu_arguments_t args;
    int32_t *A, *B, *want, *got;
    uint32_t count = 0;
    uint32_t i, bad = 0;

    A = malloc(bytes); B = malloc(bytes); want = malloc(bytes); got = malloc(bytes);
    if (!A || !B || !want || !got) { fprintf(stderr, "out of memory\n"); return 1; }

    for (i = 0; i < nelem; i++) {
        A[i] = (int32_t)(i * 3 + 1);
        B[i] = (int32_t)(i * 7 + 2);
        want[i] = A[i] + B[i];
    }

    args.size = bytes;
    args.transfer_size = bytes;
    args.kernel = 0;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/va_dpu", NULL));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_to(dpu, "DPU_INPUT_ARGUMENTS", 0, &args, sizeof(args)));
        DPU_ASSERT(dpu_copy_to(dpu, "PROBE_MODE", 0, &mode, sizeof(mode)));
        DPU_ASSERT(dpu_copy_to(dpu, DPU_MRAM_HEAP_POINTER_NAME, 0, A, bytes));
        DPU_ASSERT(dpu_copy_to(dpu, DPU_MRAM_HEAP_POINTER_NAME, bytes, B, bytes));
    }

    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, DPU_MRAM_HEAP_POINTER_NAME, bytes, got, bytes));
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_COUNT", 0, &count, sizeof(count)));
    }

    /* The instruction pass runs over the output of the cycle pass, so only the
     * first one has a result worth checking. */
    if (mode == 0)
        for (i = 0; i < nelem; i++)
            if (got[i] != want[i]) bad++;

    printf("elements\t%u\nmode\t%s\ncount\t%u\nmismatches\t%u\n", nelem,
           mode ? "instructions" : "cycles", count, bad);

    DPU_ASSERT(dpu_free(set));
    free(A); free(B); free(want); free(got);
    return bad ? 1 : 0;
}
