/* Condition-code and load/store conformance kernel.
 *
 * kernel.c covers what the ALU computes; this covers what it decides, plus the
 * narrow loads and stores. BIMSA's kernel branches on jlts/jges/jgts/jles and
 * reads sequences a byte at a time, so both paths have to match the reference
 * exactly or an alignment silently returns the wrong score.
 */
#include <stdint.h>
#include <defs.h>
#include <mram.h>

#define NPAIR 20
#define NSUB 22
#define NADD 14

__host uint32_t OPA[NPAIR];
__host uint32_t OPB[NPAIR];

/* One word per condition per operand pair. */
__host uint32_t SUBC[NSUB * NPAIR];
__host uint32_t ADDC[NADD * NPAIR];

/* Narrow loads and stores, and the sign-extension helpers. */
#define NMEM 8
__host uint32_t MEMIN[NMEM];
__host uint32_t LBU[NMEM], LBS[NMEM], LHU[NMEM], LHS[NMEM];
__host uint32_t EXTSB[NMEM], EXTSH[NMEM], EXTUB[NMEM], EXTUH[NMEM];
__host uint32_t SBSH[NMEM];

/* An MRAM round trip, which is how BIMSA moves sequences. */
__host uint32_t MRAM_OUT[NMEM];
__mram_noinit uint8_t scratch[2048];

#define SUB_PROBE(slot, cond)                                                 \
    do {                                                                      \
        uint32_t t = 0;                                                       \
        __asm__ volatile("sub zero, %[a], %[b], " cond ", 1f\n"               \
                         "jump 2f\n"                                          \
                         "1:\n"                                               \
                         "move %[t], 1\n"                                     \
                         "2:\n"                                               \
                         : [t] "+r"(t)                                        \
                         : [a] "r"(a), [b] "r"(b));                           \
        SUBC[(slot) * NPAIR + i] = t;                                         \
    } while (0)

#define ADD_PROBE(slot, cond)                                                 \
    do {                                                                      \
        uint32_t t = 0;                                                       \
        __asm__ volatile("add zero, %[a], %[b], " cond ", 1f\n"               \
                         "jump 2f\n"                                          \
                         "1:\n"                                               \
                         "move %[t], 1\n"                                     \
                         "2:\n"                                               \
                         : [t] "+r"(t)                                        \
                         : [a] "r"(a), [b] "r"(b));                           \
        ADDC[(slot) * NPAIR + i] = t;                                         \
    } while (0)

int main(void)
{
    for (unsigned i = 0; i < NPAIR; i++) {
        uint32_t a = OPA[i], b = OPB[i];

        SUB_PROBE(0, "z");
        SUB_PROBE(1, "nz");
        SUB_PROBE(2, "c");
        SUB_PROBE(3, "nc");
        SUB_PROBE(4, "ov");
        SUB_PROBE(5, "nov");
        SUB_PROBE(6, "pl");
        SUB_PROBE(7, "mi");
        SUB_PROBE(8, "eq");
        SUB_PROBE(9, "neq");
        SUB_PROBE(10, "spl");
        SUB_PROBE(11, "smi");
        SUB_PROBE(12, "ltu");
        SUB_PROBE(13, "leu");
        SUB_PROBE(14, "gtu");
        SUB_PROBE(15, "geu");
        SUB_PROBE(16, "lts");
        SUB_PROBE(17, "les");
        SUB_PROBE(18, "gts");
        SUB_PROBE(19, "ges");
        SUB_PROBE(20, "xz");
        SUB_PROBE(21, "xnz");

        ADD_PROBE(0, "z");
        ADD_PROBE(1, "nz");
        ADD_PROBE(2, "c");
        ADD_PROBE(3, "nc");
        ADD_PROBE(4, "ov");
        ADD_PROBE(5, "nov");
        ADD_PROBE(6, "pl");
        ADD_PROBE(7, "mi");
        ADD_PROBE(8, "sz");
        ADD_PROBE(9, "snz");
        ADD_PROBE(10, "spl");
        ADD_PROBE(11, "smi");
        ADD_PROBE(12, "xz");
        ADD_PROBE(13, "xnz");
    }

    /* Narrow loads read back what the matching narrow store wrote, so a fault
     * in either shows up here. */
    for (unsigned i = 0; i < NMEM; i++) {
        volatile uint32_t cell = MEMIN[i];
        volatile uint8_t *bp = (volatile uint8_t *)&cell;
        volatile int8_t *sbp = (volatile int8_t *)&cell;
        volatile uint16_t *hp = (volatile uint16_t *)&cell;
        volatile int16_t *shp = (volatile int16_t *)&cell;

        LBU[i] = bp[0];
        LBS[i] = (uint32_t)(int32_t)sbp[0];
        LHU[i] = hp[0];
        LHS[i] = (uint32_t)(int32_t)shp[0];

        uint32_t w = MEMIN[i], r;
        __asm__ volatile("extsb %[r], %[w]" : [r] "=r"(r) : [w] "r"(w));
        EXTSB[i] = r;
        __asm__ volatile("extsh %[r], %[w]" : [r] "=r"(r) : [w] "r"(w));
        EXTSH[i] = r;
        __asm__ volatile("extub %[r], %[w]" : [r] "=r"(r) : [w] "r"(w));
        EXTUB[i] = r;
        __asm__ volatile("extuh %[r], %[w]" : [r] "=r"(r) : [w] "r"(w));
        EXTUH[i] = r;

        /* A byte store then a half store into the same word. */
        volatile uint32_t packed = 0;
        volatile uint8_t *pb = (volatile uint8_t *)&packed;
        volatile uint16_t *ph = (volatile uint16_t *)&packed;
        pb[3] = (uint8_t)MEMIN[i];
        ph[0] = (uint16_t)(MEMIN[i] >> 8);
        SBSH[i] = packed;
    }

    /* MRAM round trip: write the table out and read it back. */
    uint32_t buf[NMEM];
    for (unsigned i = 0; i < NMEM; i++) buf[i] = MEMIN[i] ^ 0xA5A5A5A5u;
    mram_write(buf, scratch, NMEM * sizeof(uint32_t));
    for (unsigned i = 0; i < NMEM; i++) buf[i] = 0;
    mram_read(scratch, buf, NMEM * sizeof(uint32_t));
    for (unsigned i = 0; i < NMEM; i++) MRAM_OUT[i] = buf[i];

    return 0;
}
