# Return signature preparation

`run.py --compiler <current compiler>` checks project builds and execution at
O0/O2. Fixtures cover inferred scalar/generic returns, statement branches, void
helpers, and diagnostics in imported function bodies. The public inferred
signature must survive shallow `.vyi` generation and separate object compilation.

The interface carries omitted return types in the existing verified AST artifact,
preserving the defining module and helper visibility. It must not silently publish
an inferred value-returning function as `void`. Every project module still checks
its own bodies.
