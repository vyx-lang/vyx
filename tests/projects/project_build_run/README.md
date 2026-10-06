# Manifest build/run fixture

```sh
vyxc --run=aot --src=project . --target project_build_run -- "two words" ""
vyxc --run=aot --src=project . --target secondary
vyxc --emit=ir --src=project . --target project_build_run
```

The first executable links `native_lib/`, waits for the project postbuild hook,
prints its received argv, and exits with code 7 when its first argument is
`exit7`. The second executable makes target selection observable.

Run the automated checks from
[the project build/run gate](../../../probes/gates/project-build-run/README.md).
