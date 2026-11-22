/* The order a contended atomic bit is granted in, after a barrier.
 *
 * Tasklet 0 takes the bit before the barrier. After it, tasklets 1..N-1 spin
 * on the bit with "acquire bit, nz, .", the loop mem_alloc uses. Tasklet 0
 * waits DELAY loop iterations, so every other tasklet spins, then releases.
 * Each tasklet that gets the bit writes its number to ORDER, stamps GRANT and
 * releases. LEAVE is when each tasklet left the barrier; since the barrier
 * resumes its waiters in the order they arrived, LEAVE also gives the order
 * the barrier's own bit was granted in during the boot.
 *
 * The machine grants the bit to the thread that issues first after the
 * release. With 3 to 6 tasklets the barrier resumes 1, 2, ... in turn and the
 * machine grants the bit N-1 first and 1 last: a resumed thread goes ahead of
 * the running threads, so the rotation is packed in reverse order.
 */
#include <stdint.h>
#include <defs.h>
#include <perfcounter.h>
#include <barrier.h>
#include <atomic_bit.h>

BARRIER_INIT(gr_barrier, NR_TASKLETS);
ATOMIC_BIT_INIT(gr_bit);

__host uint32_t DELAY;
__host uint32_t REL;
__host uint32_t N;
__host uint32_t ORDER[NR_TASKLETS];
__host uint32_t LEAVE[NR_TASKLETS];
__host uint32_t GRANT[NR_TASKLETS];

int main(void)
{
    unsigned me_ = me();
    uint32_t i, x = 0;

    if (me_ == 0) {
        N = 0;
        perfcounter_config(COUNT_CYCLES, true);
        ATOMIC_BIT_ACQUIRE(gr_bit);
    }
    barrier_wait(&gr_barrier);
    LEAVE[me_] = (uint32_t)perfcounter_get();
    if (me_ == 0) {
        for (i = 0; i < DELAY; i++) __asm__ volatile("" : "+r"(x));
        REL = (uint32_t)perfcounter_get();
        ATOMIC_BIT_RELEASE(gr_bit);
        return 0;
    }
    ATOMIC_BIT_ACQUIRE(gr_bit);
    ORDER[N++] = me_;
    GRANT[me_] = (uint32_t)perfcounter_get();
    ATOMIC_BIT_RELEASE(gr_bit);
    return 0;
}
