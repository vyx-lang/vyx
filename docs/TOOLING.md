# Editors, language server, and debugging

[简体中文](TOOLING_ZH.md) · [Create a project](PROJECTS.md) · [Documentation](README.md)

Vyx supplies a compiler and project builder (`vyxc`), a language server
(`vyxc-lsp`), and a debug adapter (`vyxc-dap`). Editor plugins connect to these
executables. Use tools from the same SDK or compiler build.

Plugin sources live under `bootstrap_compiler/IDE-Plugins/` and are tracked in
this repository. The [ignore rules](../.gitignore) exclude dependencies, IDE
caches, build output, and packaged plugins. The Gradle wrapper and extension
packaging configuration are kept with the source.

## Language server

`vyxc-lsp --stdio` serves LSP clients. It provides diagnostics, completion,
hover, definition and reference lookup, rename, document and workspace symbols,
signature help, semantic tokens, formatting, code actions, and folding ranges.
Module completion includes registered packages such as `std.collections`.

References, document highlights and rename use compiler-resolved declarations
and bindings across files and modules. They distinguish shadowed locals,
parameters and unrelated members, and include import aliases, generic type
parameters, enums, globals, closure captures and string interpolation. Overloads
are renamed as a declaration family within their exact module and owner type.
Renaming an exported declaration updates import targets while preserving aliases;
renaming an alias updates only that alias and its uses.

Rename reanalyzes a temporary source overlay before returning edits, rejecting
binding capture, conflicts and lost bindings. Unsaved buffers participate;
clients supporting `documentChanges` receive document versions. Unresolved names,
semantic errors, read-only SDK declarations and external ABI names are refused.
See the [cross-module editor project](../tests/projects/editor_semantic_rename/README.md).

Open the project directory containing `Vyx.toml` so the editor has the project
context. Configure the executable path if the plugin cannot discover your SDK.

Closed files first supply declaration indexes. References and rename analyze
candidate files individually and retain only source positions and binding facts.
Dependencies supply signatures; inferred return types still require function
bodies. Standard imports select one complete provider: `VYX_STD_PACKAGES`, then
the workspace's canonical packages, then the SDK. Legacy `std/` is a fallback
only when no canonical registry exists. Other copies remain indexed for
navigation but are not merged into semantic analysis; SDK sources stay read-only.
Relative and absolute paths share one file identity.
Open buffers also receive full diagnostics. Analysis allocations are released after each request and each
indexed source. Replacing or closing a document releases its previous text,
symbols and diagnostics. Repeated pull diagnostics reuse the current result.
Generated directories (`out`, `target`, `build`, `dist`, caches, dependencies and
local seed workspaces) are excluded from discovery; explicitly opened source
files still receive analysis. This is source indexing, not a project build.

Request ownership also covers compiler-generated substring/copy allocations and
checked-pointer metadata. UTF-16 editor positions are converted at protocol
boundaries. Queued edits share an analysis pass after input becomes quiet;
requests flush preceding changes and diagnostics carry document versions.
See the [editor pressure gates](../probes/gates/editor-industrial/README.md)
for sustained edits, token responses, synchronization and memory failure guards.

## VS Code and Cursor

The extension at `bootstrap_compiler/IDE-Plugins/vscode/vyx/` declares
VS Code `^1.110.0` compatibility. Cursor must support that extension API version.
For a supplied VSIX, use **Extensions: Install from VSIX**.

To run the extension from source, install Node.js and npm, then run:

```sh
cd bootstrap_compiler/IDE-Plugins/vscode/vyx
npm ci
npm run compile
```

Open this extension directory in VS Code and press F5 to start an Extension
Development Host. Open your Vyx project in that window.

Set these paths in user settings or `.vscode/settings.json`, replacing the
example SDK location with your installation:

```json
{
  "vyx.compilerPath": "C:/Tools/vyx/bin/vyxc.exe",
  "vyx.lspPath": "C:/Tools/vyx/bin/vyxc-lsp.exe",
  "vyx.dapPath": "C:/Tools/vyx/bin/vyxc-dap.exe"
}
```

On Linux, use paths to the executables without `.exe`. The extension also
discovers tools from its bundled directories, workspace compiler output,
environment variables, and `PATH`.

The command palette includes **Vyx: Run Current File**, **Vyx: Build Project**,
**Vyx: Debug Current File**, **Vyx: Format File**, and **Vyx: Restart Language
Server**. Syntax highlighting and snippets cover `.vyx` and `.vyi`; build tasks
can use the `$vyx` problem matcher. Check the **Vyx** output channel when tool
startup fails. Set `vyx.trace.server` to `verbose` for LSP troubleshooting.

