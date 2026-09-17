/* ALU conformance kernel.
 *
 * Runs every register-register ALU operation, and the extended compare
 * conditions, over a fixed operand table. The same binary source runs on
 * UPMEM's own functional simulator and on uPIMulator, and run.sh diffs the two.
 * Inline assembly is deliberate: it pins the exact mnemonic under test instead
 * of whatever the compiler would choose for a C expression.
 */
#include <stdint.h>
#include <defs.h>

#define NPAIR 16
#define NOP   26

__host uint32_t OPA[NPAIR];
__host uint32_t OPB[NPAIR];
__host uint32_t RES[NOP * NPAIR];

/* 64-bit compare operands, and one result word per condition. */
#define NCMP 12
__host uint32_t AH[NCMP], AL[NCMP], BH[NCMP], BL[NCMP];
__host uint32_t R_XGTS[NCMP], R_XLES[NCMP], R_XGTU[NCMP], R_XLEU[NCMP];
__host uint32_t R_XZ[NCMP], R_XNZ[NCMP];

/* 64-bit add and subtract, to exercise the carry chain itself. */
__host uint32_t R_ADD64H[NCMP], R_ADD64L[NCMP];
__host uint32_t R_SUB64H[NCMP], R_SUB64L[NCMP];

#define RUN(idx, op)                                                          \
    do {                                                                      \
        uint32_t d = 0;                                                       \
        __asm__ volatile(#op " %[d], %[a], %[b]"                              \
                         : [d] "=r"(d)                                        \
                         : [a] "r"(a), [b] "r"(b));                           \
        RES[(idx) * NPAIR + i] = d;                                           \
    } while (0)

#define PROBE(cond, out)                                                      \
    do {                                                                      \
        uint32_t t = 0;                                                       \
        __asm__ volatile("sub zero, %[al], %[bl]\n"                           \
                         "subc zero, %[ah], %[bh], " cond ", 1f\n"            \
                         "jump 2f\n"                                          \
                         "1:\n"                                               \
                         "move %[t], 1\n"                                     \
                         "2:\n"                                               \
                         : [t] "+r"(t)                                        \
                         : [ah] "r"(ah), [al] "r"(al), [bh] "r"(bh),          \
                           [bl] "r"(bl));                                     \
        out[i] = t;                                                           \
    } while (0)

int main(void)
{
    for (unsigned i = 0; i < NPAIR; i++) {
        uint32_t a = OPA[i], b = OPB[i];

        RUN(0, and);
        RUN(1, andn);
        RUN(2, nand);
        RUN(3, nor);
        RUN(4, nxor);
        RUN(5, or);
        RUN(6, orn);
        RUN(7, xor);
        RUN(8, add);
        RUN(9, sub);
        RUN(10, rsub);
        RUN(11, lsl);
        RUN(12, lslx);
        RUN(13, lsr);
        RUN(14, lsrx);
        RUN(15, asr);
        RUN(16, rol);
        RUN(17, ror);
        RUN(18, lsl1);
        RUN(19, lsl1x);
        RUN(20, lsr1);
        RUN(21, lsr1x);
        RUN(22, cmpb4);
        RUN(23, mul_ul_ul);
        RUN(24, mul_sl_sl);
        RUN(25, mul_uh_ul);
    }

    for (unsigned i = 0; i < NCMP; i++) {
        uint32_t ah = AH[i], al = AL[i], bh = BH[i], bl = BL[i];
        uint32_t rh = 0, rl = 0;

        PROBE("xgts", R_XGTS);
        PROBE("xles", R_XLES);
        PROBE("xgtu", R_XGTU);
        PROBE("xleu", R_XLEU);
        PROBE("xz", R_XZ);
        PROBE("xnz", R_XNZ);

        __asm__ volatile("add %[rl], %[al], %[bl]\n"
                         "addc %[rh], %[ah], %[bh]\n"
                         : [rh] "=&r"(rh), [rl] "=&r"(rl)
                         : [ah] "r"(ah), [al] "r"(al), [bh] "r"(bh),
                           [bl] "r"(bl));
        R_ADD64H[i] = rh;
        R_ADD64L[i] = rl;

        __asm__ volatile("sub %[rl], %[al], %[bl]\n"
                         "subc %[rh], %[ah], %[bh]\n"
                         : [rh] "=&r"(rh), [rl] "=&r"(rl)
                         : [ah] "r"(ah), [al] "r"(al), [bh] "r"(bh),
                           [bl] "r"(bl));
        R_SUB64H[i] = rh;
        R_SUB64L[i] = rl;
    }

    return 0;
}
