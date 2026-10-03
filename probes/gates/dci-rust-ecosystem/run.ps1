param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { Write-Output 'dci-rust-ecosystem: SKIP (Windows Rust ABI gate)'; exit 77 }
$root = (Resolve-Path $PSScriptRoot).Path
$repo = (Resolve-Path (Join-Path $root '../../..')).Path
if (!$Compiler) { $Compiler = Join-Path $repo 'bootstrap_compiler/out/boot.exe' }
$Compiler = (Resolve-Path $Compiler).Path
$python = (Get-Command python -ErrorAction Stop).Source
$rustc = (Get-Command rustc -ErrorAction Stop).Source
$cargo = (Get-Command cargo -ErrorAction Stop).Source
$native = Join-Path $root 'native'
$cache = Join-Path $root '.cache'
$inputDir = Join-Path $cache 'inputs'
New-Item -ItemType Directory -Force $inputDir | Out-Null

function Checked([string]$exe, [string[]]$ArgumentList, [string]$label) {
    # Cargo and rustc write normal progress diagnostics to stderr.  Keep the
    # gate fail-closed on the process status without turning that stream into
    # a terminating PowerShell error under `$ErrorActionPreference = Stop`.
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $exe @ArgumentList > (Join-Path $cache "$label.out.log") 2> (Join-Path $cache "$label.err.log")
    $status = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorAction
    if ($status -ne 0) {
        Get-Content (Join-Path $cache "$label.out.log") -Tail 80 -ErrorAction SilentlyContinue
        Get-Content (Join-Path $cache "$label.err.log") -Tail 80 -ErrorAction SilentlyContinue
        throw "$label failed"
    }
}

Checked $cargo @('build','--manifest-path',(Join-Path $native 'Cargo.toml'),'--locked','--offline','--target','x86_64-pc-windows-msvc') 'cargo-build'
Checked $cargo @('run','--manifest-path',(Join-Path $native 'Cargo.toml'),'--locked','--offline','--target','x86_64-pc-windows-msvc','--', $inputDir) 'cargo-oracle'
$adapter = Join-Path $repo 'tools/dci/dci_adapter_rust.py'
foreach ($crate in @('crc32fast','adler2')) {
    $out = Join-Path $cache "$crate.dcib"
    $nativeLib = Join-Path $cache "$crate.lib"
    # Select the original registry package's public checksum function. The
    # Cargo fixture owns no wrapper API for this boundary.
    $items = if ($crate -eq 'crc32fast') { @('hash') } else { @('adler32_slice') }
    $adapterArguments = @($adapter,'--manifest-path',(Join-Path $native 'Cargo.toml'),'--package',$crate,'--offline','--locked','--emit-views','--native-lib-out',$nativeLib,'--artifact',$nativeLib,'--rustc',$rustc,'--target','x86_64-pc-windows-msvc','-o',$out)
    foreach ($item in $items) { $adapterArguments += @('--item',$item) }
    Checked $python $adapterArguments "adapter-$crate"
    if (!(Test-Path $out) -or !(Test-Path $nativeLib)) { throw "missing $crate DCI contract or native artifact" }
}
# Vyx reserves `hash` for its host hash builtin. Keep the producer's original
# Rust item selected above, and add a consumer spelling that points at the same
# measured bridge/link identity. This is a contract name alias, not a Rust or
# C ABI wrapper.
$aliasScript = @'
import copy, sys
from pathlib import Path
from tools.dci import dcib
p = Path(sys.argv[1])
doc = dcib.decode(p.read_bytes())
symbols = doc.get("exports", {}).get("symbols", [])
source = next((item for item in symbols if item.get("name") == "hash"), None)
if source is None:
    raise SystemExit("crc32fast contract did not retain the original hash export")
alias = copy.deepcopy(source)
alias["name"] = "crc32_hash"
alias["semantic_id"] = "crc32_hash(&[u8])"
alias["rust_name"] = "hash"
symbols.append(alias)
p.write_bytes(dcib.encode(doc))
'@
$env:PYTHONPATH = $repo
$aliasScript | & $python - (Join-Path $cache 'crc32fast.dcib')
if ($LASTEXITCODE -ne 0) { throw 'failed to add the Vyx consumer alias for crc32fast' }
$check = @'
from pathlib import Path
from tools.dci import dcib
for name in ("crc32fast", "adler2"):
    doc = dcib.decode((Path(".cache") / (name + ".dcib")).read_bytes())
    cargo = doc.get("source", {}).get("cargo", {})
    if not cargo.get("manifest_path") or not cargo.get("locked") or not cargo.get("offline"):
        raise SystemExit(f"{name}: missing locked Cargo provenance")
    if not any(x.get("type_name") == "dci.RustSlice_u8" for x in doc.get("exports", {}).get("layouts", [])):
        raise SystemExit(f"{name}: missing measured RustSlice_u8 view")
    symbols = doc.get("exports", {}).get("symbols", [])
    wanted = "crc32_hash" if name == "crc32fast" else "adler32_slice"
    if not any(x.get("name") == wanted and str(x.get("link_name", "")).startswith("__vyx_dci_export_") for x in symbols):
        raise SystemExit(f"{name}: native link bridge was not exported")
    if name == "crc32fast" and not any(x.get("name") == "hash" and x.get("rust_name") is None for x in symbols):
        raise SystemExit("crc32fast: original Rust hash export was not retained")
'@
Push-Location $root
try {
    $oldPythonPath = $env:PYTHONPATH
    $env:PYTHONPATH = $repo
    $check | & $python -
    $env:PYTHONPATH = $oldPythonPath
    if ($LASTEXITCODE -ne 0) { throw 'contract validation failed' }
    $nativeArgs = @('--link','kernel32','--link','ntdll','--link','userenv','--link','ws2_32','--link','dbghelp')
    foreach ($level in @('O0','O2')) {
        $exe = Join-Path $root "target/dci_rust_ecosystem_$level.exe"
        $arguments = @('--src=file','main.vyx','--emit=exe',"-$level",'--dci','.cache/crc32fast.dcib','--dci','.cache/adler2.dcib','--link-obj','.cache/crc32fast.lib','--link-obj','.cache/adler2.lib') + $nativeArgs + @('-o',$exe)
        Checked $Compiler $arguments "vyx-$level"
        if (!(Test-Path $exe)) { throw "Vyx AOT executable missing ($level)" }
        Checked $exe @() "vyx-$level-run"
        $actual = ((Get-Content -Raw (Join-Path $cache "vyx-$level-run.out.log")) -split '\s+' | Where-Object { $_ -ne '' })
        $expected = ((Get-Content -Raw (Join-Path $inputDir 'expected.txt')) -split '\s+' | Where-Object { $_ -ne '' })
        if (($actual -join ' ') -ne ($expected -join ' ')) { throw "Vyx $level output differs from Cargo oracle" }
        Write-Output "$level Cargo oracle parity OK"
    }
} finally { Pop-Location }
Write-Output 'dci-rust-ecosystem: Cargo locked crc32fast/adler2 + measured slice views + AOT O0/O2 OK'
