# p7_opt_mismatch

Intent: `-O0` vs `-O2` must agree.

Compile `opt_probe.vyx` at `--mir-opt 0 --llvm-opt 0` and at `-O2`, run both,
and compare stdout + exit code. The program has constant-foldable arithmetic
(`fold_me` == 34), a statically-dead branch (`dead_branch`), and a loop that
threads a class value through calls (`loop_sum(100)` == 4950); total 4991.

A pass: both print `result=4991` and exit 0, and neither MIR module is empty.
A fail: divergent output/exit, an empty HIR/MIR module, or opt-only crash.
