/* A host program written against the real UPMEM API, used to exercise the shim.
 *
 * It follows the same call sequence as a PrIM or BIMSA host: allocate, load,
 * scatter inputs with DPU_FOREACH plus prepare/push, launch, read results back,
 * free. Linked against -ldpu like any UPMEM program; LD_PRELOAD decides whether
 * those calls reach the hardware or the simulator.
 *
 * It takes the bin directory of a uPIMulator run and checks the DPU's output
 * against the expected bytes recorded there, so a pass means the simulated DPUs
 * computed the right answer through the shim, not merely that nothing crashed.
 *
 * Usage: va_host <bin-dir> <input-offset> <output-offset> [nr-dpus]
 *
 * With more than one DPU it gives every DPU the same input and checks each one
 * separately, which is what exercises DPU_FOREACH indexing in the shim.
 */

#include <dpu.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* read_byte_file loads one of the simulator's memory images, which store one
 * decimal byte per line rather than raw bytes. */
static uint8_t *read_byte_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "r");

    if (file == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }

    size_t capacity = 4096;
    size_t count = 0;
    uint8_t *data = malloc(capacity);

    int value;
    while (fscanf(file, "%d", &value) == 1) {
        if (count == capacity) {
            capacity *= 2;
            data = realloc(data, capacity);
        }

        data[count++] = (uint8_t)value;
    }

    fclose(file);

    *length = count;
    return data;
}

static char *join(const char *dir, const char *name)
{
    size_t length = strlen(dir) + strlen(name) + 2;
    char *path = malloc(length);
    snprintf(path, length, "%s/%s", dir, name);
    return path;
}

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s <bin-dir> <input-offset> <output-offset> [nr-dpus]\n", argv[0]);
        return 2;
    }

    uint32_t wanted_dpus = (argc == 5) ? (uint32_t)strtoul(argv[4], NULL, 10) : 1;

    const char *bin_dir = argv[1];
    uint32_t input_offset = (uint32_t)strtoul(argv[2], NULL, 10);
    uint32_t output_offset = (uint32_t)strtoul(argv[3], NULL, 10);

    char input_name[256];
    char output_name[256];
    snprintf(input_name, sizeof(input_name), "input_dpu_mram_heap_pointer_name_%u_0_0.bin", input_offset);
    snprintf(output_name, sizeof(output_name), "output_dpu_mram_heap_pointer_name_%u_0_0.bin", output_offset);

    size_t input_length = 0;
    size_t expected_length = 0;
    size_t arguments_length = 0;

    char *input_path = join(bin_dir, input_name);
    char *output_path = join(bin_dir, output_name);
    char *arguments_path = join(bin_dir, "input_DPU_INPUT_ARGUMENTS_0_0.bin");

    uint8_t *input = read_byte_file(input_path, &input_length);
    uint8_t *expected = read_byte_file(output_path, &expected_length);
    uint8_t *arguments = read_byte_file(arguments_path, &arguments_length);

    if (input == NULL || expected == NULL || arguments == NULL) {
        return 2;
    }

    printf("input %zu bytes, arguments %zu bytes, expected output %zu bytes\n",
        input_length, arguments_length, expected_length);

    struct dpu_set_t set;
    struct dpu_set_t dpu;
    uint32_t nr_dpus = 0;

    DPU_ASSERT(dpu_alloc(wanted_dpus, NULL, &set));
    DPU_ASSERT(dpu_load(set, "./bin/va", NULL));
    DPU_ASSERT(dpu_get_nr_dpus(set, &nr_dpus));

    printf("allocated %u DPU(s)\n", nr_dpus);

    /* Kernel arguments go to a named WRAM symbol, the same way every PrIM host
     * does it. */
    DPU_ASSERT(dpu_copy_to(set, "DPU_INPUT_ARGUMENTS", 0, arguments, arguments_length));

    /* Bulk input goes to the MRAM heap through the scatter path. */
    uint32_t i = 0;
    DPU_FOREACH (set, dpu, i) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, input));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, input_offset, input_length,
        DPU_XFER_DEFAULT));

    /* BIMSA launches asynchronously and then spins on dpu_status rather than
     * calling dpu_sync, so cover that path too. Set UPIM_TEST_ASYNC=1. */
    if (getenv("UPIM_TEST_ASYNC") != NULL) {
        bool done = false;
        bool fault = false;
        uint32_t polls = 0;

        DPU_ASSERT(dpu_launch(set, DPU_ASYNCHRONOUS));

        while (!done) {
            DPU_ASSERT(dpu_status(set, &done, &fault));

            if (fault) {
                fprintf(stderr, "FAIL: the DPU set reported a fault\n");
                return 1;
            }

            if (++polls > 1000000) {
                fprintf(stderr, "FAIL: dpu_status never reported done\n");
                return 1;
            }
        }

        printf("asynchronous launch completed after %u status poll(s)\n", polls);
    } else {
        DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
    }

    /* One landing buffer per DPU, so a mix-up in DPU_FOREACH indexing shows up
     * as a wrong answer rather than being hidden by a shared buffer. */
    uint8_t *results = calloc(nr_dpus * expected_length, 1);

    DPU_FOREACH (set, dpu, i) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, results + (size_t)i * expected_length));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, DPU_MRAM_HEAP_POINTER_NAME, output_offset, expected_length,
        DPU_XFER_DEFAULT));

    size_t mismatches = 0;
    for (uint32_t d = 0; d < nr_dpus; d++) {
        uint8_t *result = results + (size_t)d * expected_length;

        for (size_t j = 0; j < expected_length; j++) {
            if (result[j] != expected[j]) {
                if (mismatches < 4) {
                    fprintf(stderr, "dpu %u byte %zu: got %u, expected %u\n", d, j, result[j], expected[j]);
                }

                mismatches++;
            }
        }
    }

    DPU_ASSERT(dpu_free(set));

    if (mismatches != 0) {
        printf("FAIL: %zu bytes differ across %u DPU(s)\n", mismatches, nr_dpus);
        return 1;
    }

    printf("PASS: all %u DPU(s) produced the expected %zu output bytes\n", nr_dpus, expected_length);
    return 0;
}
