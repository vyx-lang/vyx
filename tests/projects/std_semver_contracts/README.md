# Semantic version consumer

```sh
vyxc --src=project . --run=aot -j4
```

Run from this directory so `catalog.txt` is the input. The separate catalog
module reads line-delimited release versions, chooses the highest permitted
precedence, retains the first record when only build metadata differs, returns
None when no stable version qualifies, and reports malformed records.

The project also checks strict parsing, byte offsets, i32 boundaries, long
numeric prerelease identifiers, scoped input destruction, deep Clone, container
sorting and hash-key agreement. Build metadata is retained in serialized values
but ignored by comparison, Eq and Hashable. Checked increments clear prerelease
and build metadata and reject overflow.

For O0/O2 and independent generated version comparisons, run the
[standard-library gate](../../../probes/gates/stdlib-contracts/README.md) with
`--projects std_semver_contracts`. Add `--negative` for convenience-API panic
checks. Package-manager version ranges are a separate API; this module accepts
complete SemVer versions, without a `v` prefix or whitespace normalization.
