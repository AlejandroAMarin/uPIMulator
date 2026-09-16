/* Isolate each soft-float routine. volatile stops the compiler folding the
 * arithmetic away, which hid two of the three cases last time. */
#include <stdint.h>
#include <defs.h>

__host uint32_t INPUT;
__host uint32_t OUT_CONV;  /* __floatunsidf + __fixdfsi */
__host uint32_t OUT_ADD;   /* __adddf3  */
__host uint32_t OUT_MUL;   /* __muldf3  */
__host uint32_t OUT_SUB;   /* __subdf3  */

volatile double a, b;

int main(void)
{
    a = (double)INPUT;          /* 152.0 */
    OUT_CONV = (uint32_t)a;     /* expect 152 */

    b = 0.8;
    OUT_MUL = (uint32_t)(a * b);   /* expect 121 */

    b = 2.0;
    OUT_ADD = (uint32_t)(a + b);   /* expect 154 */
    OUT_SUB = (uint32_t)(a - b);   /* expect 150 */
    return 0;
}
