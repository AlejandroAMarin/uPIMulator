/* Replays one DPU's work, recorded by the runner's shim, on one DPU.
 *
 *   UPIM_RECORD=<record> UPIM_RECORD_DPU=<index> runner/upimulator run ... -- <host> ...
 *   replay_host <DPU ELF> <record> <out>
 *
 * A host that spreads its work over many DPUs cannot be timed DPU by DPU on
 * the machine, and GIMSA needs 199 of them. The record holds every copy the
 * host made to one DPU, every copy it made from it, and every launch, in
 * order. This host makes the same copies to one DPU and launches it the same
 * number of times; it makes the same copies from it after each launch and
 * writes their bytes to <out>, in order, so a run on the machine and a run in
 * the simulator can be compared byte for byte.
 *
 * XPROFILE picks the backend: backend=hw for the machine. For the simulator,
 * run it under runner/upimulator. Build it with:
 *
 *   gcc -O2 -o replay_host runner/scripts/replay_host.c $(dpu-pkg-config --cflags --libs dpu)
 */
#include <dpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_exact(FILE *file, void *buffer, size_t length)
{
    return fread(buffer, 1, length, file) == length;
}

int main(int argc, char **argv)
{
    struct dpu_set_t set, dpu;
    FILE *record, *out;
    int type;
    unsigned launches = 0, copies = 0, reads = 0;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <DPU ELF> <record> <out>\n", argv[0]);
        return 2;
    }
    record = fopen(argv[2], "rb");
    out = fopen(argv[3], "wb");
    if (record == NULL || out == NULL) {
        perror("replay_host");
        return 1;
    }

    DPU_ASSERT(dpu_alloc(1, getenv("XPROFILE"), &set));
    DPU_ASSERT(dpu_load(set, argv[1], NULL));
    DPU_FOREACH (set, dpu) {
        break;
    }

    while ((type = fgetc(record)) != EOF) {
        if (type == 'L') {
            DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
            launches++;
            continue;
        }

        uint32_t name_length, fields[2];
        char name[256];
        if (type != 'C' && type != 'R') {
            fprintf(stderr, "replay_host: bad record type %d\n", type);
            return 1;
        }
        if (!read_exact(record, &name_length, sizeof(name_length)) || name_length >= sizeof(name) ||
            !read_exact(record, name, name_length) || !read_exact(record, fields, sizeof(fields))) {
            fprintf(stderr, "replay_host: truncated record\n");
            return 1;
        }
        name[name_length] = 0;

        void *buffer = malloc(fields[1] ? fields[1] : 1);
        if (type == 'C') {
            if (!read_exact(record, buffer, fields[1])) {
                fprintf(stderr, "replay_host: truncated copy\n");
                return 1;
            }
            DPU_ASSERT(dpu_copy_to(dpu, name, fields[0], buffer, fields[1]));
            copies++;
        } else {
            DPU_ASSERT(dpu_copy_from(dpu, name, fields[0], buffer, fields[1]));
            fwrite(&fields[1], sizeof(fields[1]), 1, out);
            fwrite(buffer, 1, fields[1], out);
            reads++;
        }
        free(buffer);
    }

    fprintf(stderr, "replay_host: %u copies to the DPU, %u from it, %u launches\n", copies, reads, launches);
    fclose(out);
    DPU_ASSERT(dpu_free(set));
    return 0;
}
