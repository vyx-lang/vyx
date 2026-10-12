# Planner pipe-set gate

`check-source-scans.ps1 -Compiler <current SDK compiler>` extracts the production
source-text cache, token scan, argument and path helpers. O0/O2 checks cover empty payloads,
prefix collisions, CRLF and a final row without a newline, distinct source
paths, invalidation after a size change, and 500 reads of a 2 MiB source.
Repeated duplicate-row insertion also checks that the planner reads its
existing table without cloning the full input, preserving the original bytes.
The repeated-read test observes Windows private commit and limits retained
growth to 32 MiB. The mtime wrapper calls the actual backend filesystem API;
all cache and index algorithms come from compiler source. Environment cases
include dollar and Windows percent variables, nested defaults and unmatched
delimiters. Path cases preserve UTF-8 bytes and ASCII-only case conversion.
DCI token cases exclude strings and comments and include exact end-of-input
matches. Long token scans and 2 MiB arguments must stay within the same memory
bound; throughput is checked separately with complete project builds.

```powershell
pwsh -File probes/gates/build_sets/run.ps1
```

The runner extracts the new production `build_sets.vyx` module and the original
four helper functions from commit `e52de175`, without rewriting their bodies.
It compiles both units with the actual bootstrap compiler and executes both.
`-Compiler` and `-BaselineRevision` can select another explicit compiler or
baseline. This is a unit-extraction gate; it does not replace the whole-compiler
module integration and self-host gates.

The 32-case cross product compares all output bytes for `has`, `add` and `merge`
(3,072 results). Cases include empty sets, duplicate and empty tokens, original
ordering, whitespace trimming, exact token/subsequence boundaries, UTF-8,
`Aa`/`B@` (equal DJB2 hashes), long whitespace, and a heap-sized input producing
a short result. It preserves the existing `has` behavior for values containing
`|`: a boundary-aligned multi-token subsequence can match. The existing set's
spelling and duplicate tokens are preserved; added tokens are trimmed.

Separate 0/1/23/24/63/64-byte cases verify returned strings after helper builders
have been destroyed and other calls have reused stack/heap storage. Fixtures
are assertion adapters only; no Python/PowerShell copy of the set algorithm
acts as a semantic oracle.

A separate process for each implementation merges 1,436 unique names
(35,899 bytes), verifies identical output and observes real Windows private
commit before/after and peak. The coarse allocation gate permits an 8 MiB
noise allowance and requires the new growth to remain below a quarter of the
baseline plus that allowance. Wall time is diagnostic, not a speed threshold.
Compiler/runtime SHA-256, generated source, compile logs, output files and OS
memory observations remain under ignored `.runs/`.

On the September 27 development build the old single merge grew private commit
by 138,752,000 bytes; the new merge grew it by 139,264 bytes. This demonstrates
the helper allocation change, not an end-to-end compiler benchmark. Existing
callers that clone entire inputs or repeatedly reconstruct sets still need
separate planner work.

The new merge owns a bounded `Vec<i64>` table of spans into its two borrowed
inputs. A hash chooses a probe chain; exact span bytes decide equality. It
appends accepted names in input order to one `StringBuilder`, then detaches the
result before destroying owned storage. The table and builder are explicitly
destroyed, including the heap-capacity/empty-result branch. The standalone
`has` scan makes no full-set clone; `add` remains linear in the existing set.

`check-source-scans.ps1 -Compiler <current SDK compiler>` also compares cached
generic-call spans with the original full-text scanner at O0/O2. The 90
combinations cover exact identifier boundaries, repeated calls, nested type
arguments, malformed angles, qualified names, and selecting the outer versus
inner call. A selected outer call consumes its complete span; unselected outer
calls still expose nested candidates. The planner keeps the scanner's existing
interpretation of comments and strings; semantic validation belongs to the
frontend. Cached spans use the same path/mtime/size identity as source text and
are invalidated when that identity changes. Repeated queries of a large source
must reuse the span rows and remain within the private-memory allowance.
