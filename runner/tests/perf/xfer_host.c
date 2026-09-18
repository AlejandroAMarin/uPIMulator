/* Measure host<->DPU transfer cost on real hardware, so uPIMulator's transfer
 * model can be fitted to it rather than guessed at.
 *
 * Reports the median of many repetitions, not the best. The best is what the
 * link can do on a warm path; the median is what a program actually waits for,
 * and that is what a simulator should predict. The first repetition of all is
 * discarded and reported separately: it costs an order of magnitude more than
 * the rest, because the rank is being brought up, and folding that into an
 * average would corrupt every point.
 *
 * Sweeps the DPU count and the transfer size in both directions, one TSV row
 * per point, via dpu_push_xfer -- the bulk path that BIMSA and GIMSA use.
 */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y);
}

static const size_t SIZES[] = {8, 512, 4096, 32768, 262144, 1048576, 4194304};
#define NSIZES (sizeof(SIZES) / sizeof(SIZES[0]))
static const uint32_t DPUS[] = {1, 2, 4, 8, 16, 32, 64};
#define NDPUS (sizeof(DPUS) / sizeof(DPUS[0]))
#define MAXREPS 200

int main(void)
{
    struct dpu_set_t set, dpu;
    char **buf;
    size_t maxbytes = SIZES[NSIZES - 1];
    uint32_t maxdpus = DPUS[NDPUS - 1];
    double t[MAXREPS];
    uint32_t i;
    unsigned di, si;
    int r, dir;
    double cold = 0.0;
    int seen_cold = 0;

    buf = malloc(maxdpus * sizeof(char *));
    for (i = 0; i < maxdpus; i++) {
        buf[i] = malloc(maxbytes);
        memset(buf[i], (int)i + 1, maxbytes);
    }

    printf("dpus\tbytes_per_dpu\tdir\tmedian_us\tmin_us\n");

    for (di = 0; di < NDPUS; di++) {
        uint32_t nd = DPUS[di];

        if (dpu_alloc(nd, getenv("XPROFILE"), &set) != DPU_OK) break;
        DPU_ASSERT(dpu_load(set, getenv("XBIN") ? getenv("XBIN") : "/tmp/xfer_dpu", NULL));

        for (si = 0; si < NSIZES; si++) {
            size_t bytes = SIZES[si];

            for (dir = 0; dir < 2; dir++) {
                dpu_xfer_t way = dir == 0 ? DPU_XFER_TO_DPU : DPU_XFER_FROM_DPU;
                int reps = (int)(64 * 1024 * 1024 / (bytes * nd));

                if (reps < 5) reps = 5;
                if (reps > MAXREPS) reps = MAXREPS;

                for (r = 0; r < reps; r++) {
                    double a = now_s();
                    i = 0;
                    DPU_FOREACH (set, dpu, i) { DPU_ASSERT(dpu_prepare_xfer(dpu, buf[i])); }
                    DPU_ASSERT(dpu_push_xfer(set, way, DPU_MRAM_HEAP_POINTER_NAME, 0,
                                             bytes, DPU_XFER_DEFAULT));
                    t[r] = (now_s() - a) * 1e6;
                }

                if (!seen_cold) { cold = t[0]; seen_cold = 1; }

                /* Drop the first: it pays for bringing the rank up. */
                qsort(t + 1, (size_t)(reps - 1), sizeof(double), cmp_double);
                printf("%u\t%zu\t%s\t%.3f\t%.3f\n", nd, bytes,
                       dir == 0 ? "write" : "read", t[1 + (reps - 1) / 2], t[1]);
            }
        }

        DPU_ASSERT(dpu_free(set));
    }

    fprintf(stderr, "first transfer of all: %.1f us\n", cold);
    return 0;
}
