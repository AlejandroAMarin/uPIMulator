/* Register-file bank conflicts, measured against the machine.
 *
 * uPIMulator charges extra cycles when the registers an instruction reads,
 * taken together with the register the instruction before it wrote, fall
 * unevenly across an even and an odd bank: floor(even/2) + floor(odd/2). It
 * holds the whole DPU's pipeline for that long. Nothing had ever checked
 * either half of that against hardware, and it costs a fifth of BIMSA's time
 * at 16 tasklets.
 *
 * Each case is one instruction repeated eight times in a row, so the loop's
 * own instructions cannot hide it. Read the result at one tasklet first: there
 * the revolver gives a thread one slot every 11 cycles, so a penalty of one
 * slot shows up as 11 extra cycles and cannot be mistaken for anything else.
 *
 * r4..r11 are caller-saved in the DPU ABI, so an asm block may use them.
 */
#include <stdint.h>
#include <defs.h>
#include <perfcounter.h>
#include <barrier.h>

BARRIER_INIT(rb_barrier, NR_TASKLETS);

#define NCASES 17

__host uint32_t LOOPS;

/* A WRAM buffer for the load and store cases. Eight-byte aligned, because a
 * 64-bit ld or sd needs that. */
__dma_aligned uint64_t scratch[8];
__host uint32_t OUT_CYCLES[NCASES];
__host uint32_t OUT_INSTR[NCASES];

#define X8(s) s s s s s s s s

/* RUNP is RUN with a pointer put in r6 first, for the cases that touch WRAM.
 * The extra move is one instruction of the nine in an iteration and it is the
 * same on both sides, so it cancels. */
#define RUNP(k, BODY, ...)                                                     \
    do {                                                                       \
        if (me_ == 0) perfcounter_config(COUNT_CYCLES, true);                  \
        barrier_wait(&rb_barrier);                                             \
        a = perfcounter_get();                                                 \
        for (i = 0; i < n; i++)                                                \
            __asm__ volatile("move r6, %[p]\n\t" X8(BODY)                      \
                             :                                                 \
                             : [p] "r"(scratch)                                \
                             : "r6", __VA_ARGS__);                             \
        b = perfcounter_get();                                                 \
        if (me_ == 0) OUT_CYCLES[k] = (uint32_t)(b - a);                       \
        barrier_wait(&rb_barrier);                                             \
        if (me_ == 0) perfcounter_config(COUNT_INSTRUCTIONS, true);            \
        barrier_wait(&rb_barrier);                                             \
        a = perfcounter_get();                                                 \
        for (i = 0; i < n; i++)                                                \
            __asm__ volatile("move r6, %[p]\n\t" X8(BODY)                      \
                             :                                                 \
                             : [p] "r"(scratch)                                \
                             : "r6", __VA_ARGS__);                             \
        b = perfcounter_get();                                                 \
        if (me_ == 0) OUT_INSTR[k] = (uint32_t)(b - a);                        \
        barrier_wait(&rb_barrier);                                             \
    } while (0)

/* run(k, body): time the body and record it, then count its instructions.
 * Tasklet 0 owns the counter, and a barrier keeps every tasklet inside the
 * same window -- the counter is one register the whole DPU shares. */
#define RUN(k, BODY, ...)                                                     \
    do {                                                                       \
        if (me_ == 0) perfcounter_config(COUNT_CYCLES, true);                  \
        barrier_wait(&rb_barrier);                                             \
        a = perfcounter_get();                                                 \
        for (i = 0; i < n; i++) __asm__ volatile(X8(BODY) ::: __VA_ARGS__);           \
        b = perfcounter_get();                                                 \
        if (me_ == 0) OUT_CYCLES[k] = (uint32_t)(b - a);                       \
        barrier_wait(&rb_barrier);                                             \
        if (me_ == 0) perfcounter_config(COUNT_INSTRUCTIONS, true);            \
        barrier_wait(&rb_barrier);                                             \
        a = perfcounter_get();                                                 \
        for (i = 0; i < n; i++) __asm__ volatile(X8(BODY) ::: __VA_ARGS__);           \
        b = perfcounter_get();                                                 \
        if (me_ == 0) OUT_INSTR[k] = (uint32_t)(b - a);                        \
        barrier_wait(&rb_barrier);                                             \
    } while (0)

int main(void)
{
    unsigned me_ = me();
    uint32_t n = LOOPS, i;
    perfcounter_t a, b;

    /* reads one even and one odd */
    RUN(0, "add r4, r4, r5\n\t", "r4", "r5");
    /* reads two even */
    RUN(1, "add r4, r6, r8\n\t", "r4", "r6", "r8");
    /* reads two odd */
    RUN(2, "add r4, r5, r7\n\t", "r4", "r5", "r7");
    /* reads one odd and one even, writes even: tells a rule that counts the
       destination apart from one that counts only the operands read */
    RUN(3, "add r4, r5, r6\n\t", "r4", "r5", "r6");
    /* reads two even, writes odd */
    RUN(4, "add r5, r4, r6\n\t", "r5", "r4", "r6");
    /* reads one register, writes the same one */
    RUN(5, "add r4, r4, 1\n\t", "r4");
    /* reads one even, writes a different even */
    RUN(6, "add r4, r6, 1\n\t", "r4", "r6");
    /* reads two even, writes a third even, with no operand in common */
    RUN(7, "sub r10, r6, r8\n\t", "r10", "r6", "r8");
    /* three odd: the test of a rule that counts each bank on its own */
    RUN(8, "add r5, r7, r9\n\t", "r5", "r7", "r9");
    /* two odd read, one even write */
    RUN(9, "add r6, r7, r9\n\t", "r6", "r7", "r9");
    /* one odd read, one even read, odd write */
    RUN(10, "add r5, r6, r7\n\t", "r5", "r6", "r7");

    /* The forms BIMSA's extend phase is built from, which the cases above do
     * not cover: WRAM loads and stores, 64-bit pairs, and the shift-and-add.
     * A 64-bit ld or sd touches four registers where a 32-bit one touches two,
     * and nothing had ever checked what the machine charges for that. */
    RUNP(11, "lw r4, r6, 0\n\t", "r4");
    RUNP(12, "ld d4, r6, 0\n\t", "r4", "r5");
    RUNP(13, "ld d8, r6, 0\n\t", "r8", "r9");
    RUNP(14, "sw r6, 0, r4\n\t", "r4");
    RUNP(15, "sd r6, 0, d4\n\t", "r4", "r5");
    RUN(16, "lsl_add r4, r6, r8, 2\n\t", "r4", "r6", "r8");

    return 0;
}
