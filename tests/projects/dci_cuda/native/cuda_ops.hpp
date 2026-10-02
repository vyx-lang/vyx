#pragma once

#include <cstdint>

// CUDA-backed provider surface.
//
// Everything the DCI contract ever sees is ordinary C++: the device code
// (__global__/__device__ qualifiers, cuda_runtime.h, <<<>>> launches) stays
// inside cuda_ops.cu, which nvcc compiles.  The consumer therefore needs no
// CUDA headers, no GPU target triple and no nvcc at all -- it constructs this
// class and calls its methods through the C++ ABI, exactly like any other DCI
// provider.

class CudaVector {
public:
    explicit CudaVector(int32_t count) noexcept;
    ~CudaVector() noexcept;

    int32_t count() const noexcept;

    // Both run nvcc-compiled kernels and return the GPU-side reduction, or NaN
    // when a CUDA call failed (see cuda_ops_last_error()).
    double fill_and_sum(double base) noexcept;      // data[i] = base + i
    double scale_and_sum(double factor) noexcept;   // data[i] *= factor

private:
    double* data_;
    int32_t count_;
};

int32_t cuda_ops_device_count();
int32_t cuda_ops_last_error();
