/* A loop of random 16 B reads with m more instructions in it, m = 0..24.
 *
 * One tasklet. Each case is the same asm loop, an xorshift address and an
 * ldma, with m adds after the ldma. Cycles per iteration should grow by 11
 * per instruction. On the machine they grow in steps instead: the loop locks
 * to the MRAM refresh, so its period sits on 2736 / k for a range of m
 * (195.4, 210.5, 228, 248.7, 273.6, 304, 342, 391, 456). A model whose read
 * latency is a few cycles off locks at other m. The instructions count the
 * same before the ldma as after it.
 */
#include <stdint.h>
#include <defs.h>
#include <perfcounter.h>

#define NCASES 25
__host uint32_t LOOPS;
__host uint32_t OUT_CYCLES[NCASES];
__host uint32_t OUT_INSTR[NCASES];
__dma_aligned uint8_t buf[64];

/* xorshift on r5, a 16 B aligned address below 32 MB in r7, ldma into buf. */
#define RAND "lsl r6, r5, 13\n\txor r5, r5, r6\n\tlsr r6, r5, 17\n\txor r5, r5, r6\n\tlsl r6, r5, 5\n\txor r5, r5, r6\n\tand r7, r5, 0x1fffff0\n\t"
#define LD   "ldma r4, r7, 1\n\t"
#define ADD  "add r8, r8, 1\n\t"

#define RUN(k, BODY)                                                            \
    do {                                                                        \
        uint32_t i, seed = 2463534242u;                                         \
        perfcounter_config(COUNT_CYCLES, true);                                 \
        a = perfcounter_get();                                                  \
        for (i = 0; i < n; i++)                                                 \
            __asm__ volatile("move r4, %1\n\tmove r5, %0\n\t" BODY "move %0, r5" \
                             : "+r"(seed) : "r"(buf) : "r4", "r5", "r6", "r7", "r8"); \
        b = perfcounter_get();                                                  \
        OUT_CYCLES[k] = (uint32_t)(b - a);                                      \
        seed = 2463534242u;                                                     \
        perfcounter_config(COUNT_INSTRUCTIONS, true);                           \
        a = perfcounter_get();                                                  \
        for (i = 0; i < n; i++)                                                 \
            __asm__ volatile("move r4, %1\n\tmove r5, %0\n\t" BODY "move %0, r5" \
                             : "+r"(seed) : "r"(buf) : "r4", "r5", "r6", "r7", "r8"); \
        b = perfcounter_get();                                                  \
        OUT_INSTR[k] = (uint32_t)(b - a);                                       \
    } while (0)

int main(void)
{
    uint32_t n = LOOPS;
    perfcounter_t a, b;
    RUN(0, RAND LD);
    RUN(1, RAND LD ADD);
    RUN(2, RAND LD ADD ADD);
    RUN(3, RAND LD ADD ADD ADD);
    RUN(4, RAND LD ADD ADD ADD ADD);
    RUN(5, RAND LD ADD ADD ADD ADD ADD);
    RUN(6, RAND LD ADD ADD ADD ADD ADD ADD);
    RUN(7, RAND LD ADD ADD ADD ADD ADD ADD ADD);
    RUN(8, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(9, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(10, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(11, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(12, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(13, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(14, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(15, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(16, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(17, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(18, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(19, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(20, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(21, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(22, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(23, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    RUN(24, RAND LD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD ADD);
    return 0;
}
