/* Instruction costs that only show with many tasklets, measured against the
 * machine.
 *
 * scatter_kernel.c found that a 12-instruction loop with no memory access in
 * it matches the machine at one tasklet and costs the machine about one cycle
 * more per iteration at 12 and 16 tasklets. At one tasklet a thread's next
 * instruction is 11 cycles behind its last, so anything the pipeline charges
 * between two instructions of different threads is invisible there. This probe
 * repeats one instruction form at a time to find which form it is.
 *
 * Each case is one form repeated eight times in a row, as in regbank_kernel.c.
 * r4..r11 are caller-saved in the DPU ABI, so an asm block may use them.
 */
#include <stdint.h>
#include <defs.h>
#include <perfcounter.h>
#include <barrier.h>
#include <atomic_bit.h>

BARRIER_INIT(pp_barrier, NR_TASKLETS);
ATOMIC_BIT_INIT(pp_bit);

#define NCASES 37

__host uint32_t LOOPS;
/* For the load cases. Eight-byte aligned, because a 64-bit ld needs that. */
__dma_aligned uint64_t scratch;
__dma_aligned uint64_t stackslot[2];
uint32_t heapword;
__host uint32_t OUT_CYCLES[NCASES];
__host uint32_t OUT_INSTR[NCASES];

#define X4(s) s s s s
#define X8(s) s s s s s s s s

/* A branch to the next instruction, eight times, each with its own label. */
#define TO_NEXT(op, n) op ".Lpp%=_" #n "\n.Lpp%=_" #n ":\n\t"
#define X8_TO_NEXT(op)                                                         \
    TO_NEXT(op, 0) TO_NEXT(op, 1) TO_NEXT(op, 2) TO_NEXT(op, 3)                \
    TO_NEXT(op, 4) TO_NEXT(op, 5) TO_NEXT(op, 6) TO_NEXT(op, 7)

/* Time the body, then count its instructions. Tasklet 0 owns the counter, and
 * a barrier keeps every tasklet inside the same window. */
#define RUN(k, BODY, ...)                                                      \
    do {                                                                       \
        if (me_ == 0) perfcounter_config(COUNT_CYCLES, true);                  \
        barrier_wait(&pp_barrier);                                             \
        a = perfcounter_get();                                                 \
        for (i = 0; i < n; i++) __asm__ volatile(BODY ::: __VA_ARGS__);        \
        barrier_wait(&pp_barrier);                                             \
        b = perfcounter_get();                                                 \
        if (me_ == 0) OUT_CYCLES[k] = (uint32_t)(b - a);                       \
        barrier_wait(&pp_barrier);                                             \
        if (me_ == 0) perfcounter_config(COUNT_INSTRUCTIONS, true);            \
        barrier_wait(&pp_barrier);                                             \
        a = perfcounter_get();                                                 \
        for (i = 0; i < n; i++) __asm__ volatile(BODY ::: __VA_ARGS__);        \
        barrier_wait(&pp_barrier);                                             \
        b = perfcounter_get();                                                 \
        if (me_ == 0) OUT_INSTR[k] = (uint32_t)(b - a);                        \
        barrier_wait(&pp_barrier);                                             \
    } while (0)

