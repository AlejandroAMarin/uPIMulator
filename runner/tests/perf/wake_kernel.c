/* When each tasklet leaves a barrier, counted from when the last one arrives.
 *
 * The last tasklet to reach barrier_wait resumes the others one at a time.
 * PrIM va at 16 tasklets starts 2000 cycles later in the simulator than on the
 * machine, in the contended mem_alloc right after its first barrier, and the
 * order the tasklets come out in decides who gets the lock first. Tasklet 0
 * waits DELAY loop iterations before it arrives, so it is the last. Every
 * tasklet stamps the counter before and after the barrier; with WORK set, it
 * then runs WORK iterations of an empty loop and stamps again, which shows how
 * fast a woken tasklet runs while the others wake.
 */
#include <stdint.h>
#include <defs.h>
#include <perfcounter.h>
#include <barrier.h>

BARRIER_INIT(wk_barrier, NR_TASKLETS);
BARRIER_INIT(wk_start, NR_TASKLETS);

__host uint32_t DELAY;
__host uint32_t WORK;
__host uint32_t ARRIVE[NR_TASKLETS];
__host uint32_t LEAVE[NR_TASKLETS];
__host uint32_t DONE[NR_TASKLETS];

int main(void)
{
    unsigned me_ = me();
    uint32_t i, x = 0;

    if (me_ == 0) perfcounter_config(COUNT_CYCLES, true);
    barrier_wait(&wk_start);
    if (me_ == 0)
        for (i = 0; i < DELAY; i++) __asm__ volatile("" : "+r"(x));
    ARRIVE[me_] = (uint32_t)perfcounter_get();
    barrier_wait(&wk_barrier);
    LEAVE[me_] = (uint32_t)perfcounter_get();
    for (i = 0; i < WORK; i++) __asm__ volatile("" : "+r"(x));
    DONE[me_] = (uint32_t)perfcounter_get();
    return 0;
}
