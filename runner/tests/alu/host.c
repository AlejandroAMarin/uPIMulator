/* Host for the ALU conformance kernel. Prints one line per result so the two
 * backends can be compared with diff. DPU_BIN and the profile come from the
 * environment, so the same binary drives both. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NPAIR 16
#define NOP 26
#define NCMP 12

static const char *const OP_NAME[NOP] = {
    "and", "andn", "nand", "nor", "nxor", "or", "orn", "xor", "add",
    "sub", "rsub", "lsl", "lslx", "lsr", "lsrx", "asr", "rol", "ror",
    "lsl1", "lsl1x", "lsr1", "lsr1x", "cmpb4", "mul_ul_ul", "mul_sl_sl",
    "mul_uh_ul"};

static const uint32_t A[NPAIR] = {
    0x00000000, 0xFFFFFFFF, 0x00130000, 0x80000000, 0x7FFFFFFF, 0x0000000F,
    0xDEADBEEF, 0x00000098, 0x12345678, 0xFFFF0000, 0x00000001, 0xAAAAAAAA,
    0x55555555, 0x80000001, 0x0000FFFF, 0xCAFEBABE};
static const uint32_t B[NPAIR] = {
    0x00000000, 0xFFFFFFFF, 0x00100000, 0x00000001, 0x00000001, 0x0000002D,
    0x0000001F, 0x0000002D, 0x00000020, 0x00000040, 0xFFFFFFFF, 0x55555555,
    0xAAAAAAAA, 0x00000000, 0x00000010, 0x00000007};

static const uint64_t CA[NCMP] = {
    0x0063000000000000ULL, 0x8063000000000000ULL, 0x0000000000000000ULL,
    0xFFFFFFFFFFFFFFFFULL, 0x0000000000000001ULL, 0x8000000000000000ULL,
    0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0x0000000100000000ULL,
    0x0000000000000005ULL, 0x8000000000000000ULL, 0x0000000000000000ULL};
static const uint64_t CB[NCMP] = {
    0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0x0000000000000000ULL,
    0xFFFFFFFFFFFFFFFFULL, 0x0000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
    0x8000000000000000ULL, 0x0000000000000000ULL, 0x00000000FFFFFFFFULL,
    0x0000000000000005ULL, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL};

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t res[NOP * NPAIR];
    uint32_t ah[NCMP], al[NCMP], bh[NCMP], bl[NCMP];
    uint32_t gts[NCMP], les[NCMP], gtu[NCMP], leu[NCMP], xz[NCMP], xnz[NCMP];
    uint32_t addh[NCMP], addl[NCMP], subh[NCMP], subl[NCMP];

    for (int i = 0; i < NCMP; i++) {
        ah[i] = (uint32_t)(CA[i] >> 32); al[i] = (uint32_t)CA[i];
        bh[i] = (uint32_t)(CB[i] >> 32); bl[i] = (uint32_t)CB[i];
    }

    const char *bin = getenv("ALU_DPU_BIN");
    if (bin == NULL) bin = "/tmp/alu_dpu";

    DPU_ASSERT(dpu_alloc(1, getenv("ALU_PROFILE"), &set));
    DPU_ASSERT(dpu_load(set, bin, NULL));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_to(dpu, "OPA", 0, A, sizeof(A)));
        DPU_ASSERT(dpu_copy_to(dpu, "OPB", 0, B, sizeof(B)));
        DPU_ASSERT(dpu_copy_to(dpu, "AH", 0, ah, sizeof(ah)));
        DPU_ASSERT(dpu_copy_to(dpu, "AL", 0, al, sizeof(al)));
        DPU_ASSERT(dpu_copy_to(dpu, "BH", 0, bh, sizeof(bh)));
        DPU_ASSERT(dpu_copy_to(dpu, "BL", 0, bl, sizeof(bl)));
    }

    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, "RES", 0, res, sizeof(res)));
        DPU_ASSERT(dpu_copy_from(dpu, "R_XGTS", 0, gts, sizeof(gts)));
        DPU_ASSERT(dpu_copy_from(dpu, "R_XLES", 0, les, sizeof(les)));
        DPU_ASSERT(dpu_copy_from(dpu, "R_XGTU", 0, gtu, sizeof(gtu)));
        DPU_ASSERT(dpu_copy_from(dpu, "R_XLEU", 0, leu, sizeof(leu)));
        DPU_ASSERT(dpu_copy_from(dpu, "R_XZ", 0, xz, sizeof(xz)));
        DPU_ASSERT(dpu_copy_from(dpu, "R_XNZ", 0, xnz, sizeof(xnz)));
        DPU_ASSERT(dpu_copy_from(dpu, "R_ADD64H", 0, addh, sizeof(addh)));
        DPU_ASSERT(dpu_copy_from(dpu, "R_ADD64L", 0, addl, sizeof(addl)));
        DPU_ASSERT(dpu_copy_from(dpu, "R_SUB64H", 0, subh, sizeof(subh)));
        DPU_ASSERT(dpu_copy_from(dpu, "R_SUB64L", 0, subl, sizeof(subl)));
    }

    for (int o = 0; o < NOP; o++)
        for (int i = 0; i < NPAIR; i++)
            printf("%-10s 0x%08x 0x%08x -> 0x%08x\n", OP_NAME[o], A[i], B[i],
                   res[o * NPAIR + i]);

    for (int i = 0; i < NCMP; i++)
        printf("cmp        %d xgts=%u xles=%u xgtu=%u xleu=%u xz=%u xnz=%u\n",
               i, gts[i], les[i], gtu[i], leu[i], xz[i], xnz[i]);

    for (int i = 0; i < NCMP; i++)
        printf("wide       %d add=0x%08x%08x sub=0x%08x%08x\n", i, addh[i],
               addl[i], subh[i], subl[i]);

    DPU_ASSERT(dpu_free(set));
    return 0;
}
