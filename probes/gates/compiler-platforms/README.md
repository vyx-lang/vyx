# Platform-selected values

```sh
python probes/gates/compiler-platforms/run.py --compiler /path/to/current/vyxc --output .cache/compiler-platforms
```

The fixture declares target-selected `let` and `const` values under the same
names. The gate emits Windows, Linux, macOS and Android IR at O0/O2 and checks
the native global initializer (or its folded return) and constant return against
independent expected values. It records compiler/source hashes and commands.
It does not execute cross-compiled code.
Native SDK release jobs run this gate before the standard-library consumers.

`platform.select` authorizes variables as well as functions/types/modules.
Without that schema target, the early selector discarded an annotated global
even for the matching target, leaving an unknown name. `std_system_contracts`
separately compares the selected clock identifier with native platform headers
and exercises the actual clock; `std_time_faults` checks its error paths.
