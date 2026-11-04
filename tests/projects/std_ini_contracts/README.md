# INI configuration consumer

```sh
vyxc --src=project . --run=aot -j4
```

Run from this directory so `config.ini` is the input. A separate module loads
typed application settings, requires an endpoint, chooses defaults only for
absent optional keys, and checks the application's port range. Invalid syntax
or typed values propagate as errors instead of silently selecting defaults.

The project checks LF/CRLF/CR, a UTF-8 BOM, root entries, whitespace, comments,
duplicate names, byte locations, signed i32/i64 boundaries, missing versus
empty values, borrowed document access, deep Clone and independently owned
results. Dotted section/key names and case-sensitive names have separate index
identities. Returned values remain valid after input/document destruction.

For O0/O2 plus generated reference checks, run from the repository root:

```sh
python probes/gates/stdlib-contracts/run.py --compiler /path/to/current/vyxc \
  --projects std_ini_contracts --negative --output .cache/ini-contracts
```

The model uses Python's `RawConfigParser` with case preservation, interpolation
disabled, `=` delimiters and whole-line comments. A synthetic section represents
root keys for Python versions without unnamed-section support. The generated
common subset checks 24 documents and 480 raw/typed lookups; a growing index
checks another 1024 values. This does not equate the two parsers' complete dialects.
That growth case uses a 4096-byte section name. Names are interned once, so
the index stores a bounded numeric section ID rather than repeating that name
in every key.

## API and dialect

Depend on `std:serde`, target `serde_ini`, then `use std.serde.ini;`.
`IniDocument.try_parse` returns `Result<IniDocument, IniError>`. Parse once and
reuse the document's index. `try_get` returns `Result<Option<String>, IniError>`;
`get` returns `Option<String>` and panics only for an invalid lookup name.
`try_get_i32`, `try_get_i64` and `try_get_bool` return optional typed values:
None means missing, an empty value is present and fails typed conversion.
String results and cloned documents own independent storage.

- Names are case-sensitive. Root keys use section `""`. Names cannot contain
  control bytes; section names cannot contain brackets, and keys cannot contain
  `=` or start with `[`, `#` or `;`. Outer ASCII spaces/tabs are trimmed when
  parsing names and values. Lookup names must already be trimmed.
- Assignments use the first `=`. Quotes, backslashes, percent signs, additional
  `=`, `#` and `;` inside values are literal. There is no quote unescaping,
  interpolation, DEFAULT inheritance or implicit multiline continuation.
- Whole-line comments start with `#` or `;` after optional indentation.
  A section header may have a trailing comment. Duplicate sections/keys and
  malformed lines anywhere in the document are errors. NUL and other control
  bytes are rejected except tabs and line separators.
- Decimal integers accept an optional sign and leading zeros, require digits
  throughout and check the requested width. Booleans accept true/false, yes/no,
  on/off and 1/0 without regard to ASCII case.
- Error offsets are zero-based byte positions; line/column are one-based byte
  coordinates in the original input. A leading BOM occupies three bytes in
  the first line's coordinates. Invalid lookup arguments use offset -1 and
  line/column 0 because they have no source position.

Legacy `ini_find`, `ini_find_int` and `ini_find_bool` parse the whole document.
Only missing keys keep the old empty/zero/false results. Malformed documents,
invalid integers/booleans and overflow panic; use document `try_*` APIs for
recoverable errors. The legacy text helper returns a copied compatibility str.
