/* What a contended lock costs: every tasklet calls mem_alloc at once.
 *
 * PrIM va at 16 tasklets is +1.7% in the simulator, and all of it is before
 * the first transfer: the two mem_alloc calls every tasklet makes at the
 * start finish 2000 cycles later in the simulator for the first tasklet, and
 * earlier for the last. mem_alloc takes an atomic bit with
 * "acquire zero, bit, nz, .", which retries itself until the bit is free.
 *
 * Each tasklet makes CALLS calls and stamps the counter when it is done.
 *
 * LKMODE, a build flag, picks what a call is, to find which part of mem_alloc
 * costs what: 0 mem_alloc(8), 1 mem_alloc_nolock(8) with no lock, 2 the lock
 * alone (acquire and release, no call), 3 a call to an empty function.
 */
#include <stdint.h>
#include <defs.h>
#include <alloc.h>
#include <perfcounter.h>
#include <barrier.h>
#include <atomic_bit.h>

#ifndef LKMODE
#define LKMODE 0
#endif

void *mem_alloc_nolock(size_t size);
ATOMIC_BIT_INIT(lk_bit);

void __attribute__((noinline)) lk_empty(void) { __asm__ volatile(""); }

BARRIER_INIT(lk_barrier, NR_TASKLETS);

__host uint32_t CALLS;
__host uint32_t START;
__host uint32_t DONE[NR_TASKLETS];

int main(void)
{
    unsigned me_ = me();
    uint32_t i, n = CALLS;

    if (me_ == 0) {
        mem_reset();
        perfcounter_config(COUNT_CYCLES, true);
    }
    barrier_wait(&lk_barrier);
    if (me_ == 0) START = (uint32_t)perfcounter_get();
    for (i = 0; i < n; i++) {
#if LKMODE == 0
        mem_alloc(8);
#elif LKMODE == 1
        mem_alloc_nolock(8);
#elif LKMODE == 2
        ATOMIC_BIT_ACQUIRE(lk_bit);
        ATOMIC_BIT_RELEASE(lk_bit);
#else
        lk_empty();
#endif
    }
    DONE[me_] = (uint32_t)perfcounter_get();
    barrier_wait(&lk_barrier);
    return 0;
}
