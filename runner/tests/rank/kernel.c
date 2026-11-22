/* One tasklet. Combines a value the host copies to its rank with a value the
 * host pushes to this DPU, and counts its launches since the load, so the host
 * can tell which DPUs each copy, push and launch reached. */
#include <defs.h>
#include <mram.h>
#include <stdint.h>

__host uint32_t in_value;
__host uint32_t out_value;
__host uint32_t launches;
__mram_noinit uint64_t pushed[1];

int main(void)
{
    __dma_aligned uint64_t word;

    mram_read(pushed, &word, sizeof(word));
    out_value = in_value * 2 + 1 + (uint32_t)word;
    launches++;
    return 0;
}
