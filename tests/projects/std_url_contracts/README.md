# URL consumer

```sh
vyxc --src=project . --run=aot -j4
```

The separate `client_request` module prepares a search endpoint from an absolute
URL, preserving repeated query keys and omitting the fragment. The consumer
stores and reads its request target, decodes the parameters, and checks UTF-8,
literal plus, IPv6 and application error propagation. This exercises request
preparation; it does not test an HTTP transport.

The remaining checks cover all 256 byte values, percent versus form alphabets,
located errors, registered/IPv4/IPv6 hosts, port bounds, empty delimiters, owned
fields, deep Clone and rejection of inconsistent edited fields. Query pairs
preserve order, duplicates and empty keys/values. Empty `&` segments are skipped.

For O0/O2 plus independent Python `urllib.parse` / `ipaddress` model checks:

```sh
python probes/gates/stdlib-contracts/run.py --compiler /path/to/current/vyxc \
  --projects std_url_contracts --negative --output .cache/url-contracts
```

Run that command from the repository root. `--negative` includes the shared
standard-library rejection cases. Reports record the compiler/backend/runtime
identity and generated model inputs.

`Url` handles absolute HTTP, HTTPS, WS and WSS URLs. Relative resolution,
IDNA, browser normalization, IPvFuture and IPv6 zone identifiers are outside
this API. Ports must be nonempty decimal values in 0..65535. Numeric dotted
IPv4 addresses require four canonical decimal octets. ASCII registered hosts
are syntax-checked without DNS lookup. Escape sequences and letter case are
retained. The IPv6 `host` field excludes brackets.

`try_url_decode` preserves literal `+`; `try_form_decode` turns it into space.
Both return bytes in a length-bearing string, preserve NUL/non-UTF-8 data, and
reject malformed percent escapes. The form codec uses the form alphabet and
plus rules, but does not perform permissive UTF-8 replacement. Invalid input
causes the convenience methods to panic; `try_*` methods return a located error.

Component syntax follows [RFC 3986](https://www.rfc-editor.org/rfc/rfc3986).
The [form alphabet](https://url.spec.whatwg.org/#application-x-www-form-urlencoded-percent-encode-set)
is separate from URL component encoding; the strict byte APIs do not claim
WHATWG's permissive parsing and Unicode normalization behavior.
