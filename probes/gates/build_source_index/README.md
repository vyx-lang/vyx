# BuildSourceIndex 回归

目标内源码快照在生成源码的 hooks/CUDA 准备之后建立，在对象任务执行前释放容器；registry discovery 使用独立短生命周期索引。它不是跨进程共享 Sema，也不是完整的增量事实数据库。

```powershell
pwsh -NoProfile -File probes/gates/build_source_index/run.ps1
pwsh -NoProfile -File probes/gates/build_source_index/run-project.ps1
```

脚本直接抽取生产 `build_system.vyx` 函数，使用真实文件、mtime/size、接口 stamp 与新 `BuildSourceIndex`。入口导入闭包同时运行 `e52de175` 中旧函数作为参考，输出必须相等；未实现测试替代的模块 loader。

`run-project.ps1` 另建全新 j1/j20 工程，验证同模块两个源文件之间的实际调用、两个 peer 清单、可执行结果与无修改构建。最近通过 `.runs/peers-20260927-181652-543`，两个并行度的可执行文件 hash 相同。

覆盖完整路径、最长点边界前缀、近似名字拒绝、同模块多文件顺序、循环终止、依赖 stamp 顺序与复用，以及下一目标重新观察修改后的文件。生产专用 ID 索引使用开放寻址和完整字符串等值判断；hash 不是身份。额外覆盖同 hash 的 `Aa` / `B@`、零容量起步、1024 项增长和覆盖更新。

最近通过：`.runs/20260927-181219-148`，抽取 69 个生产函数，退出 0。registry 只建立可用模块身份索引，在源文件进入 BFS 队列后才读取其 import；未用的 SDK 模块不再提前扫描完整依赖，版本同名文件仍全部入队。peer 接口列表也使用目标索引；同模块多文件列表与旧实现的实际文件逐字节一致，单文件模块不生成列表，真实写入失败返回错误。该负向测试会打印一条 `occupied` 路径错误，整项门最终应退出 0。索引保留原扫描器的语言覆盖边界，尚未把文本 import/module scanner 替换为 parser 事实。


专用 ID 索引使稳定 source ID、记录存储和 probe table 生命周期显式；本门不验证通用泛型接口。用户泛型制品、私有依赖与多消费端实例链接由 [generic_interfaces](../generic_interfaces/README.md) 独立验证。标准库接口仍省略模板 payload，该门的 `Dict` 用例显式提供标准库 source unit，不代表标准库泛型已经全部支持无源码分发。
