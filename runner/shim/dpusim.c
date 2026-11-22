/* libdpusim -- a stand-in for libdpu that runs the DPUs in uPIMulator.
 *
 * LD_PRELOAD this in front of an unmodified UPMEM host binary and every DPU
 * operation is served by the cycle-level simulator instead of the hardware.
 *
 * It includes the real <dpu.h>, so the structures and enums are the SDK's own
 * rather than copies that could drift. It does not link libdpu: that is what
 * makes a simulated run structurally incapable of touching a rank.
 *
 * Configuration comes from the environment, set by the upimulator launcher:
 *
 *   UPIM_PROGRAM_DIR    bin directory of a uPIMulator linker run, holding the
 *                       DPU image and its symbol tables. Unset, the kernel is
 *                       the DPU ELF the host passes to dpu_load, which is
 *                       what the machine runs.
 *   UPIM_NUM_TASKLETS   tasklets per DPU; must match how the image was built
 *   UPIM_NUM_DPUS       DPU count to use when the program asks for
 *                       DPU_ALLOCATE_ALL
 *   UPIM_MAX_CYCLES     per-launch cycle cap; 0 means no limit
 *   UPIM_VERBOSE        1 to trace each intercepted call on stderr
 */

#include <dpu.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libupimulator.h"

/* ---------------------------------------------------------------- state --- */

struct shim_set;

typedef struct {
    struct shim_set *set;
    uint32_t index;
} shim_dpu_t;

struct shim_set {
    int handle; /* -1 until dpu_load when the kernel is the host's ELF */
    uint32_t nr_dpus;
    long num_tasklets;

    shim_dpu_t *dpus;

    /* One staged buffer per DPU, filled by dpu_prepare_xfer and consumed by the
     * next dpu_push_xfer. */
    void **xfer;

    int launched;
};

static int verbose;

