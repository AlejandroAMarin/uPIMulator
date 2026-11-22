/* When each tasklet's reads and writes start and finish, from the DPU's own
 * cycle counter.
 *
 * mix_kernel.c with 1 KB reads and writes and a long gap is 2% to 6% slow in
 * the simulator at 4 and 8 tasklets. The simulator's own trace shows two
 * tasklets locked in a convoy: one's read always arrives between the other's
 * read and write, so both wait a whole transfer every iteration. This probe
 * records, for every iteration, the counter before and after the read and the
 * write, so the machine's arrangement can be compared with the simulator's.
 *
 * Every tasklet reads the same counter, so the stamps are on one time base.
 */
#include <stdint.h>
#include <defs.h>
#include <mram.h>
#include <perfcounter.h>
#include <barrier.h>

BARRIER_INIT(tr_barrier, NR_TASKLETS);

/* 512 stamps of 16 B per tasklet at one tasklet, fewer with more, so the
 * table fits WRAM. */
#define MAXIT (512 / NR_TASKLETS)
#define REGION (8 << 20)

__host uint32_t ITERS; /* at most MAXIT */
__host uint32_t SIZE;  /* bytes read and written, at most 1024 */
__host uint32_t GAP;
__host uint32_t READS; /* reads per iteration, 1 or 2; 2 is PrIM va's pattern */
/* Four stamps per iteration: before the first read, after it, after the
 * second read (the same as after the first when READS is 1), after the write. */
__host uint32_t STAMPS[NR_TASKLETS][MAXIT][4];

__mram_noinit uint8_t region[REGION];
__dma_aligned uint8_t cache[NR_TASKLETS][1024];

int main(void)
{
    unsigned me_ = me();
    uint32_t n = ITERS, s = SIZE, gap = GAP, reads = READS, i, j, x = me_;
    uint32_t off = me_ * 65536;

    if (n > MAXIT) n = MAXIT;
    if (me_ == 0) perfcounter_config(COUNT_CYCLES, true);
    barrier_wait(&tr_barrier);

    for (i = 0; i < n; i++) {
        STAMPS[me_][i][0] = (uint32_t)perfcounter_get();
        mram_read((__mram_ptr void *)((uintptr_t)region + off), cache[me_], s);
        STAMPS[me_][i][1] = (uint32_t)perfcounter_get();
        if (reads > 1)
            mram_read((__mram_ptr void *)((uintptr_t)region + off + 2048), cache[me_], s);
        STAMPS[me_][i][2] = (uint32_t)perfcounter_get();
        mram_write(cache[me_], (__mram_ptr void *)((uintptr_t)region + off + 4096), s);
        STAMPS[me_][i][3] = (uint32_t)perfcounter_get();
        off = (off + 8192) & (REGION / 2 - 1);
        for (j = 0; j < gap; j++) __asm__ volatile("" : "+r"(x));
    }
    barrier_wait(&tr_barrier);
    return 0;
}
