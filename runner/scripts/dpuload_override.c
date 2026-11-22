/* LD_PRELOAD wrapper for HARDWARE runs: when DPU_LOAD_OVERRIDE is set,
 * dpu_load loads that DPU binary instead of the one the host asks for.
 *
 * A timing comparison is only valid if the machine and the simulator run the
 * same kernel code. GIMSA's host loads its kernel from a path compiled into
 * it, and the binary found there on 2026-09-29 was not what the Makefile
 * builds today (main_kernel1 594 instructions against 564). To compare one
 * build: make the ELF once with the Makefile's own command, and point both
 * sides at it with DPU_LOAD_OVERRIDE. For the machine, build this wrapper:
 *
 *   gcc -O2 -shared -fPIC -o /tmp/libdpuload_override.so \
 *       runner/scripts/dpuload_override.c $(dpu-pkg-config --cflags dpu) -ldl
 *   LD_PRELOAD=/tmp/libdpuload_override.so DPU_LOAD_OVERRIDE=<kernel ELF> ./host ...
 *
 * The simulator needs no wrapper: without --program-dir the runner's shim runs
 * the ELF the host loads, and reads DPU_LOAD_OVERRIDE itself. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <dpu.h>

dpu_error_t dpu_load(struct dpu_set_t set, const char *path, struct dpu_program_t **program)
{
    static dpu_error_t (*real)(struct dpu_set_t, const char *, struct dpu_program_t **);
    const char *override = getenv("DPU_LOAD_OVERRIDE");

    if (real == NULL) real = dlsym(RTLD_NEXT, "dpu_load");
    if (override != NULL && *override != 0) {
        fprintf(stderr, "[dpuload_override] %s -> %s\n", path, override);
        path = override;
    }
    return real(set, path, program);
}
