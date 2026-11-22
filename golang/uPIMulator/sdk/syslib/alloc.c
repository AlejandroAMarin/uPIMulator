/* Copyright 2020 UPMEM. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stddef.h>
#include <dpuruntime.h>
#include <atomic_bit.h>
#include <attributes.h>

volatile unsigned int __sys_heap_pointer = (unsigned int)(&__sys_heap_pointer_reset);

ATOMIC_BIT_INIT(__heap_pointer);

/* noinline, because part of grind tracked functions
 * Also used by seqread.inc
 */
void *__noinline
mem_alloc_nolock(size_t size)
{
    unsigned int pointer = __HEAP_POINTER;

    if (size != 0) {
        pointer = (pointer + 7) & ~7;

        unsigned int new_heap_pointer, limit;

        /* The instructions of SDK 2025.1's mem_alloc_nolock, which is what
         * the machine links: the new pointer is checked against the end of
         * WRAM instead of probed with a load. The machine's limit is 65536;
         * the simulator places WRAM at 512 and gives it 128 KB, so its limit
         * is the end of that, which changes no instruction. */
        __asm__ volatile("\tmove %[lim], 131584\n"
                         "\tadd %[nhp], %[ptr], %[sz], c, . + 2\n"
                         "\tjltu %[nhp], %[lim], . + 2\n"
                         "\tfault " __STR(__FAULT_ALLOC_HEAP_FULL__) "\n"
                         : [nhp] "=&r"(new_heap_pointer), [lim] "=&r"(limit)
                         : [ptr] "r"(pointer), [sz] "r"(size));

        __HEAP_POINTER = new_heap_pointer;
    }
    return (void *)pointer;
}

void *
mem_alloc(size_t size)
{
    ATOMIC_BIT_ACQUIRE(__heap_pointer);
    void *pointer = mem_alloc_nolock(size);
    ATOMIC_BIT_RELEASE(__heap_pointer);
    return pointer;
}

void *
mem_reset()
{
    ATOMIC_BIT_ACQUIRE(__heap_pointer);

    void *initial = &__sys_heap_pointer_reset;

    __sys_heap_pointer = (unsigned int)initial;

    ATOMIC_BIT_RELEASE(__heap_pointer);

    return (void *)initial;
}
