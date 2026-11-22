/* Drives mix_kernel.c: scatter_host.c with a write after every read.
 *
 *   XWSIZES bytes written after each read, as "0,32,256"
 *
 * Mode names carry the write size, as "rand+w32". What follows is
 * scatter_host.c's description.
 *
 * Drives scatter_kernel.c over access modes, DMA sizes and gaps.
 *
 *   XMODES  modes to run, as "seq,stride,rand8m,rand,none"; none skips the read
 *   XSIZES  bytes per DMA, as "8,16,64"
 *   XGAPS   empty-loop iterations between reads, as "0,8,32"
 */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MB (1u << 20)
#define AMASK ((32 * MB - 1) & ~7u)

struct mode {
    const char *name;
    uint32_t rmask; /* 0 walks with the stride, else random bits */
    uint32_t stride; /* 0 means one transfer */
    uint32_t nodma;
};

static const struct mode MODES[] = {
    {"seq", 0, 0, 0},
    {"stride", 0, 65536, 0},
    {"rand8m", (8 * MB - 1) & ~7u, 0, 0},
    {"rand", AMASK, 0, 0},
    {"none", 0, 0, 1},
};
#define NMODES (sizeof(MODES) / sizeof(MODES[0]))

static unsigned parse(const char *env, const char *dflt, uint32_t *out, unsigned max)
{
    const char *spec = getenv(env);
    char buf[512], *tok;
    unsigned n = 0;

    snprintf(buf, sizeof(buf), "%s", spec && *spec ? spec : dflt);
    for (tok = strtok(buf, ","); tok != NULL && n < max; tok = strtok(NULL, ","))
        out[n++] = (uint32_t)atoi(tok);
    return n;
}

static int wanted(const char *name)
{
    const char *spec = getenv("XMODES");
    char buf[256], *tok;

    if (spec == NULL || *spec == 0) return 1;
    snprintf(buf, sizeof(buf), "%s", spec);
    for (tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ","))
        if (strcmp(tok, name) == 0) return 1;
    return 0;
}

static void set(struct dpu_set_t s, const char *sym, uint32_t v)
{
    struct dpu_set_t dpu;
    DPU_FOREACH (s, dpu) DPU_ASSERT(dpu_copy_to(dpu, sym, 0, &v, sizeof(v)));
}

int main(void)
{
    struct dpu_set_t s, dpu;
    uint32_t iters = (uint32_t)atoi(getenv("ITERS") ? getenv("ITERS") : "200");
    uint32_t nt = (uint32_t)atoi(getenv("TASKLETS") ? getenv("TASKLETS") : "1");
    uint32_t spread = (uint32_t)atoi(getenv("SPREAD") ? getenv("SPREAD") : "2048");
    uint32_t sizes[16], gaps[64], wsizes[8], cycles[24], instr[24];
    unsigned nw = parse("XWSIZES", "0", wsizes, 8), wi;
    unsigned nsizes = parse("XSIZES", "8,16,64", sizes, 16);
    unsigned ngaps = parse("XGAPS", "0", gaps, 64);
    unsigned m, si, gi, t;
    char name[64];

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &s));
    DPU_ASSERT(dpu_load(s, getenv("XBIN") ? getenv("XBIN") : "/tmp/mix_dpu", NULL));

    printf("mode\tbytes\tgap\tcycles_per_dma\tinstr_per_dma\n");
    for (m = 0; m < NMODES; m++) {
        if (!wanted(MODES[m].name)) continue;
        for (wi = 0; wi < nw; wi++)
        for (gi = 0; gi < ngaps; gi++) {
            for (si = 0; si < nsizes; si++) {
                double c = 0, n = 0;

                set(s, "SIZE", sizes[si]);
                set(s, "ITERS", iters);
                set(s, "STRIDE", MODES[m].rmask ? 0 : MODES[m].stride ? MODES[m].stride : sizes[si]);
                set(s, "RMASK", MODES[m].rmask);
                set(s, "AMASK", AMASK);
                set(s, "SPREAD", spread);
                set(s, "GAP", gaps[gi]);
                set(s, "NODMA", MODES[m].nodma);
                set(s, "WSIZE", wsizes[wi]);
                DPU_ASSERT(dpu_launch(s, DPU_SYNCHRONOUS));
                DPU_FOREACH (s, dpu) {
                    DPU_ASSERT(dpu_copy_from(dpu, "OUT_CYCLES", 0, cycles, (size_t)nt * 4));
                    DPU_ASSERT(dpu_copy_from(dpu, "OUT_INSTR", 0, instr, (size_t)nt * 4));
                }
                /* Every tasklet reports the same DPU-wide span. */
                for (t = 0; t < nt; t++) {
                    if (cycles[t] > c) c = cycles[t];
                    if (instr[t] > n) n = instr[t];
                }
                snprintf(name, sizeof(name), "%s+w%u", MODES[m].name, wsizes[wi]);
                printf("%s\t%u\t%u\t%.3f\t%.3f\n", name, sizes[si], gaps[gi],
                       c / ((double)iters * nt), n / ((double)iters * nt));
                fflush(stdout);
            }
        }
    }

    DPU_ASSERT(dpu_free(s));
    return 0;
}
