/* Performance-counter calibration kernel.
 *
 * The SDK reads the counter as `time r` and returns the register shifted left
 * by 4 (perfcounter.c, BIT_IMPRECISION), so the register counts in units of 16
 * and the low four bits are lost. Nothing states what the register counts in
 * each mode, or at what rate, so this measures it instead of assuming.
 *
 * Two identical loops run whatever the build. Only the counter calls are
 * conditional, so a build without them executes the same instructions and
 * uPIMulator's exact num_instructions can be differenced against the counter's.
 *
 * Run with two values of LOOPS and difference them: the setup, the call
 * overhead and the counter's own imprecision all cancel, leaving the cost of
 * the extra iterations alone.
 */
#include <stdint.h>
#include <defs.h>

#if USE_PERF
#include <perfcounter.h>
#endif

__host uint32_t LOOPS;
__host uint32_t OUT_INSTR[NR_TASKLETS];
__host uint32_t OUT_CYCLES[NR_TASKLETS];
__host uint32_t SINK[NR_TASKLETS];

int main(void)
{
    uint32_t n = LOOPS;
    uint32_t acc = 0;
    uint32_t i;
    unsigned me_ = me();

#if USE_PERF
    /* Every tasklet configures and reads. If the counter is one register the
     * whole DPU shares, each tasklet sees all of them working and the readings
     * scale with NR_TASKLETS; if it is per-thread, each sees only its own. */
    perfcounter_config(COUNT_INSTRUCTIONS, true);
    perfcounter_t s = perfcounter_get();
#endif

    for (i = 0; i < n; i++) {
        __asm__ volatile("add %[a], %[a], 1" : [a] "+r"(acc));
    }

#if USE_PERF
    perfcounter_t e = perfcounter_get();
    OUT_INSTR[me_] = (uint32_t)(e - s);

    perfcounter_config(COUNT_CYCLES, true);
    s = perfcounter_get();
#endif

    for (i = 0; i < n; i++) {
        __asm__ volatile("add %[a], %[a], 1" : [a] "+r"(acc));
    }

#if USE_PERF
    e = perfcounter_get();
    OUT_CYCLES[me_] = (uint32_t)(e - s);
#endif

    SINK[me_] = acc;
    return 0;
}
