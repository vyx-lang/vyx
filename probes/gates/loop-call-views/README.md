# Loop calls, local strings and project definitions

Run against the SDK compiler built from the current source:

```powershell
python probes/gates/loop-call-views/run.py --compiler bootstrap_compiler/out/vyxc.exe --out .cache/loop-call-views
```

The runner requires a new output directory and uses the bounded process runner
from `language-composition`. Each compiler/runtime process has a 90-second
timeout and a 2 GiB memory bound. It records commands, compiler/backend/runtime
hashes, source hashes, output, resource failures and individual verdicts.
It uses this tree's `bootstrap_compiler/std_packages` and the compiler's
companion runtime, with native AOT execution only.

The positive matrix covers O0/O2 and 1/4/8 code generation units:

- Byte scanning with `while (p < n && at(buf, p) != 32)`.
- Function calls in `&&` and `||`, including skipped RHS calls, exact evaluation
  counts and HTTP method/path comparisons.
- Loop-local `String.fromBytes` owners and their borrowed `raw()` views.
  `fromBytes` copies bytes into an owning String; it is not itself a view.
- One, two and three levels of loops passing a mutable owning object with a
  String field; independently computed state and sums must agree.
- Borrowed-parameter returns, cloned string returns and owning String returns.
- SSO and heap-sized copies returned after explicitly destroying a local String
  and StringBuilder, through aliases, branches and repeated loop calls.
- Primitive str clones returned from local String owners, including forwarding
  helpers and repeated short/heap-sized results.
- Generated functions containing 96, 224 and 416 call initializers, each followed
  by an accumulation statement. Mid-function output and exact call counts catch
  skipped statements, repeated evaluation and zero-initialized call results.

Project negatives place duplicate ordinary and generic functions in a sibling
source, and ordinary definitions across two sources. Another fixture uses the
same `&mut Model` signature twice in a project sibling. Diagnostics must name
the second definition's file and line. Single-file duplicates remain rejected;
ordinary overloads and repeated extern C prototypes remain valid.

The project regression matters because sibling declarations are tagged both
`|imported|` and `|project-local`. Treating every imported declaration as foreign
used to silently select one of two source bodies. Same-crate definitions must
participate in duplicate checking.

## Returned views

The negative fixtures return views of local owners directly, through aliases,
through calls, after assignments, from conditional branches and after loop
back-edges. Another fixture returns a reference to a local scalar. These must
fail with `E3102`. A same-named `clone` method that returns a view is not evidence
of independent storage. The cross-file negative also checks the second source's
filename and return line instead of accepting a diagnostic on the entry file.

Positive fixtures return caller-owned borrows, independent copies, static branch
results and views overwritten with static strings on both paths. They exercise
generic helpers, pipeline calls and a separate source module. A loop fixed point
checks propagation that becomes visible only on its second iteration.

Return checking uses per-function flow state, with joins for branches and loops.
Independent str-return facts use exact selected functions and resolved builtins.
When a lazy concrete helper needs resolution, its linear return chain is queried
on demand and completed bodies are released; only signature facts survive.
Unknown and recursive result origins do not prove independent ownership.

This gate checks function returns. It does not certify all escape paths through
global storage, callback captures, raw memory or arbitrary aggregates.

The gate does not execute a GPU model, prove model logits, test concurrent HTTP
serving or validate KV capacity. Real-project source snapshots and investigation
logs belong in ignored local result directories, not these fixtures.