static void trace(const char *format, ...)
{
    if (!verbose) {
        return;
    }

    va_list args;
    va_start(args, format);
    fprintf(stderr, "[upimulator] ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}

/* fail reports the simulator's own message, which is far more useful than the
 * dpu_error_t the host will print. */
static dpu_error_t fail(dpu_error_t status, const char *what)
{
    char *reason = upim_last_error();

    fprintf(stderr, "[upimulator] %s failed: %s\n", what, reason ? reason : "unknown");

    free(reason);
    return status;
}

static long env_long(const char *name, long fallback)
{
    const char *value = getenv(name);

    if (value == NULL || *value == '\0') {
        return fallback;
    }

    return strtol(value, NULL, 10);
}

/* ------------------------------------------------------------- set glue --- */

/* A whole set is carried in the rank arm of the union, a single DPU in the dpu
 * arm. Both hold pointers we allocated, so the host may copy dpu_set_t values
 * around freely, which it does: every one of these calls takes it by value. */

static struct dpu_set_t set_of(struct shim_set *set)
{
    struct dpu_set_t dpu_set;

    memset(&dpu_set, 0, sizeof(dpu_set));
    dpu_set.kind = DPU_SET_RANKS;
    dpu_set.list.nr_ranks = 1;
    dpu_set.list.ranks = (struct dpu_rank_t **)set;

    return dpu_set;
}

static struct dpu_set_t set_of_dpu(struct shim_set *set, uint32_t index)
{
    struct dpu_set_t dpu_set;

    memset(&dpu_set, 0, sizeof(dpu_set));
    dpu_set.kind = DPU_SET_DPU;
    dpu_set.dpu = (struct dpu_t *)&set->dpus[index];

    return dpu_set;
}

static struct shim_set *shim_set_of(struct dpu_set_t dpu_set)
{
    if (dpu_set.kind == DPU_SET_RANKS) {
        return (struct shim_set *)dpu_set.list.ranks;
    }

    shim_dpu_t *dpu = (shim_dpu_t *)dpu_set.dpu;
    return dpu ? dpu->set : NULL;
}

/* dpu_index_of returns the DPU a set refers to, or -1 when it covers them all. */
static int dpu_index_of(struct dpu_set_t dpu_set)
{
    if (dpu_set.kind == DPU_SET_DPU) {
        shim_dpu_t *dpu = (shim_dpu_t *)dpu_set.dpu;
        return dpu ? (int)dpu->index : -1;
    }

    return -1;
}

static void free_set(struct shim_set *set)
{
    free(set->dpus);
    free(set->xfer);
    free(set);
}

/* ------------------------------------------------------- allocate / free --- */

dpu_error_t dpu_alloc(uint32_t nr_dpus, const char *profile, struct dpu_set_t *dpu_set)
{
    (void)profile; /* the backend is this library; there is nothing to select */

    verbose = (int)env_long("UPIM_VERBOSE", 0);

    const char *program_dir = getenv("UPIM_PROGRAM_DIR");

    if (getenv("UPIM_NUM_TASKLETS") == NULL) {
        fprintf(stderr, "[upimulator] UPIM_NUM_TASKLETS is not set; "
                        "run this program through the upimulator launcher\n");
        return DPU_ERR_ALLOCATION;
    }

    if (nr_dpus == DPU_ALLOCATE_ALL) {
        nr_dpus = (uint32_t)env_long("UPIM_NUM_DPUS", 1);
        trace("DPU_ALLOCATE_ALL requested, using %u DPUs", nr_dpus);
    }

    long num_tasklets = env_long("UPIM_NUM_TASKLETS", 1);

    struct shim_set *set = calloc(1, sizeof(*set));

    if (set == NULL) {
        return DPU_ERR_SYSTEM;
    }

    set->num_tasklets = num_tasklets;
    set->handle = -1;

    /* With a linker run the image is known now. Without one it is the ELF the
     * host names in dpu_load, and the set is built there. */
    if (program_dir != NULL) {
        set->handle = upim_set_create((char *)program_dir, (int)nr_dpus, (int)num_tasklets);

        if (set->handle < 0) {
            free_set(set);
            return fail(DPU_ERR_ALLOCATION, "dpu_alloc");
        }
    }

    set->nr_dpus = nr_dpus;
    set->dpus = calloc(nr_dpus, sizeof(*set->dpus));
    set->xfer = calloc(nr_dpus, sizeof(*set->xfer));

    if (set->dpus == NULL || set->xfer == NULL) {
        upim_set_free(set->handle);
        free_set(set);
        return DPU_ERR_SYSTEM;
    }

    for (uint32_t i = 0; i < nr_dpus; i++) {
        set->dpus[i].set = set;
        set->dpus[i].index = i;
    }

    *dpu_set = set_of(set);

    trace("dpu_alloc: %u DPUs, %ld tasklets, program %s", nr_dpus, num_tasklets,
          program_dir != NULL ? program_dir : "(the ELF given to dpu_load)");
    return DPU_OK;
}

dpu_error_t dpu_alloc_ranks(uint32_t nr_ranks, const char *profile, struct dpu_set_t *dpu_set)
{
    /* A simulated rank holds whatever width was asked for, so rank granularity
     * is not meaningful here. Treat it as a DPU count. */
    return dpu_alloc(nr_ranks, profile, dpu_set);
}

/* report_stats prints the per-DPU counters the study compares against hardware.
 * logic_cycle is the DPU cycle count, directly comparable to a perfcounter
 * reading; num_instructions is frequency independent and so a tighter check. */
static void report_stats(struct shim_set *set)
{
    const char *path = getenv("UPIM_STATS");

    if (path == NULL) {
        return;
    }

    FILE *out = (strcmp(path, "-") == 0) ? stderr : fopen(path, "w");

    if (out == NULL) {
        return;
    }

    /* The last four say where the cycles went: a cycle either issued an
     * instruction (breakdown_run), found every ready thread waiting on a
     * transfer (breakdown_dma), or found nothing to issue for another reason
     * (breakdown_etc). cycle_rule counts the extra cycles the register file
     * charged. They are what tells a memory problem apart from a scheduling
     * one. */
    static const char *const names[] = {
        "logic_cycle", "num_instructions", "memory_cycle",
        "breakdown_run", "breakdown_dma", "breakdown_etc", "cycle_rule",
    };
    const unsigned nnames = sizeof(names) / sizeof(names[0]);

    fprintf(out, "dpu");
    for (unsigned n = 0; n < nnames; n++) {
        fprintf(out, "\t%s", names[n]);
    }
    fprintf(out, "\n");

    for (uint32_t i = 0; i < set->nr_dpus; i++) {
        fprintf(out, "%u", i);
        for (unsigned n = 0; n < nnames; n++) {
            fprintf(out, "\t%lld",
                upim_stat(set->handle, (int)i, (char *)names[n], 0));
        }
        fprintf(out, "\n");
    }

    /* Host transfers belong to the set, not to any DPU: one transfer covers
     * several at once and no DPU counter sees it. They do not overlap a kernel
     * here, so the two simply add up. */
    fprintf(out, "\ntransfer_cycle\ttransfer_to_bytes\ttransfer_from_bytes\ttransfer_count\n");
    fprintf(out, "%lld\t%lld\t%lld\t%lld\n",
        upim_transfer_stat(set->handle, (char *)"transfer_cycle"),
        upim_transfer_stat(set->handle, (char *)"transfer_to_bytes"),
        upim_transfer_stat(set->handle, (char *)"transfer_from_bytes"),
        upim_transfer_stat(set->handle, (char *)"transfer_count"));

    if (out != stderr) {
        fclose(out);
    }
}

dpu_error_t dpu_free(struct dpu_set_t dpu_set)
{
    struct shim_set *set = shim_set_of(dpu_set);

    if (set == NULL) {
        return DPU_ERR_INTERNAL;
    }

    trace("dpu_free");

    /* A set the host never loaded has nothing in the simulator to free. */
    if (set->handle < 0) {
        free_set(set);
        return DPU_OK;
    }

    report_stats(set);

    /* Hosts usually read the DPU log only on a failure path, which hides the
     * kernel's own diagnostics when something is wrong. UPIM_DUMP_LOG=1 prints
     * it unconditionally. */
    if (env_long("UPIM_DUMP_LOG", 0)) {
        fprintf(stderr, "[upimulator] --- DPU log ---\n");
        dpu_log_read(dpu_set, stderr);
        fprintf(stderr, "[upimulator] --- end of DPU log ---\n");
    }

    if (upim_set_free(set->handle) != 0) {
        return fail(DPU_ERR_INTERNAL, "dpu_free");
    }

    free_set(set);

    return DPU_OK;
}

dpu_error_t dpu_get_nr_dpus(struct dpu_set_t dpu_set, uint32_t *nr_dpus)
{
    struct shim_set *set = shim_set_of(dpu_set);

    if (set == NULL) {
        return DPU_ERR_INTERNAL;
    }

    *nr_dpus = (dpu_set.kind == DPU_SET_DPU) ? 1 : set->nr_dpus;
    return DPU_OK;
}

dpu_error_t dpu_get_nr_ranks(struct dpu_set_t dpu_set, uint32_t *nr_ranks)
{
    (void)dpu_set;
    *nr_ranks = 1;
    return DPU_OK;
}

/* ------------------------------------------------------------------ load --- */

dpu_error_t dpu_load(struct dpu_set_t dpu_set, const char *binary_path, struct dpu_program_t **program)
{
    if (program != NULL) {
        *program = NULL;
    }

    /* With UPIM_PROGRAM_DIR the image was built by uPIMulator's own linker and
     * installed when the set was created; the path is recorded for the log
     * only. Point a host at a different kernel and UPIM_PROGRAM_DIR must
     * change with it, or the run measures the wrong kernel. */
    if (getenv("UPIM_PROGRAM_DIR") != NULL) {
        trace("dpu_load: %s (serving UPIM_PROGRAM_DIR instead)", binary_path ? binary_path : "(null)");
        return DPU_OK;
    }

    /* Without it the kernel is this ELF, the file the machine would run. A
     * second load starts the DPUs afresh, as it does on the machine.
     * DPU_LOAD_OVERRIDE names another ELF instead, as
     * runner/scripts/dpuload_override.c does for a hardware run, so one
     * setting points both sides at the same kernel. */
    struct shim_set *set = shim_set_of(dpu_set);
    const char *override = getenv("DPU_LOAD_OVERRIDE");

    if (override != NULL && *override != 0) {
        trace("dpu_load: %s -> %s (DPU_LOAD_OVERRIDE)", binary_path ? binary_path : "(null)", override);
        binary_path = override;
    }

    if (set == NULL || binary_path == NULL) {
        return DPU_ERR_INVALID_DPU_SET;
    }

    if (set->handle >= 0) {
        upim_set_free(set->handle);
        set->handle = -1;
    }

    set->handle = upim_set_create((char *)binary_path, (int)set->nr_dpus, (int)set->num_tasklets);

    if (set->handle < 0) {
        return fail(DPU_ERR_ELF_INVALID_FILE, "dpu_load");
    }

    set->launched = 0;
    trace("dpu_load: %s", binary_path);
    return DPU_OK;
}

/* -------------------------------------------------------------- iterate --- */

struct dpu_set_dpu_iterator_t dpu_set_dpu_iterator_from(struct dpu_set_t *set)
{
    struct dpu_set_dpu_iterator_t iterator;

    memset(&iterator, 0, sizeof(iterator));
    iterator.rank_iterator.set = set;

    struct shim_set *shim = set ? shim_set_of(*set) : NULL;

    if (shim == NULL || shim->nr_dpus == 0) {
        iterator.has_next = false;
        return iterator;
    }

    /* A single-DPU set iterates over just itself. */
    if (set->kind == DPU_SET_DPU) {
        iterator.count = 0;
        iterator.next_idx = (uint32_t)dpu_index_of(*set);
        iterator.next = *set;
        iterator.has_next = true;
        return iterator;
    }

    iterator.count = 0;
    iterator.next_idx = 0;
    iterator.next = set_of_dpu(shim, 0);
    iterator.has_next = true;

    return iterator;
}

void dpu_set_dpu_iterator_next(struct dpu_set_dpu_iterator_t *iterator)
{
    struct dpu_set_t *set = iterator->rank_iterator.set;
    struct shim_set *shim = set ? shim_set_of(*set) : NULL;

    if (shim == NULL || set->kind == DPU_SET_DPU) {
        iterator->has_next = false;
        return;
    }

    iterator->next_idx++;

    if (iterator->next_idx >= shim->nr_dpus) {
        iterator->has_next = false;
        return;
    }

    iterator->count = iterator->next_idx;
    iterator->next = set_of_dpu(shim, iterator->next_idx);
    iterator->has_next = true;
}

struct dpu_set_rank_iterator_t dpu_set_rank_iterator_from(struct dpu_set_t *set)
{
    struct dpu_set_rank_iterator_t iterator;

    memset(&iterator, 0, sizeof(iterator));
    iterator.set = set;
    iterator.count = 0;
    iterator.next_idx = 0;

    if (set != NULL) {
        iterator.next = *set;
        iterator.has_next = true;
    }

    return iterator;
}

void dpu_set_rank_iterator_next(struct dpu_set_rank_iterator_t *iterator)
{
    /* One simulated rank holds every DPU, so the walk ends after the first. */
    iterator->has_next = false;
}

/* ------------------------------------------------------------ transfers --- */

static dpu_error_t copy_one(struct shim_set *set,
    uint32_t index,
    const char *symbol_name,
    uint32_t symbol_offset,
    void *buffer,
    size_t length,
    int to_dpu)
{
    long long address = upim_resolve(set->handle, (char *)symbol_name, (long long)symbol_offset);

    if (address < 0) {
        return fail(DPU_ERR_INVALID_SYMBOL_ACCESS, "resolving a symbol");
    }

    int rc = to_dpu ? upim_copy_to(set->handle, (int)index, address, buffer, (int)length)
                    : upim_copy_from(set->handle, (int)index, address, buffer, (int)length);

    if (rc != 0) {
        return fail(DPU_ERR_MRAM_BUSY, to_dpu ? "dpu_copy_to" : "dpu_copy_from");
    }

    return DPU_OK;
}

dpu_error_t dpu_copy_to(
    struct dpu_set_t dpu_set, const char *symbol_name, uint32_t symbol_offset, const void *src, size_t length)
{
    struct shim_set *set = shim_set_of(dpu_set);

    if (set == NULL) {
        return DPU_ERR_INTERNAL;
    }

    int index = dpu_index_of(dpu_set);

    trace("dpu_copy_to: %s+%u, %zu bytes, dpu %d", symbol_name, symbol_offset, length, index);

    /* A whole-set copy broadcasts the same bytes to every DPU. */
    if (index < 0) {
        for (uint32_t i = 0; i < set->nr_dpus; i++) {
            dpu_error_t status = copy_one(set, i, symbol_name, symbol_offset, (void *)src, length, 1);

            if (status != DPU_OK) {
                return status;
            }
        }

        /* One charge for the batch: the DPUs of a rank transfer together, and
         * charging each in turn would serialise what the hardware overlaps. */
        upim_charge_transfer(set->handle, (int)set->nr_dpus, (long long)length, 1);
        return DPU_OK;
    }

    {
        dpu_error_t status
            = copy_one(set, (uint32_t)index, symbol_name, symbol_offset, (void *)src, length, 1);

        if (status == DPU_OK) {
            upim_charge_transfer(set->handle, 1, (long long)length, 1);
        }

        return status;
    }
}

dpu_error_t dpu_copy_from(
    struct dpu_set_t dpu_set, const char *symbol_name, uint32_t symbol_offset, void *dst, size_t length)
{
    struct shim_set *set = shim_set_of(dpu_set);

    if (set == NULL) {
        return DPU_ERR_INTERNAL;
    }

    int index = dpu_index_of(dpu_set);

    trace("dpu_copy_from: %s+%u, %zu bytes, dpu %d", symbol_name, symbol_offset, length, index);

    /* Reading a whole set into one buffer is ambiguous, so take DPU 0, which is
     * what the hardware library does for a single-buffer read. */
    {
        dpu_error_t status = copy_one(
            set, index < 0 ? 0 : (uint32_t)index, symbol_name, symbol_offset, dst, length, 0);

        if (status == DPU_OK) {
            upim_charge_transfer(set->handle, 1, (long long)length, 0);
        }

        return status;
    }
}

dpu_error_t dpu_prepare_xfer(struct dpu_set_t dpu_set, void *buffer)
{
    struct shim_set *set = shim_set_of(dpu_set);

    if (set == NULL) {
        return DPU_ERR_INTERNAL;
    }

    int index = dpu_index_of(dpu_set);

    if (index < 0) {
        /* Staging one buffer for the whole set means every DPU reads the same
         * host memory. */
        for (uint32_t i = 0; i < set->nr_dpus; i++) {
            set->xfer[i] = buffer;
        }

        return DPU_OK;
    }

    set->xfer[index] = buffer;
    return DPU_OK;
}

dpu_error_t dpu_push_xfer(struct dpu_set_t dpu_set,
    dpu_xfer_t xfer,
    const char *symbol_name,
    uint32_t symbol_offset,
    size_t length,
    dpu_xfer_flags_t flags)
{
    (void)flags;

    struct shim_set *set = shim_set_of(dpu_set);

    if (set == NULL) {
        return DPU_ERR_INTERNAL;
    }

    trace("dpu_push_xfer: %s %s+%u, %zu bytes",
        xfer == DPU_XFER_TO_DPU ? "to" : "from", symbol_name, symbol_offset, length);

    uint32_t staged = 0;

    for (uint32_t i = 0; i < set->nr_dpus; i++) {
        if (set->xfer[i] == NULL) {
            continue;
        }

        dpu_error_t status
            = copy_one(set, i, symbol_name, symbol_offset, set->xfer[i], length, xfer == DPU_XFER_TO_DPU);

        if (status != DPU_OK) {
            return status;
        }

        staged++;
    }

    /* One charge for the whole push. This is the bulk path, and the DPUs of a
     * rank move their data at the same time; charging per DPU would report a
     * rank as slow as a single DPU repeated. */
    upim_charge_transfer(set->handle, (int)staged, (long long)length, xfer == DPU_XFER_TO_DPU);

    /* When tracing a read, report how much of it was non-zero. An all-zero
     * result usually means the kernel wrote somewhere else, which is otherwise
     * very hard to tell apart from a transfer that simply failed. */
    if (verbose && xfer == DPU_XFER_FROM_DPU) {
        for (uint32_t i = 0; i < set->nr_dpus; i++) {
            if (set->xfer[i] == NULL) {
                continue;
            }

            size_t nonzero = 0;
            const unsigned char *bytes = set->xfer[i];

            for (size_t j = 0; j < length; j++) {
                if (bytes[j] != 0) {
                    nonzero++;
                }
            }

            trace("  dpu %u read back %zu of %zu bytes non-zero", i, nonzero, length);
        }
    }

    /* Staged buffers apply to one push only. */
    memset(set->xfer, 0, set->nr_dpus * sizeof(*set->xfer));

    return DPU_OK;
}

dpu_error_t dpu_broadcast_to(struct dpu_set_t dpu_set,
    const char *symbol_name,
    uint32_t symbol_offset,
    const void *src,
    size_t length,
    dpu_xfer_flags_t flags)
{
    (void)flags;
    return dpu_copy_to(dpu_set, symbol_name, symbol_offset, src, length);
}

/* ---------------------------------------------------------------- run --- */

static dpu_error_t run_to_completion(struct shim_set *set)
{
    if (!set->launched) {
        return DPU_OK;
    }

    if (upim_run(set->handle, (long long)env_long("UPIM_MAX_CYCLES", 0)) != 0) {
        return fail(DPU_ERR_INTERNAL, "running the kernel");
    }

    set->launched = 0;
    return DPU_OK;
}

dpu_error_t dpu_launch(struct dpu_set_t dpu_set, dpu_launch_policy_t policy)
{
    struct shim_set *set = shim_set_of(dpu_set);

    if (set == NULL) {
        return DPU_ERR_INTERNAL;
    }

    trace("dpu_launch: %s", policy == DPU_SYNCHRONOUS ? "synchronous" : "asynchronous");

    if (upim_launch(set->handle) != 0) {
        return fail(DPU_ERR_INTERNAL, "dpu_launch");
    }

    set->launched = 1;

    if (policy == DPU_SYNCHRONOUS) {
        return run_to_completion(set);
    }

    return DPU_OK;
}

dpu_error_t dpu_status(struct dpu_set_t dpu_set, bool *done, bool *fault)
{
    struct shim_set *set = shim_set_of(dpu_set);

    if (set == NULL) {
        return DPU_ERR_INTERNAL;
    }

    /* Hosts poll this in a tight loop after an asynchronous launch, and nothing
     * else would advance the simulation, so the first poll runs the kernel to
     * completion. The host sees one long poll rather than many short ones. */
    dpu_error_t status = run_to_completion(set);

    if (status != DPU_OK) {
        if (fault != NULL) {
            *fault = true;
        }

        return status;
    }

    if (done != NULL) {
        *done = true;
    }

    if (fault != NULL) {
        *fault = false;
    }

    return DPU_OK;
}

dpu_error_t dpu_sync(struct dpu_set_t dpu_set)
{
    struct shim_set *set = shim_set_of(dpu_set);

    if (set == NULL) {
        return DPU_ERR_INTERNAL;
    }

    trace("dpu_sync");
    return run_to_completion(set);
}

/* ------------------------------------------------------------ reporting --- */

/* resolve_any returns the address of the first symbol that exists. The DPU
 * runtime's own symbols are local, so the linker prefixes them with the
 * relocatable they came from. */
static long long resolve_any(struct shim_set *set, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        long long address = upim_resolve(set->handle, (char *)names[i], 0);

        if (address >= 0) {
            return address;
        }
    }

    return -1;
}

dpu_error_t dpu_log_read(struct dpu_set_t dpu_set, FILE *stream)
{
    struct shim_set *set = shim_set_of(dpu_set);

    if (set == NULL) {
        return DPU_ERR_INTERNAL;
    }

    if (stream == NULL) {
        stream = stdout;
    }

    static const char *const state_names[] = { "__stdout_buffer_state", "data.immediate_memory.__stdout_buffer_state" };
    static const char *const buffer_names[] = { "stdlib.stdio.__stdout_buffer", "__stdout_buffer" };

    long long state_address = resolve_any(set, state_names, 2);
    long long buffer_address = resolve_any(set, buffer_names, 2);

    if (state_address < 0 || buffer_address < 0) {
        /* A program that never calls printf has no buffer; that is not an error. */
        return DPU_OK;
    }

    int first = (dpu_set.kind == DPU_SET_DPU) ? dpu_index_of(dpu_set) : 0;
    int last = (dpu_set.kind == DPU_SET_DPU) ? first + 1 : (int)set->nr_dpus;

    for (int i = first; i < last; i++) {
        /* struct { uint32_t wp; uint32_t has_wrapped; } */
        uint32_t state[2] = { 0, 0 };

        if (upim_copy_from(set->handle, i, state_address, state, (int)sizeof(state)) != 0) {
            return fail(DPU_ERR_MRAM_BUSY, "reading the DPU log state");
        }

        uint32_t written = state[0];

        if (written == 0) {
            continue;
        }

        char *text = calloc(written + 1, 1);

        if (text == NULL) {
            return DPU_ERR_SYSTEM;
        }

        if (upim_copy_from(set->handle, i, buffer_address, text, (int)written) != 0) {
            free(text);
            return fail(DPU_ERR_MRAM_BUSY, "reading the DPU log");
        }

        /* The DPU writes its own newlines, so emit the bytes as they are. */
        fwrite(text, 1, written, stream);
        free(text);
    }

    fflush(stream);
    return DPU_OK;
}

char *dpu_error_to_string(dpu_error_t status)
{
    /* The caller frees the string, as libdpu's DPU_CHECK does, so it must be
     * allocated: a string literal here aborts every failing DPU_ASSERT. */
    const char *text;

    switch (status) {
    case DPU_OK:
        text = "success";
        break;
    case DPU_ERR_INTERNAL:
        text = "internal error (simulated)";
        break;
    case DPU_ERR_SYSTEM:
        text = "system error (simulated)";
        break;
    case DPU_ERR_ALLOCATION:
        text = "cannot allocate the simulated DPUs";
        break;
    case DPU_ERR_INVALID_SYMBOL_ACCESS:
        text = "invalid symbol access (simulated)";
        break;
    case DPU_ERR_MRAM_BUSY:
        text = "DPU memory is busy (simulated)";
        break;
    case DPU_ERR_INVALID_DPU_SET:
        text = "invalid DPU set (simulated)";
        break;
    case DPU_ERR_ELF_INVALID_FILE:
        text = "invalid ELF file (simulated)";
        break;
    default:
        text = "unknown error (simulated)";
        break;
    }

    return strdup(text);
}

