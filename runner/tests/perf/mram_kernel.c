/* MRAM access latency, measured with the DPU's own cycle counter.
 *
 * With one tasklet a DMA blocks the only thread there is, so the stall is
 * fully exposed and a cycle count is the latency itself rather than something
 * the scheduler hid. That is what makes this the right probe for the part of
 * uPIMulator's timing model that its kernel cycle counts still get wrong.
 *
 * STRIDE selects the access pattern. A stride of one transfer walks MRAM in
 * order and keeps hitting the same DRAM row; a large stride misses it every
 * time. The gap between the two is what the row buffer is worth, and a model
 * can reproduce one without reproducing the other.
 */
#include <stdint.h>
#include <defs.h>
#include <mram.h>
#include <perfcounter.h>
#include <barrier.h>

BARRIER_INIT(probe_barrier, NR_TASKLETS);

#define REGION (8 << 20)

__host uint32_t SIZE;   /* bytes per DMA, 8..2048 */
__host uint32_t ITERS;
__host uint32_t STRIDE; /* bytes between one DMA and the next */
__host uint32_t DO_WRITE;
/* Distance between one tasklet's working area and the next. Set it to 0 and
 * every tasklet reads the same MRAM row, which is how a per-row cost is told
 * apart from a per-transfer one. */
__host uint32_t SPREAD;
__host uint32_t OUT_CYCLES[NR_TASKLETS];
__host uint32_t OUT_INSTR[NR_TASKLETS];

__mram_noinit uint8_t region[REGION];
__dma_aligned uint8_t cache[NR_TASKLETS][2048];

int main(void)
{
    unsigned me_ = me();
    uint32_t n = ITERS, s = SIZE, stride = STRIDE;
    uint32_t base = me_ * SPREAD;
    uint32_t i, off = base;
    perfcounter_t a, b;

    if (me_ == 0) perfcounter_config(COUNT_CYCLES, true);
    barrier_wait(&probe_barrier);

    a = perfcounter_get();
    for (i = 0; i < n; i++) {
        if (DO_WRITE) {
            mram_write(cache[me_], (__mram_ptr void *)((uintptr_t)region + off), s);
        } else {
            mram_read((__mram_ptr void *)((uintptr_t)region + off), cache[me_], s);
        }
        off += stride;
        if (off >= REGION - 2048) off = base;
    }
    b = perfcounter_get();
    OUT_CYCLES[me_] = (uint32_t)(b - a);

    /* The counter is one register the whole DPU shares, so tasklet 0 must not
     * reconfigure it until every tasklet has read it. Without this barrier a
     * late tasklet reads its end value after the reset and its span underflows
     * to about 2^32. */
    barrier_wait(&probe_barrier);

    /* The same loop again, counting instructions, so the loop's own cost can
     * be taken out of the cycles above. */
    if (me_ == 0) perfcounter_config(COUNT_INSTRUCTIONS, true);
    barrier_wait(&probe_barrier);

    a = perfcounter_get();
    for (i = 0; i < n; i++) {
        if (DO_WRITE) {
            mram_write(cache[me_], (__mram_ptr void *)((uintptr_t)region + off), s);
        } else {
            mram_read((__mram_ptr void *)((uintptr_t)region + off), cache[me_], s);
        }
        off += stride;
        if (off >= REGION - 2048) off = base;
    }
    b = perfcounter_get();
    OUT_INSTR[me_] = (uint32_t)(b - a);
    barrier_wait(&probe_barrier);

    return 0;
}
