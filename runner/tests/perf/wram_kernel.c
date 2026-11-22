/* Does a DMA transfer slow another thread's WRAM accesses?
 *
 * PrIM va at 2 tasklets is 3.4% slow on the machine against the simulator,
 * and its compute is WRAM loads and stores while the other thread's transfer
 * writes WRAM. Tasklet 0 runs a loop and times it. The others do nothing, or
 * read MRAM in 1 KB transfers, or write it, until tasklet 0 is done.
 *
 *   FG  0 WRAM loads and stores, 1 register arithmetic only
 *   BG  0 nothing, 1 1 KB reads, 2 1 KB writes
 */
#include <stdint.h>
#include <defs.h>
#include <mram.h>
#include <perfcounter.h>
#include <barrier.h>

BARRIER_INIT(wr_barrier, NR_TASKLETS);

__host uint32_t LOOPS;
__host uint32_t FG;
__host uint32_t BG;
__host uint32_t OUT_CYCLES;
__host uint32_t OUT_INSTR;
__host uint32_t OUT_TRANSFERS;

volatile uint32_t done;
__mram_noinit uint8_t region[NR_TASKLETS][65536];
__dma_aligned uint8_t cache[NR_TASKLETS][1024];
__dma_aligned uint32_t work[256];

static void foreground(void)
{
    uint32_t n = LOOPS, i, x = 1;
    perfcounter_t a, b;

    if (FG == 0) {
        for (i = 0; i < n; i++) {
            uint32_t k = i & 255;
            work[k] = work[k] + x;
            x = work[(k + 7) & 255];
        }
    } else {
        for (i = 0; i < n; i++) {
            x = x * 3 + i;
            __asm__ volatile("" : "+r"(x));
        }
    }
    (void)a; (void)b;
    work[0] += x;
}

int main(void)
{
    unsigned me_ = me();
    perfcounter_t a, b;
    uint32_t off = 0, count = 0;

    if (me_ == 0) {
        done = 0;
        perfcounter_config(COUNT_CYCLES, true);
    }
    barrier_wait(&wr_barrier);

    if (me_ == 0) {
        a = perfcounter_get();
        foreground();
        b = perfcounter_get();
        OUT_CYCLES = (uint32_t)(b - a);
        done = 1;
    } else if (BG != 0) {
        while (!done) {
            if (BG == 1)
                mram_read(&region[me_][off], cache[me_], 1024);
            else
                mram_write(cache[me_], &region[me_][off], 1024);
            off = (off + 1024) & 65535;
            count++;
        }
    }
    barrier_wait(&wr_barrier);
    if (me_ == 1) OUT_TRANSFERS = count;

    /* The foreground loop again, counting instructions, with nothing behind. */
    if (me_ == 0) {
        perfcounter_config(COUNT_INSTRUCTIONS, true);
        a = perfcounter_get();
        foreground();
        b = perfcounter_get();
        OUT_INSTR = (uint32_t)(b - a);
    }
    barrier_wait(&wr_barrier);
    return 0;
}
