// SPDX-License-Identifier: BSD-3-Clause
//
// Stand-in for <cuda_fp16.h>, used only by the host test harness.
//
// The harness compiles src/Kernel.cu with a host compiler, so the include the
// real build resolves from the CUDA Toolkit has to resolve here too. The types
// and conversions live in cuda_runtime.h, next to the other CUDA surfaces the
// kernels touch; this file exists so the include line in the kernel source does
// not need an #ifdef for the harness.
#ifndef MCOFX_TEST_CUDA_FP16_SHIM_H
#define MCOFX_TEST_CUDA_FP16_SHIM_H

#include "cuda_runtime.h"

#endif // MCOFX_TEST_CUDA_FP16_SHIM_H
