// CUDA provider implementation for tests/projects/dci_cuda.
//
// Compiled by nvcc, not by the Vyx build: nvcc owns the device pass, the PTX
// embedding and the host-side MSVC ABI, so the symbols defined here are exactly
// the ones the DCI contract derived from cuda_ops.hpp.

#include "cuda_ops.hpp"

#include <cuda_runtime.h>

namespace {

thread_local int32_t g_last_error = 0;

void record(cudaError_t e) {
    if (e != cudaSuccess) { g_last_error = static_cast<int32_t>(e); }
}

__global__ void fill_kernel(double* data, int32_t count, double base) {
    const int32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) { data[i] = base + static_cast<double>(i); }
}

__global__ void scale_kernel(double* data, int32_t count, double factor) {
    const int32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) { data[i] = data[i] * factor; }
}

__global__ void sum_kernel(const double* data, int32_t count, double* result) {
    // Single-block reduction: the fixture needs a correct GPU-side sum, not a
    // throughput-optimal one.
    __shared__ double scratch[256];
    const int32_t tid = threadIdx.x;
    double acc = 0.0;
    for (int32_t i = tid; i < count; i += blockDim.x) { acc += data[i]; }
    scratch[tid] = acc;
    __syncthreads();
    for (int32_t stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride) { scratch[tid] += scratch[tid + stride]; }
        __syncthreads();
    }
    if (tid == 0) { result[0] = scratch[0]; }
}

int32_t launch_blocks(int32_t count) {
    return count <= 0 ? 1 : (count + 255) / 256;
}

double reduce_on_gpu(const double* data, int32_t count) {
    double* device_result = nullptr;
    if (cudaMalloc(reinterpret_cast<void**>(&device_result), sizeof(double)) != cudaSuccess) {
        record(cudaGetLastError());
        return __builtin_nan("");
    }
    sum_kernel<<<1, 256>>>(data, count, device_result);
    record(cudaDeviceSynchronize());

    double host_result = 0.0;
    record(cudaMemcpy(&host_result, device_result, sizeof(double), cudaMemcpyDeviceToHost));
    cudaFree(device_result);
    if (g_last_error != 0) { return __builtin_nan(""); }
    return host_result;
}

}  // namespace

CudaVector::CudaVector(int32_t count) noexcept : data_(nullptr), count_(count) {
    if (count <= 0) {
        g_last_error = static_cast<int32_t>(cudaErrorInvalidValue);
        return;
    }
    const cudaError_t e = cudaMalloc(reinterpret_cast<void**>(&data_),
                                     static_cast<size_t>(count) * sizeof(double));
    if (e != cudaSuccess) {
        record(e);
        data_ = nullptr;
    }
}

CudaVector::~CudaVector() noexcept {
    if (data_ != nullptr) {
        record(cudaFree(data_));
        data_ = nullptr;
    }
}

int32_t CudaVector::count() const noexcept { return count_; }

double CudaVector::fill_and_sum(double base) noexcept {
    if (data_ == nullptr) { return __builtin_nan(""); }
    fill_kernel<<<launch_blocks(count_), 256>>>(data_, count_, base);
    record(cudaDeviceSynchronize());
    if (g_last_error != 0) { return __builtin_nan(""); }
    return reduce_on_gpu(data_, count_);
}

double CudaVector::scale_and_sum(double factor) noexcept {
    if (data_ == nullptr) { return __builtin_nan(""); }
    scale_kernel<<<launch_blocks(count_), 256>>>(data_, count_, factor);
    record(cudaDeviceSynchronize());
    if (g_last_error != 0) { return __builtin_nan(""); }
    return reduce_on_gpu(data_, count_);
}

int32_t cuda_ops_device_count() {
    int32_t devices = 0;
    const cudaError_t e = cudaGetDeviceCount(&devices);
    record(e);
    if (e != cudaSuccess) { return 0; }
    return devices;
}

int32_t cuda_ops_last_error() { return g_last_error; }
