/* LD_PRELOAD wrapper for HARDWARE runs: reads a kernel's counter probe after
 * every launch, for a host program that does not read it itself.
 *
 * runner/tests/prim/probe_main.c puts the probe around an unchanged PrIM
 * kernel: the kernel holds PROBE_MODE (0 = cycles, 1 = instructions) and
 * leaves the count in OUT_COUNT. With PROBE_OUT set, this wrapper writes
 * PROBE_MODE to every DPU of the set before each launch (when PROBE_MODE is
 * set in the environment), and after the launch has finished appends one line
 * per DPU to the file PROBE_OUT:
 *
 *   <launch number, from 1>\t<DPU index in the set>\t<OUT_COUNT>
 *
 * The simulator needs no wrapper: the runner's shim reads PROBE_OUT and
 * PROBE_MODE itself and writes the same lines. Build it with:
 *
 *   gcc -O2 -shared -fPIC -o /tmp/liblaunch_probe.so \
 *       runner/scripts/launch_probe.c $(dpu-pkg-config --cflags dpu) -ldl
 *   LD_PRELOAD=/tmp/liblaunch_probe.so PROBE_OUT=counts.tsv ./host ...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned launches;
static int pending;
static struct dpu_set_t pending_set;

static void write_mode(struct dpu_set_t set)
{
    const char *mode_text = getenv("PROBE_MODE");
    if (getenv("PROBE_OUT") == NULL || mode_text == NULL || *mode_text == 0) return;

    uint32_t mode = (uint32_t)atoi(mode_text);
    dpu_error_t status = dpu_broadcast_to(set, "PROBE_MODE", 0, &mode, sizeof(mode), DPU_XFER_DEFAULT);
    if (status != DPU_OK) fprintf(stderr, "[launch_probe] cannot write PROBE_MODE\n");
}

static void read_counts(struct dpu_set_t set)
{
    const char *path = getenv("PROBE_OUT");
    struct dpu_set_t dpu;
    uint32_t i;

    if (path == NULL || *path == 0) return;
    FILE *out = fopen(path, "a");
    if (out == NULL) return;

    launches++;
    DPU_FOREACH (set, dpu, i) {
        uint32_t count = 0;
        if (dpu_copy_from(dpu, "OUT_COUNT", 0, &count, sizeof(count)) != DPU_OK) {
            fprintf(stderr, "[launch_probe] cannot read OUT_COUNT\n");
            break;
        }
        fprintf(out, "%u\t%u\t%u\n", launches, i, count);
    }
    fclose(out);
}

dpu_error_t dpu_launch(struct dpu_set_t set, dpu_launch_policy_t policy)
{
    static dpu_error_t (*real)(struct dpu_set_t, dpu_launch_policy_t);
    if (real == NULL) real = dlsym(RTLD_NEXT, "dpu_launch");

    write_mode(set);
    dpu_error_t status = real(set, policy);

    if (status == DPU_OK && policy == DPU_SYNCHRONOUS) {
        read_counts(set);
    } else if (status == DPU_OK) {
        pending = 1;
        pending_set = set;
    }
    return status;
}

dpu_error_t dpu_sync(struct dpu_set_t set)
{
    static dpu_error_t (*real)(struct dpu_set_t);
    if (real == NULL) real = dlsym(RTLD_NEXT, "dpu_sync");

    dpu_error_t status = real(set);
    if (status == DPU_OK && pending) {
        pending = 0;
        read_counts(pending_set);
    }
    return status;
}

dpu_error_t dpu_status(struct dpu_set_t set, bool *done, bool *fault)
{
    static dpu_error_t (*real)(struct dpu_set_t, bool *, bool *);
    if (real == NULL) real = dlsym(RTLD_NEXT, "dpu_status");

    dpu_error_t status = real(set, done, fault);
    if (status == DPU_OK && pending && done != NULL && *done) {
        pending = 0;
        read_counts(pending_set);
    }
    return status;
}
