/* How often a thread retries a lock it is waiting for.
 *
 * mem_alloc waits with "acquire zero, bit, nz, .", an acquire that jumps to
 * itself until the bit is free. Tasklet 1 takes the bit and holds it for a
 * loop of known length; tasklet 0 waits for it meanwhile. The instructions the
 * DPU counts in that window, less tasklet 1's own, are tasklet 0's retries.
 * Build with NR_TASKLETS=2.
 */
#include <stdint.h>
#include <defs.h>
#include <perfcounter.h>
#include <barrier.h>
#include <atomic_bit.h>

BARRIER_INIT(sp_barrier, NR_TASKLETS);
ATOMIC_BIT_INIT(sp_bit);

__host uint32_t HOLD;   /* iterations of tasklet 1's loop while it holds the bit */
__host uint32_t MODE;   /* 0 counts cycles, 1 counts instructions */
__host uint32_t OUT_SPIN;   /* the count over tasklet 0's wait */
__host uint32_t OUT_ALONE;  /* the same loop with nobody waiting */

static void hold(uint32_t n)
{
    uint32_t i, x = 0;
    for (i = 0; i < n; i++) __asm__ volatile("" : "+r"(x));
}

int main(void)
{
    unsigned me_ = me();
    perfcounter_t a, b;

    /* Tasklet 1's loop alone, as the reference. */
    if (me_ == 0) perfcounter_config(MODE ? COUNT_INSTRUCTIONS : COUNT_CYCLES, true);
    barrier_wait(&sp_barrier);
    if (me_ == 1) {
        a = perfcounter_get();
        hold(HOLD);
        b = perfcounter_get();
        OUT_ALONE = (uint32_t)(b - a);
    }
    barrier_wait(&sp_barrier);

    if (me_ == 1) ATOMIC_BIT_ACQUIRE(sp_bit);
    barrier_wait(&sp_barrier);
    if (me_ == 1) {
        a = perfcounter_get();
        hold(HOLD);
        ATOMIC_BIT_RELEASE(sp_bit);
    } else {
        ATOMIC_BIT_ACQUIRE(sp_bit);
        b = perfcounter_get();
        ATOMIC_BIT_RELEASE(sp_bit);
    }
    barrier_wait(&sp_barrier);
    if (me_ == 0) OUT_SPIN = (uint32_t)b;
    if (me_ == 1) OUT_SPIN = (uint32_t)(OUT_SPIN - (uint32_t)a);
    barrier_wait(&sp_barrier);
    return 0;
}
