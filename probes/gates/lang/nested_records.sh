#!/usr/bin/env bash
# Nested records: `struct Outer<T> { struct Inner<U> { … } }`.
#
# A nested record is hoisted to module scope under the dotted name
# `Outer.Inner`, and its parameter list is the parent's list followed by its
# own -- unless the two lists are identical, in which case the nested record
# just re-states the parent's parameters and adds none:
#
#     struct FreqArray<T> { struct A<T>   { … } }   FreqArray<i32>::A
#     struct FreqArray<T> { struct A<U>   { … } }   FreqArray<i32>::A<f64>
#     struct FreqArray<T> { struct A<T,V> { … } }   FreqArray<i32>::A<i32,f64>
#     struct FreqArray<T> { struct A<K,V> { … } }   FreqArray<i32>::A<i64,f64>
#
# The use site always spells "arguments of the parent segment, then arguments
# of the nested segment"; the declaration must therefore concatenate its
# parameter lists the same way.  Deduplicating by name on the declaration side
# (what `parser_merge_nested_generics` did first) leaves the two sides
# disagreeing on the parameter count, and the tail arguments bind to the wrong
# parameters -- `Grp<i32>::A<i32,f64>` produced a 3-argument instance name for
# a 2-parameter record, whose `b: V` ended up typed `i32` and printed 0.
#
# Assertions:
#   1. the fixture (all five shapes above, a nested record that declares no
#      parameter list of its own -- it then inherits the parent's whole list --
#      `class` nesting with methods and constructors, and nested types in field
#      positions) builds and prints exactly the expected values;
#   2. the snippets in `docs/ADVANCED_FEATURES.md` / `docs/高级特性_ZH.md`
#      compile and print what they claim;
#   3. the deep chain flattens to one record with every level's arguments, in
#      order (`L1.L2.L3::<i32,f64,i8>`);
#   4. a use site that spells fewer arguments than the nested record declares
#      is rejected instead of silently binding the tail parameter to nothing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
export LLVM_ROOT="${LLVM_ROOT:-$ROOT/clang}"
export PATH="$LLVM_ROOT/bin:$PATH"
cd "$ROOT"

BOOT="${BOOT:-$ROOT/bootstrap_compiler/out/boot.exe}"
if [ ! -x "$BOOT" ]; then echo "nested_records FAIL: no boot at $BOOT"; exit 1; fi

to_win() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

WORK="$(mktemp -d /tmp/vyx_nested_records.XXXXXX)"
trap 'command rm -rf "$WORK"' EXIT

SRC="$(to_win "$ROOT/probes/gates/lang/nested_records.vyx")"
BAD="$(to_win "$ROOT/probes/gates/lang/nested_records_arity.vyx")"
EXE="$(to_win "$WORK/nested_records.exe")"

"$BOOT" --src=file "$SRC" --emit=exe -o "$EXE" >"$WORK/build.log" 2>&1 || {
  echo "nested_records FAIL: fixture did not build"
  sed -e 's/^/    /' "$WORK/build.log" | head -20
  exit 1
}

# Windows C runtime text-mode stdout maps "\n" to "\r\n"; the runtime never
# switches stdout to binary, so compare line content with the CR stripped
# rather than raw bytes.  Values, order and line count are still exact.
actual="$("$WORK/nested_records.exe" | tr -d '\r')"
expected="$(cat <<'EOT'
11
22.5
33
44.5
55
66.5
71
72.5
73
12
81
42
43
2.5
91
92.5
93
94.5
95
96.5
97
98.5
99
nested5-ok
EOT
)"
if [ "$actual" != "$expected" ]; then
  echo "nested_records FAIL: runtime output differs"
  diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") | sed -e 's/^/    /' | head -20
  exit 1
fi

# The documentation snippets must be a `boot` compile away from reality.
DOCS="$(to_win "$ROOT/probes/gates/lang/nested_records_docs.vyx")"
"$BOOT" --src=file "$DOCS" --emit=exe -o "$(to_win "$WORK/docs.exe")" >"$WORK/docs_build.log" 2>&1 || {
  echo "nested_records FAIL: documentation snippets do not compile"
  sed -e 's/^/    /' "$WORK/docs_build.log" | head -20
  exit 1
}
docs_expected="$(cat <<'EOT'
11 2.5 3 4.5 5 6.5
7 8.5 91 92.5 93
42
nested-docs-ok
EOT
)"
docs_actual="$("$WORK/docs.exe" | tr -d '\r')"
if [ "$docs_actual" != "$docs_expected" ]; then
  echo "nested_records FAIL: documentation snippets print the wrong values"
  diff <(printf '%s\n' "$docs_expected") <(printf '%s\n' "$docs_actual") | sed -e 's/^/    /' | head -20
  exit 1
fi

dump="$("$BOOT" --src=file "$SRC" --emit=mir 2>&1)"
if ! printf '%s\n' "$dump" | grep -q 'L1\.L2\.L3::<i32,f64,i8>'; then
  echo "nested_records FAIL: deep chain did not flatten to L1.L2.L3::<i32,f64,i8>"
  printf '%s\n' "$dump" | grep -n 'L1' | head -5 | sed -e 's/^/    /'
  exit 1
fi
# The parent segment's arguments come first, then the nested segment's -- so
# `Grp<i32>::A<i32,f64>` (a 3-parameter record) is instantiated with all three,
# not with two while a stale third one lingers in the name.
if ! printf '%s\n' "$dump" | grep -q 'Grp\.A::<i32,i32,f64>'; then
  echo "nested_records FAIL: Grp<i32>::A<i32,f64> did not instantiate Grp.A::<i32,i32,f64>"
  printf '%s\n' "$dump" | grep -n 'Grp' | head -5 | sed -e 's/^/    /'
  exit 1
fi

# Negative: two arguments for a three-parameter nested record must not compile.
if "$BOOT" --src=file "$BAD" --emit=obj -o "$(to_win "$WORK/bad.obj")" \
      >"$WORK/bad.log" 2>&1; then
  echo "nested_records FAIL: arity-mismatched use site compiled (expected a diagnostic)"
  exit 1
fi

echo "nested_records OK: 5 shapes + class nesting + doc snippets + deep chain + arity diagnostic"
