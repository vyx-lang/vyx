# Module discovery

Run with the SDK compiler built from the current source:

```powershell
python probes/gates/module-discovery/run.py --compiler bootstrap_compiler/out/vyxc.exe
```

The gate creates 1,024 unused source modules below a nested `.cache` directory.
A single-file hello must finish semantic analysis within five seconds without
adding those modules to its registry. A generated `std/` directory and a `src/`
ancestor exercise the other implicit discovery roots. A missing import must
produce a diagnostic naming the missing module and must not scan the cached tree.

Two separate AOT consumers import a source module from a cached directory, one
through `--module-source`, the other through `-L`. Both execute an assertion that
the imported function returns `42`. An ordinary directory named `cache_data`
continues to support implicit discovery; the name is not a generated-directory
marker. The same applies to directories named `source.attr` and `source.vyi`:
their suffixes do not turn the directory into an attribute or interface file.
A missing import in ordinary source is also checked from a generated working
directory; the fallback must not recursively discover that working directory.

An ordinary source directory with 1,024 modules checks the inventory path that
must remain available outside generated directories. Semantic analysis retains
the five-second budget; a separate AOT consumer imports the final module and
asserts its value. On Windows both checks measure peak committed memory and
require it to remain below 1 GiB. The temporary source directory is removed after
the checks. Other platforms record time and execution without claiming that
Windows memory measurement.

Function registration is checked with two modules defining ordinary functions
with the same signature, generic functions with the same parameter count, and
full specializations with the same type arguments. Their bodies return different
results, which separate AOT consumers assert. Three negative fixtures place a
different module's function between two declarations in the same module; the
true duplicate must still be diagnosed for each function form. This keeps module
identity precise without hiding duplicate declarations behind another module.

Two more AOT consumers call distinct public functions in those modules. Each
public function calls a private helper by its bare name, with matching helper
signatures in both modules and different results. Ordinary and generic helpers
must resolve in the enclosing function's declaring module, including imported
function bodies; the primary translation unit cannot supply their scope.
Another consumer has no `module` declaration and defines its own same-named
helper and a generic helper. Its root namespace is also an exact scope; it must
not include the imported module's private helpers during overload scoring or
generic selection. Two local ordinary overloads also exercise argument scoring.

An ordinary source directory is compiled with a relative `main.vyx` path and
a relative project unity sibling. Implicit module scanning must identify the
sibling as the source already loaded into that unit, avoiding duplicate imports
and native definitions. Keeping this fixture outside a generated directory is
necessary because those directories intentionally skip implicit discovery.

Logs, compiler identity, timings and verdicts are kept in the ignored
`.cache/<timestamp>/` directory. `--output-dir` selects a fresh result directory;
the runner refuses to overwrite it. `--budget` changes the discovery-only timeout
for slow hosts. Deterministic checks of the discovery log remain active at any
budget. This gate checks single-file discovery and native execution, not project
build performance or cross-platform self-hosting.

Use `--case <name>` to run a specific check, or repeat it to select several checks.
