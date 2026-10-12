# Vyx JSON package sources

`vyxc install`, `search`, `lock`, `update`, `publish` and `source` use this
Python 3.11+ standard-library tool. The SDK includes these modules under
`tools/packages/`; source builds locate the same directory in the repository.
`VYX_PACKAGE_TOOLS` can select another tools directory, and `VYX_PYTHON` can
select the Python executable. No install hooks run when fetching a package.

## Configure a source

An index can be an HTTP(S) URL, a `file://` URL, or a local JSON file. GitHub
raw files and private servers use the same index format.

```console
vyxc source add company https://packages.example.org/index.json --token-env COMPANY_PACKAGE_TOKEN
vyxc source list
vyxc source remove company
```

Sources are saved in `~/.vyx/sources.json`, or the file selected by
`VYX_PACKAGE_SOURCES`:

```json
{
  "schema": 1,
  "sources": [
    { "name": "company", "url": "https://packages.example.org/index.json", "token_env": "COMPANY_PACKAGE_TOKEN" }
  ]
}
```

Projects may declare sources in `Vyx.toml`:

```toml
[sources.company]
url = "https://packages.example.org/index.json"
token_env = "COMPANY_PACKAGE_TOKEN"

[sources.local]
url = "../package-index/index.json"

[dependencies]
geometry = { version = "^1.2", source = "company" }
data = { version = "~2.1", package = "data_core", source = "local" }
```

Project sources come first, followed by global sources not overridden by
name. Without `source`, the first source containing the package owns resolution;
an unavailable source, invalid index, or incompatible version requirement is
an error. Resolution does not skip these errors to use another source.
Graph identities include the configured source name and URL. Two named sources
at the same URL may use different bearer-token environment variables and return
different private package views; their indices and graph entries remain separate.

`token_env` supplies a bearer token. Tokens are not stored in locks or URLs.
Authenticated redirects to another origin are rejected, and a source's token
is not forwarded to an artifact hosted at another origin.

## Index format

```json
{
  "schema": 1,
  "packages": {
    "geometry": {
      "description": "Geometry primitives",
      "versions": {
        "1.2.0": {
          "url": "packages/geometry/1.2.0/geometry.tar.gz",
          "sha256": "<64 hexadecimal digits>",
          "strip_prefix": "geometry-1.2.0"
        }
      }
    }
  }
}
```

Archive URLs resolve relative to the index URL. Supported archives are ZIP and
tar archives, including tar.gz. Each archive must contain a `Vyx.toml` whose
package name and version match the index. Dependencies are read from that
manifest, rather than a second dependency declaration in the index.

Source releases may omit `kind` or use `"kind": "source"`. Binary releases and
DCI releases use exact platform artifacts:

```json
{
  "kind": "dci",
  "artifacts": [
    {
      "triplet": "x86_64-pc-windows-msvc",
      "abi": "msvc",
      "url": "packages/widgets/1.0.0/widgets-windows.zip",
      "sha256": "<64 hexadecimal digits>"
    }
  ]
}
```

The selected `(triplet, abi)` must identify exactly one artifact. Wildcards,
overlapping variants and missing platforms are errors. `kind = "binary"` uses
the same artifact format. The package manifest must agree with the selected
kind and platform:

```toml
[package]
name = "widgets"
version = "1.0.0"
kind = "dci"

[target.widgets]
type = "prebuilt"
artifact = "lib/widgets.lib"
linkage = "static"
triplet = "x86_64-pc-windows-msvc"
abi = "msvc"
sources = ["definitions/widgets.vyx"]
dci_files = ["contracts/widgets.dcib"]
runtime_files = []
```

`interface_sources` may list packaged Vyx interface files. All native artifacts,
definitions, interfaces, contracts and runtime files must be regular files
inside the package. A DCI package needs native artifacts, nonempty `sources`
and nonempty `dci_files`. Fetching an already produced DCI package does not
rerun its producer adapter or replace its definitions with C ABI wrappers.

