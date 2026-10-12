# Container ownership smoke

Build with `../../../Zyn/bin/zyn.ps1 build -j20`, then run
`target/zyn_container_ownership_smoke.exe --self-test` from this directory.
Set `ZYN_VYXC` to the current SDK compiler when checking a compiler change.

The headless test checks component-list transfer order, repeated mutable
dispatch through stored trait objects, borrowed mounting, an independent view
snapshot after host destruction, and UTF-8 backspace with undo/redo.
It requires the current Zyn library and its staged native dependencies.
