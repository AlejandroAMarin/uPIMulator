/* The cost of a small MRAM read at a random address, against one read in order.
 *
 * mram_kernel.c walks 8 MB with a 64 KB stride, which touches 128 addresses.
 * GIMSA's binary search reads one word at a random place in a 57 MB table, from
 * 12 tasklets, with some arithmetic between reads. This probe does that.
 *
 * Every mode runs the same instructions. The address is always
 *
 *     off = (base + walk + (x & RMASK)) & AMASK
 *
 * where x is a per-tasklet xorshift32 state that advances every iteration and
 * walk advances by STRIDE. Sequential sets RMASK = 0 and STRIDE = SIZE; random
 * sets RMASK to the region and STRIDE = 0. So a difference in cycles between
 * two modes is the memory, not the loop.
 *
 * GAP runs that many iterations of an empty loop between two reads. With enough
 * of it the DMA engine is idle when a read arrives, which is how a latency is
 * told apart from a throughput. NODMA runs the same loop without the read, so
 * the loop's own cost can be taken out.
 */
#include <stdint.h>
#include <defs.h>
#include <mram.h>
#include <perfcounter.h>
#include <barrier.h>

BARRIER_INIT(probe_barrier, NR_TASKLETS);

/* 32 MB is addressed; the rest is room for the last read of 2 KB. */
#define REGION (40 << 20)

__host uint32_t SIZE;   /* bytes per DMA, 8..2048 */
__host uint32_t ITERS;
__host uint32_t STRIDE; /* bytes the walk advances each read */
__host uint32_t RMASK;  /* random bits added to the address */
__host uint32_t AMASK;  /* the address wraps at this; at most 32 MB - 8 */
__host uint32_t SPREAD; /* distance between one tasklet's base and the next */
__host uint32_t GAP;
__host uint32_t NODMA;  /* 1 runs the loop without the read */
__host uint32_t OUT_CYCLES[NR_TASKLETS];
__host uint32_t OUT_INSTR[NR_TASKLETS];

__mram_noinit uint8_t region[REGION];
__dma_aligned uint8_t cache[NR_TASKLETS][2048];

static uint32_t run(unsigned me_, uint32_t seed)
{
    uint32_t n = ITERS, s = SIZE, stride = STRIDE, rmask = RMASK, amask = AMASK;
    uint32_t gap = GAP, nodma = NODMA, base = me_ * SPREAD, walk = 0, x = seed, i, j, off;

    for (i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        off = (base + walk + (x & rmask)) & amask;
        if (!nodma) mram_read((__mram_ptr void *)((uintptr_t)region + off), cache[me_], s);
        walk += stride;
        for (j = 0; j < gap; j++) __asm__ volatile("" : "+r"(x));
    }
    return x;
}

int main(void)
{
    unsigned me_ = me();
    uint32_t seed = 0x9e3779b9u * (me_ + 1);
    perfcounter_t a, b;

    if (me_ == 0) perfcounter_config(COUNT_CYCLES, true);
    barrier_wait(&probe_barrier);
    a = perfcounter_get();
    run(me_, seed);
    /* Close the window with a barrier: the counter is one register the whole
     * DPU shares, so without it the span depends on the order the tasklets
     * finish in. */
    barrier_wait(&probe_barrier);
    b = perfcounter_get();
    OUT_CYCLES[me_] = (uint32_t)(b - a);
    barrier_wait(&probe_barrier);

    /* The same loop again, counting instructions. */
    if (me_ == 0) perfcounter_config(COUNT_INSTRUCTIONS, true);
    barrier_wait(&probe_barrier);
    a = perfcounter_get();
    run(me_, seed);
    barrier_wait(&probe_barrier);
    b = perfcounter_get();
    OUT_INSTR[me_] = (uint32_t)(b - a);
    barrier_wait(&probe_barrier);

    return 0;
}
