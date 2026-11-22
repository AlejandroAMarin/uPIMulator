/* Measure what a set that spans several ranks does on real hardware: how
 * dpu_alloc spreads the DPUs over ranks, and what a transfer and a launch cost
 * as the number of ranks grows. Fits the runner's rank model.
 *
 * Part 1 prints, for each set size, the ranks the set holds and the DPUs on
 * each, and which physical ranks the process owns (from sysfs).
 * Part 2 prints one TSV row per point: the median time of a dpu_push_xfer
 * to or from every DPU, of a dpu_broadcast_to, and of a synchronous launch of
 * an empty kernel. The first transfer of each set is reported on its own,
 * because it brings the ranks up.
 *
 *   XPROFILE=backend=hw UPMEM_PROFILE=backend=hw XBIN=/tmp/xfer_dpu ./rank_host
 *
 * XDPUS="64,128" overrides the set sizes, XSIZES="512,32768" the bytes per DPU.
 */
#include <dirent.h>
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

static unsigned parse_list(const char *text, unsigned long *out, unsigned max, const unsigned long *fallback,
                           unsigned nfallback)
{
    unsigned n = 0;
    if (text == NULL || *text == 0) {
        memcpy(out, fallback, nfallback * sizeof(*out));
        return nfallback;
    }
    while (*text && n < max) {
        out[n++] = strtoul(text, (char **)&text, 10);
        if (*text == ',') text++;
    }
    return n;
}

static int read_sysfs(const char *rank, const char *name)
{
    char path[256];
    int value = -1;
    snprintf(path, sizeof(path), "/sys/class/dpu_rank/%s/%s", rank, name);
    FILE *file = fopen(path, "r");
    if (file) {
        if (fscanf(file, "%d", &value) != 1) value = -1;
        fclose(file);
    }
    return value;
}

/* Prints the physical ranks marked owned, with their channel. */
static void print_owned(void)
{
    DIR *dir = opendir("/sys/class/dpu_rank");
    struct dirent *entry;
    printf("#   owned:");
    if (dir == NULL) {
        printf(" (no sysfs)\n");
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "dpu_rank", 8) != 0) continue;
        if (read_sysfs(entry->d_name, "is_owned") == 1)
            printf(" %s(ch%d)", entry->d_name + 8, read_sysfs(entry->d_name, "channel_id"));
    }
    closedir(dir);
    printf("\n");
}

#define MAXREPS 100
#define MAXDPUS 2560

static const unsigned long DEFAULT_DPUS[] = {64, 65, 128, 199, 256, 512, 1024};
static const unsigned long DEFAULT_SIZES[] = {512, 32768, 262144};

int main(void)
{
    struct dpu_set_t set, dpu, rank;
    unsigned long dpus[16], sizes[16];
    unsigned ndpus = parse_list(getenv("XDPUS"), dpus, 16, DEFAULT_DPUS, 7);
    unsigned nsizes = parse_list(getenv("XSIZES"), sizes, 16, DEFAULT_SIZES, 3);
    const char *binary = getenv("XBIN") ? getenv("XBIN") : "/tmp/xfer_dpu";
    size_t maxbytes = 0;
    double t[MAXREPS];
    char **buf;
    uint32_t i, r;

    for (unsigned s = 0; s < nsizes; s++)
        if (sizes[s] > maxbytes) maxbytes = sizes[s];
    buf = malloc(MAXDPUS * sizeof(char *));
    for (i = 0; i < MAXDPUS; i++) {
        buf[i] = malloc(maxbytes);
        memset(buf[i], (int)(i + 1), maxbytes);
    }

    printf("dpus\tranks\top\tbytes_per_dpu\tmedian_us\tmin_us\tfirst_us\n");

    for (unsigned d = 0; d < ndpus; d++) {
        uint32_t nd = (uint32_t)dpus[d], nr = 0, got = 0;

        if (dpu_alloc(nd, getenv("XPROFILE"), &set) != DPU_OK) {
            fprintf(stderr, "dpu_alloc(%u) failed\n", nd);
            break;
        }
        DPU_ASSERT(dpu_load(set, binary, NULL));
        DPU_ASSERT(dpu_get_nr_ranks(set, &nr));
        DPU_ASSERT(dpu_get_nr_dpus(set, &got));

        printf("# set of %u: %u DPUs on %u ranks:", nd, got, nr);
        DPU_RANK_FOREACH (set, rank, r) {
            uint32_t in_rank = 0;
            DPU_ASSERT(dpu_get_nr_dpus(rank, &in_rank));
            printf(" %u", in_rank);
        }
        printf("\n");
        print_owned();

        double first = -1.0;
        for (unsigned s = 0; s < nsizes; s++) {
            size_t bytes = sizes[s];
            for (int op = 0; op < 3; op++) {
                int reps = (int)(256.0 * 1024 * 1024 / ((double)bytes * nd));
                if (reps < 5) reps = 5;
                if (reps > MAXREPS) reps = MAXREPS;

                for (int k = 0; k < reps; k++) {
                    double a = now_s();
                    if (op == 2) {
                        DPU_ASSERT(dpu_broadcast_to(set, DPU_MRAM_HEAP_POINTER_NAME, 0, buf[0], bytes,
                                                    DPU_XFER_DEFAULT));
                    } else {
                        DPU_FOREACH (set, dpu, i) { DPU_ASSERT(dpu_prepare_xfer(dpu, buf[i])); }
                        DPU_ASSERT(dpu_push_xfer(set, op == 0 ? DPU_XFER_TO_DPU : DPU_XFER_FROM_DPU,
                                                 DPU_MRAM_HEAP_POINTER_NAME, 0, bytes, DPU_XFER_DEFAULT));
                    }
                    t[k] = (now_s() - a) * 1e6;
                }
                if (first < 0) first = t[0];

                /* Drop the first repetition: it may pay for bringing ranks up. */
                qsort(t + 1, (size_t)(reps - 1), sizeof(double), cmp_double);
                printf("%u\t%u\t%s\t%zu\t%.1f\t%.1f\t%.1f\n", nd, nr,
                       op == 0 ? "write" : op == 1 ? "read" : "broadcast", bytes, t[1 + (reps - 1) / 2], t[1],
                       s == 0 && op == 0 ? first : t[0]);
            }
        }

        for (int k = 0; k < 50; k++) {
            double a = now_s();
            DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
            t[k] = (now_s() - a) * 1e6;
        }
        qsort(t + 1, 49, sizeof(double), cmp_double);
        printf("%u\t%u\tlaunch\t0\t%.1f\t%.1f\t%.1f\n", nd, nr, t[25], t[1], t[0]);
        fflush(stdout);

        DPU_ASSERT(dpu_free(set));
    }

    return 0;
}
