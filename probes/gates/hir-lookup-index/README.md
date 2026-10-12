# HIR cache indexes

`run.ps1 -Compiler <current SDK compiler>` builds the production instance-map
implementation with its production hash and record helpers, then checks 20,000
declaration identities at `-O0` and `-O2`. It covers multiple keys per declaration,
colliding buckets, repeated table growth, overwrites through both APIs, missing
entries, reverse provenance, disposal and reuse. The isolated harness substitutes
only allocator/profiling and invalid-ID helpers; it does not copy the map algorithm.

The compiler's main module remains the performance workload. This gate checks
cache behavior and cannot establish whole compiler speed or a self-host fixpoint.

`check-names.ps1 -Compiler <current SDK compiler>` compares the production
name predicate with its body extracted from commit `ee4d3ffb`. O0/O2 cases
include plain and encoded names, underscores, UTF-8, operator aliases and
prefix collisions. A repeated miss scan checks retained private memory.
The predicate retains the existing matching rules; ownership, visibility and
overload decisions still belong to their original call sites.

`check-roots.ps1 -Compiler <current SDK compiler>` compares root-list membership
with the previous delimiter predicate at O0/O2, checks generic request order and
duplicates, and measures retained memory during repeated long-list lookups. A
real IR export checks that a selected generic specialization and ordinary root
are emitted while an unselected function is absent. The builder filters generic
requests once per compilation; ordinary root checks borrow the existing list.