## Install, resolve, and update

```console
vyxc install geometry@^1.2 --source company
vyxc install
vyxc lock
vyxc update
vyxc install --offline --locked
vyxc lock --triplet aarch64-linux-android --abi android
```

The host platform is the default. Other target triplets require an explicit
`--triplet`; ABI is inferred for MSVC, GNU/Linux, Android and Darwin, or supplied
with `--abi`. An existing lock for another platform is rejected until an
explicit `update --triplet ... --abi ...`.

Requirements support exact SemVer, `^`, `~`, comparisons such as
`>=1.2.0 <2.0.0`, `*`/`x` wildcards and `||` alternatives. Prereleases require
an explicit prerelease comparator. One version is selected per source/package
identity. Resolution backtracks version choices to satisfy the transitive graph;
unresolvable constraints and dependency cycles report errors.

Path dependencies, `std:` dependencies and same-manifest target dependencies
retain their existing build behavior. Target and template overlays apply before
package constraints are collected. All declared targets participate in the
lock; unused templates and fully overridden dependencies do not. Distinct
targets demanding incompatible versions of the same source/package still
conflict under the single-version rule.

`Vyx.packages.lock` records exact versions, source URLs, archive hashes, verified
payload digests, platform selection and dependency edges. It does not replace
the module-version `Vyx.lock`. Normal builds retain matching locked versions;
`update` deliberately selects again. A previously locked named-source/package/version
cannot change archive checksum or kind for the same platform.

`--locked` rejects changes to package dependency semantics or source configuration.
Comments, compiler flags and thread settings do not invalidate a lock.
`--offline` requires a matching lock and a verified local package cache.
Cache verification covers payload files and package metadata; generated top-level
`.cache`, `target` and `out` directories are allowed. Editing both a payload and
its local marker still fails against the digest stored in the lock. Archives
reject traversal, links, duplicate paths, case collisions and special files.

Caches live under `.cache/packages/<source-identity-hash>/<name>/<version>/<archive-hash>/`.
The source identity hash includes its configured name and normalized index URL;
it contains no token value.
Different sources cannot overwrite each other's packages. Root and sibling path
dependencies are recorded relative to the project; dependencies on another
Windows drive require an absolute path and are machine-specific.

## Publish to a local source

```console
vyxc publish --source local --dry-run
vyxc publish --source local
```

Publishing writes an archive and updates the selected local JSON index. HTTP
uploads and GitHub pushes are not part of this command. Existing source releases
and existing binary variants cannot be replaced.

Use `[package] include = ["src/**", "lib/**", "contracts/**"]` and `exclude`
arrays to select the payload. `Vyx.toml` is required. Hidden files/directories,
chat and IDE state, generated/cache directories, logs and temporary files are
excluded by default; symbolic links are rejected. `--dry-run` prints the actual
file list without publishing. Put binary distribution files in a payload
directory such as `lib/`, rather than a generated `out/` or `target/` directory.
The module-version `Vyx.lock` remains part of the payload when present; the
package-resolution `Vyx.packages.lock` is excluded.

Project, source configuration and local publication operations use operating
system file locks. Locks release when a process exits, including abnormal exit.

## Compiler integration API

```console
python package_manager.py resolve-path --project ROOT --owner OWNER --section target.app.dependencies --name geometry --output PATHFILE
```

The path query prepares or verifies the entire package graph, then writes the
absolute dependency directory as UTF-8 without surrounding quotes. Errors use
stderr and a nonzero exit code. `VYX_PACKAGE_PROJECT` carries the root into
recursive dependency builds so transitive packages share one lock and cache.
An optional `--map-output FILE` writes the verified graph as UTF-8 TSV rows
`absolute-owner`, `original-dependency-section`, `alias`, `absolute-destination`.
Tabs and newlines in field values are rejected. Verification results are reused
within a single CLI transaction; subsequent invocations verify the cache again.

Run the source, transport and archive regression gate with:

```console
python probes/gates/package-sources/test_sources.py
```
