/* Copyright 2020 UPMEM. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <perfcounter.h>
#include <attributes.h>

#define BIT_IMPRECISION 4

perfcounter_t
perfcounter_get(void)
{
    uint32_t reg_value;
    __asm__ volatile("time %[r]" : [r] "=r"(reg_value));
    return ((perfcounter_t)reg_value) << BIT_IMPRECISION;
}

#ifndef DPU_PROFILING
perfcounter_t
perfcounter_config(perfcounter_config_t config, bool reset_value)
{
    uint32_t reg_value;
    /* SDK 2025.1 rejects a source of 6 or 7 before it configures anything;
     * the library the machine links does this, so the simulator's does too,
     * or a probe's first reading would differ by three instructions. */
    if ((config & ~1u) == 6)
        __asm__ volatile("fault 9");
    uint32_t reg_config = (reset_value ? 1 : 0) | (config << 1);
    __asm__ volatile("time_cfg %[r], %[c]" : [r] "=r"(reg_value) : [c] "r"(reg_config));
    return ((perfcounter_t)reg_value) << BIT_IMPRECISION;
}
#endif /* !DPU_PROFILING */