int main(void)
{
    unsigned me_ = me();
    uint32_t n = LOOPS, i;
    perfcounter_t a, b;

    RUN(0, X8("add r4, r4, 1\n\t"), "r4");
    RUN(1, X8("lsl r4, r5, 13\n\t"), "r4");
    RUN(2, X8("lsr r4, r5, 17\n\t"), "r4");
    RUN(3, X8("xor r5, r4, r5\n\t"), "r5");
    /* the xorshift step, each instruction reading what the one before wrote */
    RUN(4, X4("lsl r6, r5, 13\n\txor r5, r6, r5\n\t"), "r5", "r6");
    RUN(5, X8_TO_NEXT("jump "), "r4");
    RUN(6, "move r4, 0\n\t" X8_TO_NEXT("jeq r4, 0, "), "r4");
    RUN(7, "move r4, 0\n\t" X8_TO_NEXT("jneq r4, 0, "), "r4");
    /* scatter_kernel.c's loop without the read, at a gap of 0, straight-line:
     * three branches that fall through and a jump that is taken. The
     * registers keep the parity of the compiled loop. */
    RUN(8,
        "move r7, 1\n\tmove r9, 0\n\tmove r11, -1\n\t"
        "add r4, r4, 1\n\tadd r8, r8, r10\n\tjeq r4, r11, .Lpp%=_x\n\t"
        "lsl r6, r5, 13\n\txor r5, r6, r5\n\tlsr r6, r5, 17\n\txor r5, r6, r5\n\t"
        "lsl r6, r5, 5\n\txor r5, r6, r5\n\tjeq r7, 0, .Lpp%=_x\n\t"
        "jneq r9, 0, .Lpp%=_x\n\tjump .Lpp%=_x\n.Lpp%=_x:\n\t",
        "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11");
    /* the same without the jump */
    RUN(9,
        "move r7, 1\n\tmove r9, 0\n\tmove r11, -1\n\t"
        "add r4, r4, 1\n\tadd r8, r8, r10\n\tjeq r4, r11, .Lpp%=_x\n\t"
        "lsl r6, r5, 13\n\txor r5, r6, r5\n\tlsr r6, r5, 17\n\txor r5, r6, r5\n\t"
        "lsl r6, r5, 5\n\txor r5, r6, r5\n\tjeq r7, 0, .Lpp%=_x\n\t"
        "jneq r9, 0, .Lpp%=_x\n.Lpp%=_x:\n\t",
        "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11");

    /* Pieces of case 9, to find the one that loses the slot. */
    RUN(10, X8("add r8, r8, r10\n\t"), "r8");
    RUN(11, "move r11, -1\n\t" X8_TO_NEXT("jeq r4, r11, "), "r11");
    RUN(12,
        "move r7, 1\n\tmove r9, 0\n\tmove r11, -1\n\t"
        "add r4, r4, 1\n\tadd r8, r8, r10\n\t"
        "lsl r6, r5, 13\n\txor r5, r6, r5\n\tlsr r6, r5, 17\n\txor r5, r6, r5\n\t"
        "lsl r6, r5, 5\n\txor r5, r6, r5\n\tjeq r7, 0, .Lpp%=_x\n\t"
        "jneq r9, 0, .Lpp%=_x\n.Lpp%=_x:\n\t",
        "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11");
    RUN(13,
        "move r7, 1\n\tmove r9, 0\n\tmove r11, -1\n\t"
        "add r4, r4, 1\n\tjeq r4, r11, .Lpp%=_x\n\t"
        "lsl r6, r5, 13\n\txor r5, r6, r5\n\tlsr r6, r5, 17\n\txor r5, r6, r5\n\t"
        "lsl r6, r5, 5\n\txor r5, r6, r5\n\tjeq r7, 0, .Lpp%=_x\n\t"
        "jneq r9, 0, .Lpp%=_x\n.Lpp%=_x:\n\t",
        "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11");
    RUN(14,
        "lsl r6, r5, 13\n\txor r5, r6, r5\n\tlsr r6, r5, 17\n\txor r5, r6, r5\n\t"
        "lsl r6, r5, 5\n\txor r5, r6, r5\n\t",
        "r5", "r6");
    RUN(15,
        "move r7, 1\n\tmove r9, 0\n\tmove r11, -1\n\t"
        "add r4, r4, 1\n\tadd r8, r8, r10\n\tjeq r4, r11, .Lpp%=_x\n\t"
        "jeq r7, 0, .Lpp%=_x\n\tjneq r9, 0, .Lpp%=_x\n.Lpp%=_x:\n\t",
        "r4", "r7", "r8", "r9", "r10", "r11");
    RUN(16, "move r7, 1\n\tmove r9, 0\n\t" X4("jeq r7, 0, .Lpp%=_x\n\tjneq r9, 0, .Lpp%=_x\n\t")
        ".Lpp%=_x:\n\t", "r7", "r9");
    RUN(17, X8("move r11, -1\n\t"), "r11");

    /* Which registers count. The old rule counted this instruction's
     * destination with its sources; the rule the cases above fit counts the
     * previous instruction's destination instead. These tell the two apart. */
    /* three even by the old rule, two by the new */
    RUN(18, X4("add r5, r5, 1\n\tadd r4, r6, r8\n\t"), "r4", "r5");
    /* three odd by the old rule, two by the new */
    RUN(19, X4("move r4, 1\n\tadd r5, r7, r9\n\t"), "r4", "r5");
    /* two even by the old rule, three by the new */
    RUN(20, X4("add r4, r4, 1\n\tadd r5, r6, r8\n\t"), "r4", "r5");
    /* a load's destination as the previous write */
    RUN(21, X4("lw r4, zero, LOOPS\n\tadd r5, r6, r8\n\t"), "r4", "r5");
    /* a 64-bit load writes a pair; is its odd half counted? */
    RUN(22, X4("ld d4, zero, scratch\n\tadd r6, r7, r9\n\t"), "r4", "r5", "r6");

    /* The lock mem_alloc takes: acquire and release of one atomic bit, which
     * no other tasklet touches, so every acquire succeeds. */
    RUN(23, X4("acquire zero, __atomic_bit_pp_bit, nz, .\n\trelease zero, __atomic_bit_pp_bit, nz, .+1\n\t"));
    /* A call and its return, as mem_alloc calls mem_alloc_nolock. */
    RUN(24, "call r23, .Lpp%=_f\n\tjump .Lpp%=_e\n.Lpp%=_f:\n\tjump r23\n.Lpp%=_e:\n\t", "r23");
    /* A byte load from WRAM, as mem_alloc_nolock probes the heap's end. */
    RUN(25, "move r6, scratch\n\t" X8("lbu r4, r6, 0\n\t"), "r4", "r6");
    /* An acquire that fails: the bit is held already, so the seven after the
     * first all fail and fall through to the next instruction. */
    RUN(26, "acquire zero, __atomic_bit_pp_bit, nz, .+1\n\t"
            X4("acquire zero, __atomic_bit_pp_bit, nz, .+1\n\t")
            "acquire zero, __atomic_bit_pp_bit, nz, .+1\n\tacquire zero, __atomic_bit_pp_bit, nz, .+1\n\t"
            "acquire zero, __atomic_bit_pp_bit, nz, .+1\n\trelease zero, __atomic_bit_pp_bit, nz, .+1\n\t");
    /* The same, but a failed acquire jumps to a label two instructions on. */
    RUN(27, "acquire zero, __atomic_bit_pp_bit, nz, .+1\n\t"
            X4("acquire zero, __atomic_bit_pp_bit, nz, .+2\n\tmove r4, 0\n\t")
            "release zero, __atomic_bit_pp_bit, nz, .+1\n\t", "r4");

    /* mem_alloc's whole path, straight-line, registers renumbered with the
     * same parity: r22/r23 -> r10/r11, r0/r1/r2 -> r4/r5/r6. */
#define ALLOC_PATH(pre, post)                                                  \
    "move r10, stackslot\n\tmove r4, 8\n\t"                               \
    "call r11, .Lpp%=_m\n\tjump .Lpp%=_e\n"                                \
    ".Lpp%=_m:\n\tsd r10, 0, d10\n\tadd r10, r10, 8\n\t"                \
    pre "call r11, .Lpp%=_n\n\t" post                                        \
    "ld d10, r10, -8\n\tjump r11\n"                                         \
    ".Lpp%=_n:\n\tlw r5, zero, heapword\n\tjeq r4, 0, .Lpp%=_z\n\t"      \
    "add r5, r5, 7\n\tand r5, r5, -8\n\t"                                  \
    "add r4, r5, r4, nc, .Lpp%=_k\n\tfault 1\n.Lpp%=_k:\n\tlbu r6, r4, -1\n\t" \
    "sw zero, heapword, r4\n.Lpp%=_z:\n\tmove r4, r5\n\tjump r11\n.Lpp%=_e:\n\t"
    /* The heap pointer starts at a real WRAM symbol: the simulator places
     * WRAM at 512, so an absolute address below that is not WRAM there. */
    RUN(28, "move r9, scratch\n\tsw zero, heapword, r9\n\t" ALLOC_PATH("", ""),
        "r4", "r5", "r6", "r9", "r10", "r11");
    /* the add that jumps over the fault, then the byte load behind it */
    RUN(29, "move r5, 0\n\tmove r4, scratch\n\tadd r4, r4, 1\n\t"
            "add r4, r5, r4, nc, .Lpp%=_k\n\tfault 1\n.Lpp%=_k:\n\tlbu r6, r4, -1\n\t",
        "r4", "r5", "r6");
    /* the stack store after the call that wrote the return address */
    RUN(30, "move r10, stackslot\n\tcall r11, .Lpp%=_m\n\tjump .Lpp%=_e\n"
            ".Lpp%=_m:\n\tsd r10, 0, d10\n\tjump r11\n.Lpp%=_e:\n\t", "r10", "r11");
    /* the stack load and the return after it */
    RUN(31, "move r10, stackslot\n\tcall r11, .Lpp%=_m\n\tjump .Lpp%=_e\n"
            ".Lpp%=_m:\n\tsd r10, 0, d10\n\tld d10, r10, 0\n\tjump r11\n.Lpp%=_e:\n\t", "r10", "r11");
    /* the load of the heap pointer and the test that follows it */
    RUN(32, "move r4, 8\n\t" X4("lw r5, zero, heapword\n\tjeq r4, 0, .Lpp%=_z\n\t") ".Lpp%=_z:\n\t",
        "r4", "r5");
    /* The same path with mem_alloc's lock around the inner call. */
    RUN(33, "move r9, scratch\n\tsw zero, heapword, r9\n\t"
            ALLOC_PATH("acquire zero, __atomic_bit_pp_bit, nz, .\n\t",
                       "release zero, __atomic_bit_pp_bit, nz, .+1\n\t"),
        "r4", "r5", "r6", "r9", "r10", "r11");
    /* acquire then a call, and a return then release, the lock's two joins */
    RUN(34, "acquire zero, __atomic_bit_pp_bit, nz, .\n\tcall r11, .Lpp%=_f\n\t"
            "release zero, __atomic_bit_pp_bit, nz, .+1\n\tjump .Lpp%=_e\n"
            ".Lpp%=_f:\n\tjump r11\n.Lpp%=_e:\n\t", "r11");
    /* mem_alloc_nolock's body with its own registers, r0, r1 and r2. */
    RUN(35, "move r9, scratch\n\tsw zero, heapword, r9\n\tmove r0, 8\n\t"
            "call r11, .Lpp%=_n\n\tjump .Lpp%=_e\n"
            ".Lpp%=_n:\n\tlw r1, zero, heapword\n\tjeq r0, 0, .Lpp%=_z\n\t"
            "add r1, r1, 7\n\tand r1, r1, -8\n\t"
            "add r0, r1, r0, nc, .Lpp%=_k\n\tfault 1\n.Lpp%=_k:\n\tlbu r2, r0, -1\n\t"
            "sw zero, heapword, r0\n.Lpp%=_z:\n\tmove r0, r1\n\tjump r11\n.Lpp%=_e:\n\t",
        "r0", "r1", "r2", "r9", "r11");
    /* the same with r4, r5 and r6 */
    RUN(36, "move r9, scratch\n\tsw zero, heapword, r9\n\tmove r4, 8\n\t"
            "call r11, .Lpp%=_n\n\tjump .Lpp%=_e\n"
            ".Lpp%=_n:\n\tlw r5, zero, heapword\n\tjeq r4, 0, .Lpp%=_z\n\t"
            "add r5, r5, 7\n\tand r5, r5, -8\n\t"
            "add r4, r5, r4, nc, .Lpp%=_k\n\tfault 1\n.Lpp%=_k:\n\tlbu r6, r4, -1\n\t"
            "sw zero, heapword, r4\n.Lpp%=_z:\n\tmove r4, r5\n\tjump r11\n.Lpp%=_e:\n\t",
        "r4", "r5", "r6", "r9", "r11");

    return 0;
}
