/* Host for PrIM's shared-histogram kernel.
 *
 * Same shape as va_host.c: it reads the DPU's own counter rather than the
 * host's clock, because only the DPUs are simulated, and it checks the whole
 * histogram against a CPU reference before reporting a number.
 */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DPU_MRAM_HEAP_POINTER_NAME
#define DPU_MRAM_HEAP_POINTER_NAME "__sys_used_mram_end"
#endif

#define DEPTH 12 /* the kernel's, from HST-S/support/common.h */

typedef struct {
    uint32_t size;
    uint32_t transfer_size;
    uint32_t bins;
    uint32_t kernel;
} dpu_arguments_t;

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t nelem = (uint32_t)atoi(getenv("NELEM") ? getenv("NELEM") : "16384");
    uint32_t bins = (uint32_t)atoi(getenv("BINS") ? getenv("BINS") : "256");
    uint32_t mode = (uint32_t)atoi(getenv("PROBE_MODE") ? getenv("PROBE_MODE") : "0");
    uint32_t bytes = nelem * (uint32_t)sizeof(uint32_t);
    uint32_t hbytes = bins * (uint32_t)sizeof(uint32_t);
    dpu_arguments_t args;
    uint32_t *A, *want, *got;
    uint32_t count = 0;
    uint32_t i, bad = 0;

    A = malloc(bytes); want = calloc(bins, sizeof(uint32_t)); got = malloc(hbytes);
    if (!A || !want || !got) { fprintf(stderr, "out of memory\n"); return 1; }

    /* Values below 2^DEPTH, which is what the kernel's index arithmetic
     * assumes: histo[(d * bins) >> DEPTH]. */
    for (i = 0; i < nelem; i++) {
        A[i] = (i * 2654435761u) % (1u << DEPTH);
        want[(A[i] * bins) >> DEPTH]++;
    }

    args.size = bytes;
    args.transfer_size = bytes;
    args.bins = bins;
    args.kernel = 0;

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/hst_dpu", NULL));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_to(dpu, "DPU_INPUT_ARGUMENTS", 0, &args, sizeof(args)));
        DPU_ASSERT(dpu_copy_to(dpu, "PROBE_MODE", 0, &mode, sizeof(mode)));
        DPU_ASSERT(dpu_copy_to(dpu, DPU_MRAM_HEAP_POINTER_NAME, 0, A, bytes));
    }

    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, DPU_MRAM_HEAP_POINTER_NAME, bytes, got, hbytes));
        DPU_ASSERT(dpu_copy_from(dpu, "OUT_COUNT", 0, &count, sizeof(count)));
    }

    /* The histogram does not depend on its own previous output, so both passes
     * produce the same answer and both are worth checking. */
    for (i = 0; i < bins; i++)
        if (got[i] != want[i]) bad++;

    printf("elements\t%u\nbins\t%u\nmode\t%s\ncount\t%u\nmismatches\t%u\n", nelem,
           bins, mode ? "instructions" : "cycles", count, bad);

    DPU_ASSERT(dpu_free(set));
    free(A); free(want); free(got);
    return bad ? 1 : 0;
}
