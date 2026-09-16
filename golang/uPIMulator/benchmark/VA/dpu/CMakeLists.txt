SET(BL 10)
SET(TYPE INT32)

# The DPU compiler comes from the installed UPMEM SDK. Override with -DDPU_CC=<path>.
if(NOT DPU_CC)
  set(DPU_CC "dpu-upmem-dpurte-clang")
endif()
set(CMAKE_C_COMPILER "${DPU_CC}")
set(CMAKE_C_FLAGS "-w -I${CMAKE_CURRENT_SOURCE_DIR}/../support -O2 -S -g0 -DNR_TASKLETS=${NR_TASKLETS} -DBL=${BL} -D${TYPE}")

file(GLOB_RECURSE SRCS *.c)

add_library(VA_device OBJECT ${SRCS})

