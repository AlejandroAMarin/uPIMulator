/* Checks what a set that spans several ranks does, on the machine and in the
 * simulator; run.sh compares the two outputs line by line.
 *
 *   host <DPU ELF> [DPUs]
 *
 * It allocates the set (199 DPUs by default, 4 ranks on the machine) and
 * prints how the DPUs lie on the ranks. Then it copies a value to each rank
 * on its own, pushes a value to each DPU rank by rank, pushes to rank 0 only
 * with buffers staged on every DPU, launches rank by rank, launches rank 1
 * once more on its own, and checks every DPU's result. Last, it checks
 * dpu_alloc_ranks and a single DPU's rank count. XPROFILE picks the backend.
 */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    struct dpu_set_t set, rank, dpu;
    uint32_t nr_dpus = argc > 2 ? (uint32_t)atoi(argv[2]) : 199;
    uint32_t got, nr_ranks, r, i;
    uint32_t *rank_of, *first_of;
    uint64_t *pushed;
    unsigned errors = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <DPU ELF> [DPUs]\n", argv[0]);
        return 2;
    }

    DPU_ASSERT(dpu_alloc(nr_dpus, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, argv[1], NULL));
    DPU_ASSERT(dpu_get_nr_dpus(set, &got));
    DPU_ASSERT(dpu_get_nr_ranks(set, &nr_ranks));
    printf("set: %u DPUs on %u ranks\n", got, nr_ranks);

    rank_of = calloc(got, sizeof(*rank_of));
    first_of = calloc(nr_ranks, sizeof(*first_of));
    pushed = calloc(got, sizeof(*pushed));

    /* The layout, and DPU_FOREACH over one rank counting from 0. */
    uint32_t global = 0;
    DPU_RANK_FOREACH (set, rank, r) {
        uint32_t in_rank, seen = 0, ranks_of_rank;
        DPU_ASSERT(dpu_get_nr_dpus(rank, &in_rank));
        DPU_ASSERT(dpu_get_nr_ranks(rank, &ranks_of_rank));
        first_of[r] = global;
        DPU_FOREACH (rank, dpu, i) {
            if (i != seen) errors++;
            rank_of[global++] = r;
            seen++;
        }
        printf("rank %u: %u DPUs, iterated %u, nr_ranks %u\n", r, in_rank, seen, ranks_of_rank);
    }
    if (global != got) {
        printf("ERROR: the ranks hold %u DPUs, the set %u\n", global, got);
        errors++;
    }

    /* A copy to one rank reaches that rank only. */
    DPU_RANK_FOREACH (set, rank, r) {
        uint32_t value = 100 + r;
        DPU_ASSERT(dpu_copy_to(rank, "in_value", 0, &value, sizeof(value)));
    }

    /* A push to one rank, with its own DPUs staged. */
    for (i = 0; i < got; i++) pushed[i] = 1000 + i;
    global = 0;
    DPU_RANK_FOREACH (set, rank, r) {
        DPU_FOREACH (rank, dpu, i) { DPU_ASSERT(dpu_prepare_xfer(dpu, &pushed[global++])); }
        DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "pushed", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
    }

    /* A push to rank 0 while every DPU of the set is staged moves rank 0's. */
    uint64_t *other = calloc(got, sizeof(*other));
    for (i = 0; i < got; i++) other[i] = 5000 + i;
    DPU_FOREACH (set, dpu, i) { DPU_ASSERT(dpu_prepare_xfer(dpu, &other[i])); }
    DPU_RANK_FOREACH (set, rank, r) {
        if (r == 0) {
            DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "pushed", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
        }
    }
    for (i = 0; i < got; i++)
        if (rank_of[i] == 0) pushed[i] = other[i];

    /* Launch rank by rank, wait for the set, then launch rank 1 alone. */
    DPU_RANK_FOREACH (set, rank, r) { DPU_ASSERT(dpu_launch(rank, DPU_ASYNCHRONOUS)); }
    DPU_ASSERT(dpu_sync(set));
    if (nr_ranks > 1) {
        DPU_RANK_FOREACH (set, rank, r) {
            if (r == 1) DPU_ASSERT(dpu_launch(rank, DPU_SYNCHRONOUS));
        }
    }

    /* Every DPU's result. */
    uint64_t sum_out = 0, sum_launches = 0;
    DPU_FOREACH (set, dpu, i) {
        uint32_t out, launches;
        uint32_t want_launches = (nr_ranks > 1 && rank_of[i] == 1) ? 2 : 1;
        uint32_t want_out = (100 + rank_of[i]) * 2 + 1 + (uint32_t)pushed[i];
        DPU_ASSERT(dpu_copy_from(dpu, "out_value", 0, &out, sizeof(out)));
        DPU_ASSERT(dpu_copy_from(dpu, "launches", 0, &launches, sizeof(launches)));
        if (out != want_out || launches != want_launches) {
            if (errors < 10)
                printf("ERROR: DPU %u (rank %u): out %u launches %u, expected %u and %u\n", i, rank_of[i], out,
                       launches, want_out, want_launches);
            errors++;
        }
        sum_out += out;
        sum_launches += launches;
    }
    printf("results: sum of outputs %llu, launches %llu\n", (unsigned long long)sum_out,
           (unsigned long long)sum_launches);

    /* One DPU is one rank. */
    DPU_FOREACH (set, dpu, i) {
        uint32_t one, ranks;
        DPU_ASSERT(dpu_get_nr_dpus(dpu, &one));
        DPU_ASSERT(dpu_get_nr_ranks(dpu, &ranks));
        if (one != 1 || ranks != 1) errors++;
        break;
    }
    DPU_ASSERT(dpu_free(set));

    /* Whole ranks. */
    DPU_ASSERT(dpu_alloc_ranks(2, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_get_nr_dpus(set, &got));
    DPU_ASSERT(dpu_get_nr_ranks(set, &nr_ranks));
    printf("dpu_alloc_ranks(2): %u DPUs on %u ranks\n", got, nr_ranks);
    DPU_ASSERT(dpu_free(set));

    printf("%s: %u errors\n", errors ? "FAIL" : "PASS", errors);
    return errors ? 1 : 0;
}
