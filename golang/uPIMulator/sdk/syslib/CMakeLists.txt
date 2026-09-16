# The DPU compiler comes from the installed UPMEM SDK. Override with -DDPU_CC=<path>.
if(NOT DPU_CC)
  set(DPU_CC "dpu-upmem-dpurte-clang")
endif()
set(CMAKE_C_COMPILER "${DPU_CC}")

# -S makes the compiler emit assembly text into the files CMake names *.c.o.
# uPIMulator's own linker lexes those files; it never consumes real objects.
# -g0 suppresses DWARF: the debug sections carry hex literals with letter digits
# (0x1a), which the linker's lexer cannot tokenize.
# NR_THREADS is no longer predefined by the SDK driver (it was in 2023.2.0).
# 24 matches ConfigLoader.MaxNumTasklets(), which is what the linker allocates
# STACK_SIZE_TASKLET_0..23 and __sys_stack_thread_0..23 for.
set(CMAKE_C_FLAGS "-O3 -S -g0 -DNR_TASKLETS=${NR_TASKLETS} -DNR_THREADS=24")

include_directories("${CMAKE_SOURCE_DIR}/misc")
include_directories("${CMAKE_SOURCE_DIR}/stdlib")
include_directories("${CMAKE_SOURCE_DIR}/syslib")

file(GLOB_RECURSE SRCS *.c)

# seqread.inc calls __builtin_dpu_seqread_get, a compiler builtin that SDK
# 2025.1.0 no longer provides, and the SDK ships no replacement source. Only
# GEMV and MLP pull in seqread.h, so the rest of the runtime builds without it.
# Removing these here keeps the build clean, so a real error is never masked.
file(GLOB SEQREAD_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/seqread*.c)
list(REMOVE_ITEM SRCS ${SEQREAD_SRCS})

# OBJECT, not a static library: archiving assembly text would fail.
add_library(syslib OBJECT ${SRCS})
