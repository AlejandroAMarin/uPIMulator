/* Does nothing. The host only needs a loaded program to transfer into. */
#include <stdint.h>
#include <defs.h>
#include <mram.h>

__mram_noinit uint8_t buffer[8 << 20];

int main(void)
{
    return 0;
}
