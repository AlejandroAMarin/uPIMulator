/* A read's latency, and when refresh delays it, one read at a time.
 *
 * One tasklet stamps the counter, reads SIZE bytes, stamps again, and runs
 * DELAY empty iterations, 512 times. The stamps advance in steps of 16
 * cycles, but over many reads they fall at every point of a step, so the mean
 * latency of the reads that refresh did not touch is exact to about a cycle.
 * On the machine it is 242.5 + 0.4985 x SIZE cycles, stamping included.
 *
 * The reads refresh delays show its period: every 2736 cycles. They also
 * show a loop locking to it. With DELAY 37 the machine's loop runs 9
 * iterations in 4 refresh periods and every ninth read waits about 112
 * cycles; the natural period, 1203.6 cycles, is just short of 10944 / 9.
 */
#include <stdint.h>
#include <defs.h>
#include <mram.h>
#include <perfcounter.h>

#define N 512

__host uint32_t DELAY;
__host uint32_t SIZE;
__host uint32_t T0[N];
__host uint32_t T1[N];

__mram_noinit uint8_t region[1 << 20];
__dma_aligned uint8_t buf[2048];

int main(void)
{
    uint32_t i, j, x = 0, d = DELAY, s = SIZE;

    perfcounter_config(COUNT_CYCLES, true);
    for (i = 0; i < N; i++) {
        T0[i] = (uint32_t)perfcounter_get();
        mram_read(&region[(i * 4096) & ((1 << 20) - 1)], buf, s);
        T1[i] = (uint32_t)perfcounter_get();
        for (j = 0; j < d; j++) __asm__ volatile("" : "+r"(x));
    }
    return 0;
}
