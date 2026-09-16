/* Isolation tripwire test.
 *
 * Asks for one DPU with the DEFAULT profile (NULL). That is deliberately the
 * dangerous case: with no backend= key, libdpu probes for ranks and, finding
 * 40 on this machine, selects real hardware. So this program allocates real
 * DPUs when run unprotected.
 *
 * NEVER run it outside scripts/test_isolation.sh, which supplies both env
 * layers and watches with strace.
 *
 * Expected under protection: dpu_alloc fails, nothing opens a rank.
 */
#include <dpu.h>
#include <stdio.h>

int main(void)
{
    struct dpu_set_t set;

    dpu_error_t err = dpu_alloc(1, NULL, &set);

    if (err == DPU_OK) {
        /* Reaching here means isolation FAILED and we hold a real rank.
         * Give it straight back, then report loudly. */
        dpu_free(set);
        printf("ALLOC_SUCCEEDED\n");
        return 2;
    }

    printf("ALLOC_REFUSED err=%u (%s)\n", err, dpu_error_to_string(err));
    return 0;
}
