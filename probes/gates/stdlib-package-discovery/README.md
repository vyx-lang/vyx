# Standard-package discovery

```sh
python probes/gates/stdlib-package-discovery/run.py --compiler /path/to/vyxc --output /path/to/results
```

Uses the supplied SDK compiler and its companion libraries in an isolated SDK
layout. Checks that a flat `std/` copy is not an implicit provider, package
discovery requires `std_packages/registry`, and explicitly supplied module
sources remain supported. These are semantic import checks; they do not certify
the standard library's runtime APIs.

For AOT package coverage, also run `probes/gates/bootstrap-native-stdlib/run.py`.
For editor bindings, use `probes/gates/editor-industrial/check_lsp_packages.py`.
