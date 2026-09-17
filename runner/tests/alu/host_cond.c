/* Host for the condition and load/store conformance kernel. */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NPAIR 20
#define NSUB 22
#define NADD 14
#define NMEM 8

static const char *const SUB_NAME[NSUB] = {
    "z", "nz", "c", "nc", "ov", "nov", "pl", "mi", "eq", "neq", "spl",
    "smi", "ltu", "leu", "gtu", "geu", "lts", "les", "gts", "ges", "xz", "xnz"};
static const char *const ADD_NAME[NADD] = {
    "z", "nz", "c", "nc", "ov", "nov", "pl", "mi", "sz", "snz", "spl", "smi",
    "xz", "xnz"};

static const uint32_t A[NPAIR] = {
    0x00000000, 0x00000001, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x80000000,
    0x7FFFFFFF, 0x00000005, 0x00000005, 0xFFFFFFFB, 0xFFFFFFFB, 0x80000001,
    0x00001000, 0xFFFF0000, 0x0000FFFF, 0x12345678, 0xDEADBEEF, 0x00000002,
    0xCAFEBABE, 0x40000000};
static const uint32_t B[NPAIR] = {
    0x00000000, 0x00000000, 0xFFFFFFFF, 0x00000001, 0x00000001, 0xFFFFFFFF,
    0x80000000, 0x00000005, 0x00000006, 0xFFFFFFFA, 0x00000005, 0x7FFFFFFF,
    0x00002000, 0x0000FFFF, 0xFFFF0000, 0x87654321, 0x0000001F, 0xFFFFFFFF,
    0x00000007, 0xC0000000};

static const uint32_t MEM[NMEM] = {0x00000000, 0x000000FF, 0x0000FF00,
                                   0x00007F80, 0x0000FFFF, 0x12345678,
                                   0xDEADBEEF, 0x80808080};

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t subc[NSUB * NPAIR], addc[NADD * NPAIR];
    uint32_t lbu[NMEM], lbs[NMEM], lhu[NMEM], lhs[NMEM];
    uint32_t esb[NMEM], esh[NMEM], eub[NMEM], euh[NMEM], sbsh[NMEM];
    uint32_t mramo[NMEM];

    const char *bin = getenv("COND_DPU_BIN");
    if (bin == NULL) bin = "/tmp/cond_dpu";

    DPU_ASSERT(dpu_alloc(1, getenv("ALU_PROFILE"), &set));
    DPU_ASSERT(dpu_load(set, bin, NULL));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_to(dpu, "OPA", 0, A, sizeof(A)));
        DPU_ASSERT(dpu_copy_to(dpu, "OPB", 0, B, sizeof(B)));
        DPU_ASSERT(dpu_copy_to(dpu, "MEMIN", 0, MEM, sizeof(MEM)));
    }

    DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));

    DPU_FOREACH (set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, "SUBC", 0, subc, sizeof(subc)));
        DPU_ASSERT(dpu_copy_from(dpu, "ADDC", 0, addc, sizeof(addc)));
        DPU_ASSERT(dpu_copy_from(dpu, "LBU", 0, lbu, sizeof(lbu)));
        DPU_ASSERT(dpu_copy_from(dpu, "LBS", 0, lbs, sizeof(lbs)));
        DPU_ASSERT(dpu_copy_from(dpu, "LHU", 0, lhu, sizeof(lhu)));
        DPU_ASSERT(dpu_copy_from(dpu, "LHS", 0, lhs, sizeof(lhs)));
        DPU_ASSERT(dpu_copy_from(dpu, "EXTSB", 0, esb, sizeof(esb)));
        DPU_ASSERT(dpu_copy_from(dpu, "EXTSH", 0, esh, sizeof(esh)));
        DPU_ASSERT(dpu_copy_from(dpu, "EXTUB", 0, eub, sizeof(eub)));
        DPU_ASSERT(dpu_copy_from(dpu, "EXTUH", 0, euh, sizeof(euh)));
        DPU_ASSERT(dpu_copy_from(dpu, "SBSH", 0, sbsh, sizeof(sbsh)));
        DPU_ASSERT(dpu_copy_from(dpu, "MRAM_OUT", 0, mramo, sizeof(mramo)));
    }

    for (int c = 0; c < NSUB; c++)
        for (int i = 0; i < NPAIR; i++)
            printf("sub.%-4s 0x%08x 0x%08x -> %u\n", SUB_NAME[c], A[i], B[i],
                   subc[c * NPAIR + i]);

    for (int c = 0; c < NADD; c++)
        for (int i = 0; i < NPAIR; i++)
            printf("add.%-4s 0x%08x 0x%08x -> %u\n", ADD_NAME[c], A[i], B[i],
                   addc[c * NPAIR + i]);

    for (int i = 0; i < NMEM; i++)
        printf("mem 0x%08x lbu=0x%08x lbs=0x%08x lhu=0x%08x lhs=0x%08x "
               "extsb=0x%08x extsh=0x%08x extub=0x%08x extuh=0x%08x "
               "sbsh=0x%08x mram=0x%08x\n",
               MEM[i], lbu[i], lbs[i], lhu[i], lhs[i], esb[i], esh[i], eub[i],
               euh[i], sbsh[i], mramo[i]);

    DPU_ASSERT(dpu_free(set));
    return 0;
}
