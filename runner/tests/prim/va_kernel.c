/* PrIM's vector-addition kernel, unchanged except for a counter probe.
 *
 * BIMSA is a wavefront: a small working set, a lot of arithmetic on it, and
 * short scattered transfers. This is the opposite -- long sequential transfers
 * and almost no arithmetic between them -- so it tests a different part of the
 * timing model with the same method. The source is
 * golang/uPIMulator/benchmark/VA/dpu/task.c; only the lines marked PROBE are
 * added, and they are the pattern runner/tests/perf uses everywhere else.
 *
 * Do not change anything else here. A workload that is edited to suit the
 * simulator measures the simulator, not the machine.
 */
#include <stdint.h>
#include <stdio.h>
#include <defs.h>
#include <mram.h>
#include <alloc.h>
#include <perfcounter.h>
#include <barrier.h>

#include "../support/common.h"

__host dpu_arguments_t DPU_INPUT_ARGUMENTS;

/* PROBE: one counter serves the whole DPU, so tasklet 0's reading already
 * covers every tasklet's work. Never sum these over tasklets.
 * PROBE_MODE picks what it counts, so one binary gives both cycles and
 * instructions and the comparison cannot be spoiled by a rebuild. */
__host uint32_t PROBE_MODE; /* 0 = cycles, 1 = instructions */
__host uint32_t OUT_COUNT;
BARRIER_INIT(probe_barrier, NR_TASKLETS);

// vector_addition: Computes the vector addition of a cached block 
void __attribute__ ((noinline)) vector_addition(T *bufferB, T *bufferA, unsigned int l_size) {
    for (unsigned int i = 0; i < l_size; i++){
        bufferB[i] += bufferA[i];
    }
}

// Barrier
BARRIER_INIT(my_barrier, NR_TASKLETS);

extern int main_kernel1(void);

int (*kernels[nr_kernels])(void) = {main_kernel1};

int main(void) { 
    // Kernel
    return kernels[DPU_INPUT_ARGUMENTS.kernel](); 
}

// main_kernel1
int main_kernel1() {
    unsigned int tasklet_id = me();
#if PRINT
    printf("tasklet_id = %u\n", tasklet_id);
#endif
    if (tasklet_id == 0){ // Initialize once the cycle counter
        mem_reset(); // Reset the heap
    }
    // Barrier
    barrier_wait(&my_barrier);

    /* PROBE */
    if (tasklet_id == 0)
        perfcounter_config(PROBE_MODE ? COUNT_INSTRUCTIONS : COUNT_CYCLES, true);
    barrier_wait(&probe_barrier);
    perfcounter_t probe_start = perfcounter_get();

    uint32_t input_size_dpu_bytes = DPU_INPUT_ARGUMENTS.size; // Input size per DPU in bytes
    uint32_t input_size_dpu_bytes_transfer = DPU_INPUT_ARGUMENTS.transfer_size; // Transfer input size per DPU in bytes

    // Address of the current processing block in MRAM
    uint32_t base_tasklet = tasklet_id << BLOCK_SIZE_LOG2;
    uint32_t mram_base_addr_A = (uint32_t)DPU_MRAM_HEAP_POINTER;
    uint32_t mram_base_addr_B = (uint32_t)(DPU_MRAM_HEAP_POINTER + input_size_dpu_bytes_transfer);

    // Initialize a local cache to store the MRAM block
    T *cache_A = (T *) mem_alloc(BLOCK_SIZE);
    T *cache_B = (T *) mem_alloc(BLOCK_SIZE);

    for(unsigned int byte_index = base_tasklet; byte_index < input_size_dpu_bytes; byte_index += BLOCK_SIZE * NR_TASKLETS){

        // Bound checking
        uint32_t l_size_bytes = (byte_index + BLOCK_SIZE >= input_size_dpu_bytes) ? (input_size_dpu_bytes - byte_index) : BLOCK_SIZE;

        // Load cache with current MRAM block
        mram_read((__mram_ptr void const*)(mram_base_addr_A + byte_index), cache_A, l_size_bytes);
        mram_read((__mram_ptr void const*)(mram_base_addr_B + byte_index), cache_B, l_size_bytes);

        // Computer vector addition
        vector_addition(cache_B, cache_A, l_size_bytes >> DIV);

        // Write cache to current MRAM block
        mram_write(cache_B, (__mram_ptr void*)(mram_base_addr_B + byte_index), l_size_bytes);

    }

    /* PROBE: the barrier comes first so that the window covers every tasklet's
     * work. Without it tasklet 0 stops counting when it finishes its own share,
     * and what the counter reports then depends on the order the tasklets
     * happen to finish in -- which made the instruction counts differ by 2.4%
     * between the machine and the simulator for no reason at all. */
    barrier_wait(&probe_barrier);
    perfcounter_t probe_end = perfcounter_get();
    if (tasklet_id == 0) OUT_COUNT = (uint32_t)(probe_end - probe_start);

    return 0;
}
