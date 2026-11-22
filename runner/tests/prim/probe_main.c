/* A counter probe around a PrIM kernel whose source stays unchanged.
 *
 * Compile PrIM's dpu/task.c with -Dmain=prim_main and link it with this file.
 * This main starts the DPU's counter, runs PrIM's main in every tasklet, and
 * stops the counter when the last tasklet is done. The host reads OUT_COUNT
 * after the launch: runner/scripts/launch_probe.c on the machine, the shim in
 * the simulator (PROBE_OUT, PROBE_MODE).
 *
 * One counter serves the whole DPU, so tasklet 0's reading covers every
 * tasklet's work. The barrier before the reading makes the window end with
 * the last tasklet, not with tasklet 0's own share.
 */
#include <barrier.h>
#include <defs.h>
#include <perfcounter.h>
#include <stdint.h>

__host uint32_t PROBE_MODE; /* 0 = cycles, 1 = instructions */
__host uint32_t OUT_COUNT;
BARRIER_INIT(probe_barrier, NR_TASKLETS);

int prim_main(void);

int main(void)
{
    if (me() == 0)
        perfcounter_config(PROBE_MODE ? COUNT_INSTRUCTIONS : COUNT_CYCLES, true);
    barrier_wait(&probe_barrier);
    perfcounter_t start = perfcounter_get();

    int result = prim_main();

    barrier_wait(&probe_barrier);
    perfcounter_t end = perfcounter_get();
    if (me() == 0)
        OUT_COUNT = (uint32_t)(end - start);
    return result;
}
