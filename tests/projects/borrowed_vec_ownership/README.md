# 借用容器与文本所有权项目

```powershell
vyxc --src=project . --run=aot
```

`fixture.stats` 从借用参数遍历容器，调用端在遍历后继续读取、修改容器。
项目同时验证 String 的借用、移动、clone、返回及重新初始化。
完整编译矩阵与错误诊断回归见 `probes/gates/borrowed-vec-string/run.ps1`。
