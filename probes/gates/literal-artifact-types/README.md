# Literal types in imported bodies

```sh
python probes/gates/literal-artifact-types/run.py --compiler /path/to/vyxc
```

The gate compares source and fresh shallow-interface bodies through streaming
MIR verification and native AOT at O0/O2. It checks the i32 default, wide integers,
explicit i64/u8 suffixes, overload selection, and bitwise masks passed to an i32
helper. The standard-library UTF-8 cursor must decode one- through four-byte
scalars and make progress through malformed input. Illegal narrowing and
ambiguous overloads must still fail.

Results, compiler identity and failed commands are retained under `.cache/` or
`--output`. Run `compiler-self-source` separately to recheck the compiler's actual
module plans with freshly emitted interfaces; this focused gate does not replace it.
