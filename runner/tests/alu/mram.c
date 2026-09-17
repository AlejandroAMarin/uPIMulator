/* DMA alignment conformance kernel.
 *
 * The hardware moves whole 8-byte words, so it ignores the low three bits of
 * both DMA addresses: an mram_write to an address 4 mod 8 lands on the word
 * containing it, not four bytes along. uPIMulator honoured those bits instead,
 * which silently moved every unaligned transfer and is why BIMSA reported a
 * distance of zero for an alignment it had computed correctly.
 *
 * The host sweeps OFFSET and WOFFSET over aligned and unaligned values.
 */
#include <stdint.h>
#include <defs.h>
#include <mram.h>

__host uint32_t SRC[2];
__host uint32_t OUT[16];
__host uint32_t OFFSET;   /* MRAM byte offset */
__host uint32_t WOFFSET;  /* WRAM byte offset */

__mram_noinit uint8_t region[256];

int main(void)
{
    uint32_t i;
    uint8_t raw[64];
    uint32_t *buf = (uint32_t *)raw;

    for (i = 0; i < 2; i++) buf[i] = 0;
    for (i = 0; i < 64; i += 8) mram_write(buf, region + i, 8);

    /* place the payload at a WRAM byte offset, then push 8 bytes from there
     * to an MRAM byte offset. Both offsets may be unaligned. */
    uint32_t *src = (uint32_t *)(raw + 8 + WOFFSET);
    src[0] = SRC[0];
    src[1] = SRC[1];

    mram_write((void *)src, (__mram_ptr void *)((uintptr_t)region + OFFSET), 8);

    for (i = 0; i < 16; i += 2) {
        uint32_t tmp[2];
        mram_read((__mram_ptr void *)((uintptr_t)region + i * 4), tmp, 8);
        OUT[i] = tmp[0];
        OUT[i + 1] = tmp[1];
    }
    return 0;
}
