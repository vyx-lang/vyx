# Owning enum payload cleanup

This is an open compiler ownership regression, not a passing release gate.
With the current source compiler, discarded Result/Option payloads with a
destructor are not automatically destroyed. Binding an `unwrap_err` result
to its own local does run the payload destructor. The expected contract is
five constructions and five destructions; any other result is a failure.

```sh
vyxc --src=project probes/gates/enum-payload-cleanup --run=aot -O0
vyxc --src=project probes/gates/enum-payload-cleanup --run=aot -O2
```

The C file counts only construction/destruction callbacks. It does not
allocate resources or replace any production function. The owning value has
no Copy implementation. This separates missing enum drop obligations from
native parser/regex cleanup or a malloc implementation.

The gap affects ignored owning error payloads in the typed TOML and Regex
APIs, including `is_err()` checks on temporaries. Vyx end-to-end LeakSanitizer
still reports retained runtime String allocations even though actual PCRE2
program and TOML node-handle counters return to zero. Native bridge-only
ASan/UBSan/LSan passing does not prove the compiler cleans every enum payload.

Adding a generic library `drop` method alone is insufficient: consuming APIs
also need an owning receiver and correct transfer/drop modelling. The
[full-expression gate](../full-expression/README.md) now checks explicit enum
destructors with `own self` extraction, guarded pattern transfer and borrowed
generic-enum matches. Automatic structural enum cleanup and the standard
Result/Option consuming APIs still need this gate and std ownership/generic
regressions. Do not add a library raw tag/layout workaround or suppress
LeakSanitizer reports.
