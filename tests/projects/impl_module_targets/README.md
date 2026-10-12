# Imported nominal impl targets

Two modules declare `Record` with different layouts and methods. Three other
source files implement traits for them: two use module imports and the same
short target name, another uses a type alias. The impl target must bind in its own
file's module and import scope before the compiler registers its methods.
An enum in another module also receives a trait impl and dispatches both of
its variants through a match in that impl.

```powershell
./tests/projects/impl_module_targets/run.ps1 -Compiler ./bootstrap_compiler/out/vyxc.exe
```

The runner builds and executes fresh copies at O0 and O2 using both `build` and
`--run=aot --src=project`, and repeats the build with impl files preceding the
type definitions. It also rejects an impl whose target is ambiguous
between the two imported modules, including when its body does not access self.
Logs and compiler identity remain in the ignored `tests/.cache` result directory.