## Debug a project

Build with debug information and without optimization:

```sh
vyxc build --target hello_app -g -O0
```

For the default project output directory, create `.vscode/launch.json`:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "vyx",
      "request": "launch",
      "name": "Debug hello_app",
      "program": "${workspaceFolder}/target/hello_app.exe",
      "cwd": "${workspaceFolder}",
      "args": []
    }
  ]
}
```

On Linux, remove `.exe`. If you changed `build.output_dir` to `out`, update
`program` accordingly. Rebuild after changing source; this launch configuration
does not itself invoke a build. **Debug Current File** is a separate command
that compiles the selected file with debug information before launching it.

`vyxc-dap --stdio` delegates the protocol directly to CodeLLDB, on both
Windows and Linux. The debugger owns asynchronous target execution, paged stack
frames, lazy variable expansion, output streaming and session cleanup. The Vyx
entry point does not retain copies of scopes, variables or target output.
Features are negotiated through the actual debugger's `initialize` response.

The complete SDK supplies `bin/debugger/adapter/codelldb` and its matching LLDB
runtime. Source builds prepare this bundle with
`python scripts/prepare_debug_adapter.py` from `bootstrap_compiler/`. The
archives are version pinned and SHA-256 checked. Cargo/Rust, Git and clang++
are required to build the adapter from source; SDK users do not need them.
The Vyx patch adds variable paging and separates Windows internal-console output
from DAP transport. Packaging verifies the patch and executable hashes and
preserves component licenses. See [debugger build details](../tools/debugger/README.md).
To select another CodeLLDB or LLDB-DAP installation, set `VYX_DAP_ADAPTER` to its
executable. The older `VYX_LLDB_DAP` variable remains accepted. In VS Code/Cursor,
`debuggerPath` in a launch configuration selects the same override. Old `lldb`
CLI paths select the sibling `lldb-dap`. Missing adapters produce an explicit
startup error. Build the program with `-g -O0` for source and local-variable debug
information. See [CodeLLDB](https://github.com/vadimcn/codelldb/blob/v1.12.3/MANUAL.md).

## IntelliJ IDEA and CLion

The plugin at `bootstrap_compiler/IDE-Plugins/IDEA/vyx/`
targets platform build 262 (2026.2) or later with the native DAP module.
Its source build requires JDK 25 and a compatible local IDE installation.

From the plugin directory on Windows:

```powershell
cd bootstrap_compiler/IDE-Plugins/IDEA/vyx
.\gradlew.bat buildPlugin "-PvyxIdePath=C:/Tools/CLion"
```

On Linux:

```sh
cd bootstrap_compiler/IDE-Plugins/IDEA/vyx
./gradlew buildPlugin -PvyxIdePath=/path/to/compatible/ide
```

Pass `-PvyxSdkPath=/path/to/packaged/sdk` to include a complete SDK in the plugin:
compiler, language server, debug adapter, runtime, standard packages and LLVM
driver. Without this option, configure a complete SDK for builds outside the
source checkout.

Install the resulting ZIP under `build/distributions` using **Settings → Plugins
→ Install Plugin from Disk**. In **Settings → Languages & Frameworks → Vyx**,
configure `vyxc`, `vyxc-lsp`, and `vyxc-dap` from your SDK. The plugin provides
project creation for executable, static-library, and shared-library projects,
language-server integration, Vyx run configurations, and DAP debugging.
The executable and LLDB requirements are the same as above.

**Build & Run Project** invokes `vyxc --run=aot --src=project <dir>` in the configured working
directory and forwards the selected manifest target. Program arguments go after
`--`. Debug builds use `-g -O0 --artifact-file <temporary-file>` and launch the
compiler-reported executable path, including target-specific output directories.

## Building the servers from the repository

With a working Vyx SDK and the compiler build prerequisites installed:

```sh
cd bootstrap_compiler
vyxc build --target vyxc-lsp
vyxc build --target vyxc-dap
```

Build instructions and runtime requirements are in the
[compiler README](../bootstrap_compiler/README.md).
The active implementations are [lsp_main.vyx](../bootstrap_compiler/src/core/tooling/lsp_main.vyx)
and [dap_main.vyx](../bootstrap_compiler/src/core/tooling/dap_main.vyx).
