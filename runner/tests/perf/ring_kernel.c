/* The rotation the boot chain leaves the tasklets in.
 *
 * Tasklet 0 takes a bit before the others start. They spin on it; tasklet 0
 * waits DELAY loop iterations and releases it; each tasklet that gets the bit
 * writes its number to ORDER and releases it. The bit goes to the thread that
 * issues first after the release, so ORDER is the rotation, read from
 * tasklet 0 onwards.
 *
 * On the machine tasklet k boots k+1 with its third instruction, and k+1
 * issues its first 13 cycles after that: 35 cycles after k's first, so each
 * tasklet sits two cycles after the one before it in the 11-cycle revolver.
 * With 8 tasklets the machine gives 6 1 7 2 3 4 5.
 */
#include <stdint.h>
#include <defs.h>
#include <atomic_bit.h>

ATOMIC_BIT_INIT(rg_bit);

__host uint32_t DELAY;
__host uint32_t N;
__host uint32_t ORDER[NR_TASKLETS];

int main(void)
{
    unsigned me_ = me();
    uint32_t i, x = 0;

    if (me_ == 0) {
        ATOMIC_BIT_ACQUIRE(rg_bit);
        N = 0;
        for (i = 0; i < DELAY; i++) __asm__ volatile("" : "+r"(x));
        ATOMIC_BIT_RELEASE(rg_bit);
        return 0;
    }
    ATOMIC_BIT_ACQUIRE(rg_bit);
    ORDER[N++] = me_;
    ATOMIC_BIT_RELEASE(rg_bit);
    return 0;
}
