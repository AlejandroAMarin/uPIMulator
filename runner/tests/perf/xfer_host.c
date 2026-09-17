/* Measure host<->DPU transfer bandwidth.
 *
 * uPIMulator's channel model charges size/bandwidth cycles for a transfer, with
 * read_bandwidth and write_bandwidth in bytes per cycle. Nothing had ever
 * checked those two numbers against a real rank. This does.
 *
 * The runner does not use the channel model at all -- api.CopyTo writes DPU
 * memory directly -- so this only runs on hardware, and says whether the model
 * would be right if it were put on that path.
 */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MB (1024 * 1024)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

int main(void)
{
    struct dpu_set_t set, dpu;
    uint32_t nr_dpus = (uint32_t)atoi(getenv("XDPUS") ? getenv("XDPUS") : "1");
    size_t bytes = (size_t)atoi(getenv("XBYTES_MB") ? getenv("XBYTES_MB") : "4") * MB;
    int reps = atoi(getenv("XREPS") ? getenv("XREPS") : "3");
    char **buf;
    double t0, tw = 1e30, tr = 1e30;
    uint32_t i;
    int r;

    buf = malloc(nr_dpus * sizeof(char *));
    for (i = 0; i < nr_dpus; i++) {
        buf[i] = malloc(bytes);
        memset(buf[i], (int)i + 1, bytes);
    }

    DPU_ASSERT(dpu_alloc(nr_dpus, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/xfer_dpu", NULL));

    for (r = 0; r < reps; r++) {
        i = 0;
        t0 = now_s();
        DPU_FOREACH (set, dpu, i) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, buf[i]));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0,
                                 bytes, DPU_XFER_DEFAULT));
        t0 = now_s() - t0;
        if (t0 < tw) tw = t0;

        i = 0;
        t0 = now_s();
        DPU_FOREACH (set, dpu, i) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, buf[i]));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0,
                                 bytes, DPU_XFER_DEFAULT));
        t0 = now_s() - t0;
        if (t0 < tr) tr = t0;
    }

    {
        double total = (double)bytes * (double)nr_dpus;
        printf("dpus=%-3u bytes/dpu=%-9zu write=%7.3f GB/s (%8.3f ms)  "
               "read=%7.3f GB/s (%8.3f ms)\n",
               nr_dpus, bytes, total / tw / 1e9, tw * 1e3,
               total / tr / 1e9, tr * 1e3);
    }

    DPU_ASSERT(dpu_free(set));
    return 0;
}
