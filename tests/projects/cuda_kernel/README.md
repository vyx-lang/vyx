# cuda_kernel：CUDA 端到端回归

这个目录是当前“Vyx 直接跑在 CUDA 上”的端到端闸门。普通 `vyx build`
会读取 `Vyx.toml` 中的设备源和架构，调用内置 NVPTX 后端生成 PTX，
并把 PTX、kernel 符号和参数布局嵌入 host。CUDA driver import library
也由编译器定位，不再复制到项目的 `native/prebuilt/`。

`run.ps1` 调用项目构建，用 `ptxas` 校验生成的 PTX，再运行生成的 host。
host 不读取 `.cache/device.ptx` 或 `device_entry.txt`。

当前链路：

1. `[target.cuda_kernel]` 声明 `cuda_arch` 和 `cuda_device_sources`。
2. 构建器在链接前用内置 NVPTX 后端编译设备源，产出缓存 PTX。
3. 构建器将 kernel 指定为设备编译的导出根，按真实 mangler 的逐参数规则
   生成符号，并核对 PTX 中的入口；符号、参数大小和偏移随 PTX 一起写入
   host 嵌入源。设备文件不需要伪造 `main` 来保留 kernel。
4. host 编译该嵌入源，构建器从本机 CUDA toolkit 找到驱动导入库并链接。
5. `src/host.vyx` 从嵌入字符串加载 PTX，使用生成的符号和偏移调用
   `cuLaunchKernel`。

设备索引仍通过用户声明的 NVVM intrinsic 获取。

## 后续方向

设备代码保持普通 Vyx 函数；未来的类型化运行时 API 可以把 Driver API
调用收进语言库，例如：

```vyx
@[cuda_kernel]
public fn scale_kernel(dst: *f64, src: *f64, k: f64, n: i32) {
    let tid = cuda::thread_idx().x;
    let stride = cuda::block_dim().x;
    var i = tid;
    while (i < n) {
        unsafe { dst[i] = src[i] * k; }
        i = i + stride;
    }
}

fn main() -> i32 {
    let kernel = cuda::load_kernel::<scale_kernel>(cuda::arch("sm_120"));
    kernel.launch(grid(4, 1, 1), block(256, 1, 1), dst, src, 2.5, n);
    cuda::synchronize();
    return 0;
}
```

上面的 `cuda::load_kernel` 尚未实现。当前 host 使用 CUDA Driver API，
由构建器提供 PTX、kernel 符号和参数布局。

## 仍需解决

- 同一份源码里的 host/device 自动切分。
- `cuda::thread_idx()`、`cuda::block_dim()`、`cuda::sync_threads()` 等
  内建，替换手写 NVVM `extern`。
- 把 Driver API、context、模块加载、launch 和 CUDA 错误收进 `cuda` 运行时。
- cubin、多 kernel、多架构、增量缓存复用和 device 语言子集诊断。
- 缺 toolkit、缺 GPU、架构不匹配时的稳定诊断。

## 验收

在项目目录执行：

```powershell
.\run.ps1 -BootstrapCompiler ..\..\..\bootstrap_compiler\out\vyxc.exe
```

这里显式指定以兼容 Release SDK 为 Stage 0、从当前源码构建的 SDK 编译器，
配套使用同次构建的 `vyx_compiler_backend` / `vyx_runtime`。
脚本用该 SDK 编译器构建项目，检查 PTX 入口与嵌入的 kernel 符号一致、
目标为 `sm_120`，再用 `ptxas` 交叉验证并在项目根目录运行
`target\cuda_kernel.exe`。成功输出必须包含 `cuda_kernel: OK`。
