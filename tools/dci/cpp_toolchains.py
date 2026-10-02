#!/usr/bin/env python3
"""C++ compiler discovery and target-policy helpers for DCI adapters.

This module deliberately contains no header parsing or ABI extraction logic.
It gives C++ adapters one shared, deterministic vocabulary for compiler
identity, driver syntax, target compatibility and descriptor metadata.

The compiler frontend and the C++ ABI are separate identities.  Clang may
produce either the Microsoft or Itanium C++ ABI, GCC produces the Itanium ABI,
and MSVC produces the Microsoft ABI.  Callers must validate the requested
target before asking a backend to extract facts; unsupported combinations are
rejected rather than silently falling back to another compiler.
"""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import platform as host_platform
import re
import shutil
import subprocess
from typing import Any, Iterable, Mapping, Sequence


TOOL_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOL_DIR.parents[1]

SUPPORTED_COMPILER_FAMILIES = ("clang", "gcc", "msvc")

TARGET_ALIASES = {
    "x64_windows": "x86_64-pc-windows-msvc",
    "windows_x64": "x86_64-pc-windows-msvc",
    "x64-windows": "x86_64-pc-windows-msvc",
    "arm64_windows": "aarch64-pc-windows-msvc",
    "windows_arm64": "aarch64-pc-windows-msvc",
    "arm64-windows": "aarch64-pc-windows-msvc",
    "x86_windows": "i686-pc-windows-msvc",
    "windows_x86": "i686-pc-windows-msvc",
    "x64_linux": "x86_64-unknown-linux-gnu",
    "linux_x64": "x86_64-unknown-linux-gnu",
    "x64-linux": "x86_64-unknown-linux-gnu",
    "arm64_linux": "aarch64-unknown-linux-gnu",
    "linux_arm64": "aarch64-unknown-linux-gnu",
    "arm64-linux": "aarch64-unknown-linux-gnu",
    "x86_linux": "i686-unknown-linux-gnu",
    "linux_x86": "i686-unknown-linux-gnu",
    "arm64_android": "aarch64-linux-android23",
    "android_arm64": "aarch64-linux-android23",
    "arm64-android": "aarch64-linux-android23",
    "x64_android": "x86_64-linux-android23",
    "android_x64": "x86_64-linux-android23",
    "x64-android": "x86_64-linux-android23",
    "x64_windows_gnu": "x86_64-w64-windows-gnu",
    "windows_x64_gnu": "x86_64-w64-windows-gnu",
    "x64_mingw": "x86_64-w64-windows-gnu",
}


class CppToolchainError(RuntimeError):
    """Raised when a requested C++ compiler/target contract is unavailable."""


@dataclass(frozen=True)
class TargetInfo:
    triple: str
    architecture: str
    platform: str
    object_format: str
    pointer_width: int
    endian: str
    abi_family: str
    data_model: str
    long_width: int

    def descriptor(self) -> dict[str, Any]:
        return {
            "triple": self.triple,
            "architecture": self.architecture,
            "platform": self.platform,
            "object_format": self.object_format,
            "pointer_width": self.pointer_width,
            "endian": self.endian,
            "endianness": self.endian,
            "abi_family": self.abi_family,
            "data_model": self.data_model,
            "long_width": self.long_width,
        }


@dataclass(frozen=True)
class CppToolchain:
    family: str
    executable: str
    version: str
    version_line: str
    native_target: str
    driver_mode: str

    @property
    def name(self) -> str:
        return self.family

    def identity(self) -> dict[str, str]:
        identity = {"name": self.family}
        if self.version:
            identity["version"] = self.version
        if self.version_line:
            identity["build"] = self.version_line
        return identity


def canonical_target(value: str) -> str:
    target = value.strip()
    if not target:
        return ""
    return TARGET_ALIASES.get(target.lower(), target)


def _target_segments(target: str) -> list[str]:
    return [part for part in canonical_target(target).lower().split("-") if part]


def _canonical_architecture(value: str) -> str:
    arch = value.strip().lower()
    if arch in {"x86_64", "amd64", "x64"}:
        return "x86_64"
    if arch in {"aarch64", "arm64"}:
        return "aarch64"
    if arch in {"i386", "i486", "i586", "i686", "x86"}:
        return "x86"
    if arch.startswith("armv7") or arch == "arm":
        return "arm"
    if arch.startswith("riscv64"):
        return "riscv64"
    if arch.startswith("riscv32"):
        return "riscv32"
    if arch.startswith("wasm64"):
        return "wasm64"
    if arch.startswith("wasm32"):
        return "wasm32"
    if arch in {"powerpc64le", "ppc64le"}:
        return "powerpc64le"
    if arch in {"powerpc64", "ppc64"}:
        return "powerpc64"
    return arch


def target_info(value: str) -> TargetInfo:
    triple = canonical_target(value)
    if not triple:
        raise CppToolchainError("empty C++ target triple")
    lowered = triple.lower()
    segments = _target_segments(triple)
    architecture = _canonical_architecture(segments[0] if segments else "")

    if "android" in lowered:
        target_platform, object_format = "android", "elf"
    elif any(token in lowered for token in ("windows", "mingw", "msvc")):
        target_platform, object_format = "windows", "coff"
    elif "darwin" in lowered or "apple" in lowered or "macos" in lowered:
        target_platform, object_format = "darwin", "macho"
    elif "linux" in lowered:
        target_platform, object_format = "linux", "elf"
    elif "wasi" in lowered:
        target_platform, object_format = "wasi", "wasm"
    else:
        target_platform, object_format = "", ""

    if "msvc" in lowered:
        abi_family = "msvc"
    elif target_platform in {"android", "linux", "darwin", "wasi"} or any(
        token in lowered for token in ("gnu", "mingw", "musl")
    ):
        abi_family = "itanium"
    else:
        abi_family = ""

    if architecture in {
        "x86_64",
        "aarch64",
        "riscv64",
        "wasm64",
        "powerpc64",
        "powerpc64le",
    }:
        pointer_width = 64
    elif architecture in {"x86", "arm", "riscv32", "wasm32"}:
        pointer_width = 32
    else:
        pointer_width = 0

    endian = "big" if architecture == "powerpc64" else "little"
    if pointer_width == 32:
        data_model, long_width = "ilp32", 32
    elif pointer_width == 64 and target_platform == "windows":
        data_model, long_width = "llp64", 32
    elif pointer_width == 64:
        data_model, long_width = "lp64", 64
    else:
        data_model, long_width = "", 0

    return TargetInfo(
        triple=triple,
        architecture=architecture,
        platform=target_platform,
        object_format=object_format,
        pointer_width=pointer_width,
        endian=endian,
        abi_family=abi_family,
        data_model=data_model,
        long_width=long_width,
    )


def normalize_compiler_family(value: str) -> str:
    family = value.strip().lower()
    aliases = {
        "clang++": "clang",
        "clang-cl": "clang",
        "llvm": "clang",
        "g++": "gcc",
        "gnu": "gcc",
        "mingw": "gcc",
        "cl": "msvc",
        "cl.exe": "msvc",
        "visual-c++": "msvc",
        "visual_cpp": "msvc",
    }
    return aliases.get(family, family)


def infer_compiler_family(executable: str, version_output: str = "") -> str:
    output = version_output.lower()
    if "apple clang" in output or "clang version" in output:
        return "clang"
    if "microsoft (r) c/c++" in output or "microsoft c/c++" in output:
        return "msvc"
    if "free software foundation" in output or re.search(r"\bg(?:cc|\+\+)\b", output):
        return "gcc"

    name = Path(executable).name.lower()
    if name.endswith(".exe"):
        name = name[:-4]
    if name == "cl":
        return "msvc"
    if "clang" in name:
        return "clang"
    if name in {"g++", "gcc"} or name.endswith(("-g++", "-gcc")):
        return "gcc"
    return ""


def compiler_driver_mode(family: str, executable: str) -> str:
    normalized = normalize_compiler_family(family)
    name = Path(executable).name.lower()
    if name.endswith(".exe"):
        name = name[:-4]
    if normalized == "clang":
        return "clang-cl" if "clang-cl" in name else "clang"
    if normalized == "gcc":
        return "gnu"
    if normalized == "msvc":
        return "cl"
    return ""


def _first_nonempty_line(text: str) -> str:
    for line in text.splitlines():
        clean = line.strip()
        if clean:
            return clean
    return ""


def _compiler_version(family: str, output: str) -> str:
    patterns = {
        "clang": r"(?:Apple\s+)?clang version\s+([^\s]+)",
        "gcc": r"\b(\d+\.\d+(?:\.\d+)?)\b",
        "msvc": r"\bVersion\s+(\d+\.\d+(?:\.\d+)?)\b",
    }
    match = re.search(patterns.get(family, r"$^"), output, re.IGNORECASE)
    if match is None and family == "msvc":
        # `/Bv` is localized.  The executable inventory line remains stable
        # enough to identify cl.exe even when the word "Version" is translated.
        match = re.search(
            r"(?mi)\bcl(?:\.exe)?\s*:\s*\D*(\d+\.\d+(?:\.\d+){1,2})\b",
            output,
        )
    return match.group(1) if match else ""


def _compiler_version_line(family: str, output: str, version: str) -> str:
    if family != "msvc":
        return _first_nonempty_line(output)
    for line in output.splitlines():
        clean = line.strip()
        if "Microsoft" in clean and "C/C++" in clean:
            return clean
    # Do not persist the absolute cl.exe path or localized `/Bv` headings in
    # a deterministic descriptor.  The normalized identity is sufficient.
    return ("MSVC " + version) if version else "MSVC"


def _target_from_version_output(output: str) -> str:
    match = re.search(r"(?mi)^\s*Target:\s*([^\s]+)", output)
    return canonical_target(match.group(1)) if match else ""


def _msvc_target_from_path(executable: str, env: Mapping[str, str]) -> str:
    configured = env.get("VSCMD_ARG_TGT_ARCH", "").strip().lower()
    path = executable.replace("/", "\\").lower()
    match = re.search(r"\\host(?:x64|x86|arm64)\\(x64|x86|arm64)\\cl(?:\.exe)?$", path)
    arch = match.group(1) if match else configured
    if not arch:
        arch = host_platform.machine().lower()
    canonical_arch = _canonical_architecture(arch)
    if canonical_arch == "x86_64":
        return "x86_64-pc-windows-msvc"
    if canonical_arch == "aarch64":
        return "aarch64-pc-windows-msvc"
    if canonical_arch == "x86":
        return "i686-pc-windows-msvc"
    return ""


def _run_capture(command: Sequence[str], timeout: int = 10) -> tuple[int, str]:
    try:
        proc = subprocess.run(
            list(command),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise CppToolchainError(f"cannot execute C++ compiler {command[0]!r}: {exc}") from exc
    return proc.returncode, proc.stdout or ""


def identify_cpp_toolchain(
    executable: str,
    *,
    expected_family: str = "",
    env: Mapping[str, str] | None = None,
) -> CppToolchain:
    environment = os.environ if env is None else env
    path = _resolve_candidate(executable)
    if not path:
        raise CppToolchainError(f"C++ compiler executable not found: {executable}")

    name_family = infer_compiler_family(path)
    probe_family = normalize_compiler_family(expected_family) or name_family
    if probe_family == "msvc":
        _rc, output = _run_capture([path, "/Bv", "/nologo"])
    else:
        _rc, output = _run_capture([path, "--version"])
    family = infer_compiler_family(path, output) or name_family
    if family not in SUPPORTED_COMPILER_FAMILIES:
        raise CppToolchainError(
            f"cannot identify C++ compiler family for {path!r}; expected Clang, GCC or MSVC"
        )
    expected = normalize_compiler_family(expected_family)
    if expected and family != expected:
        raise CppToolchainError(
            f"C++ compiler {path!r} is {family}, not requested family {expected}"
        )

    driver_mode = compiler_driver_mode(family, path)
    native_target = _target_from_version_output(output)
    if family in {"clang", "gcc"} and not native_target:
        rc, machine = _run_capture([path, "-dumpmachine"])
        if rc == 0:
            native_target = canonical_target(_first_nonempty_line(machine))
    if family == "msvc":
        native_target = _msvc_target_from_path(path, environment)

    version = _compiler_version(family, output)
    return CppToolchain(
        family=family,
        executable=path,
        version=version,
        version_line=_compiler_version_line(family, output, version),
        native_target=native_target,
        driver_mode=driver_mode,
    )


def _resolve_candidate(candidate: str) -> str:
    value = os.path.expandvars(os.path.expanduser(candidate.strip()))
    if not value:
        return ""
    path = Path(value)
    if path.is_file():
        return str(path.resolve())
    found = shutil.which(value)
    return str(Path(found).resolve()) if found else ""


def _append_unique(values: list[str], candidate: str) -> None:
    clean = candidate.strip()
    if clean and clean not in values:
        values.append(clean)


def _clang_candidates(env: Mapping[str, str]) -> Iterable[str]:
    values: list[str] = []
    _append_unique(values, env.get("CLANGXX", ""))
    cxx = env.get("CXX", "")
    if infer_compiler_family(cxx) == "clang":
        _append_unique(values, cxx)
    local_bin = REPO_ROOT / "clang" / "bin"
    for name in ("clang++.exe", "clang-cl.exe", "clang++", "clang-cl"):
        candidate = local_bin / name
        if candidate.is_file():
            _append_unique(values, str(candidate))
    for name in ("clang++", "clang-cl"):
        _append_unique(values, name)
    return values


def _gcc_prefix_for_target(target: str) -> str:
    info = target_info(target)
    if info.platform == "windows":
        if info.architecture == "x86_64":
            return "x86_64-w64-mingw32-"
        if info.architecture == "x86":
            return "i686-w64-mingw32-"
    if info.platform == "linux":
        if info.architecture == "x86_64":
            return "x86_64-linux-gnu-"
        if info.architecture == "aarch64":
            return "aarch64-linux-gnu-"
        if info.architecture == "arm":
            return "arm-linux-gnueabihf-"
    return ""


def _gcc_candidates(env: Mapping[str, str], target: str) -> Iterable[str]:
    values: list[str] = []
    _append_unique(values, env.get("GXX", ""))
    cxx = env.get("CXX", "")
    if infer_compiler_family(cxx) == "gcc":
        _append_unique(values, cxx)
    if target:
        prefix = _gcc_prefix_for_target(target)
        if prefix:
            _append_unique(values, prefix + "g++")
    for name in ("g++", "gcc"):
        _append_unique(values, name)
    return values


def _msvc_install_roots(env: Mapping[str, str]) -> Iterable[Path]:
    roots: list[Path] = []
    tools = env.get("VCToolsInstallDir", "").strip()
    if tools:
        roots.append(Path(tools))
    install = env.get("VSINSTALLDIR", "").strip()
    if install:
        tools_root = Path(install) / "VC" / "Tools" / "MSVC"
        if tools_root.is_dir():
            roots.extend(
                sorted(
                    (path for path in tools_root.iterdir() if path.is_dir()),
                    reverse=True,
                )
            )
    vswhere_candidates = [
        env.get("VSWHERE", ""),
        str(
            Path(env.get("ProgramFiles(x86)", ""))
            / "Microsoft Visual Studio"
            / "Installer"
            / "vswhere.exe"
        )
        if env.get("ProgramFiles(x86)")
        else "",
        "vswhere.exe",
        "vswhere",
    ]
    vswhere = next(
        (
            resolved
            for candidate in vswhere_candidates
            if candidate and (resolved := _resolve_candidate(candidate))
        ),
        "",
    )
    if vswhere:
        rc, output = _run_capture(
            [
                vswhere,
                "-products",
                "*",
                "-requires",
                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-property",
                "installationPath",
            ]
        )
        if rc == 0:
            for line in output.splitlines():
                clean = line.strip()
                if not clean:
                    continue
                installation = Path(clean)
                tools_root = installation / "VC" / "Tools" / "MSVC"
                if tools_root.is_dir():
                    roots.extend(
                        sorted(
                            (path for path in tools_root.iterdir() if path.is_dir()),
                            reverse=True,
                        )
                    )
    deduplicated: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        key = str(root).lower()
        if key not in seen:
            seen.add(key)
            deduplicated.append(root)
    return deduplicated


def _msvc_candidates(env: Mapping[str, str], target: str) -> Iterable[str]:
    values: list[str] = []
    cxx = env.get("CXX", "")
    if infer_compiler_family(cxx) == "msvc":
        _append_unique(values, cxx)
    target_arch = "x64"
    if target:
        arch = target_info(target).architecture
        if arch == "aarch64":
            target_arch = "arm64"
        elif arch == "x86":
            target_arch = "x86"
    for root in _msvc_install_roots(env):
        for host in ("Hostx64", "Hostx86"):
            _append_unique(values, str(root / "bin" / host / target_arch / "cl.exe"))
    _append_unique(values, "cl.exe")
    _append_unique(values, "cl")
    return values


def _candidate_sequence(family: str, env: Mapping[str, str], target: str) -> Iterable[str]:
    if family == "clang":
        return _clang_candidates(env)
    if family == "gcc":
        return _gcc_candidates(env, target)
    if family == "msvc":
        return _msvc_candidates(env, target)
    return ()


def discover_cpp_toolchain(
    selection: str = "auto",
    *,
    executable: str | None = None,
    target: str = "",
    env: Mapping[str, str] | None = None,
) -> CppToolchain:
    environment = os.environ if env is None else env
    requested = selection.strip() or "auto"
    normalized = normalize_compiler_family(requested)

    if executable:
        expected = "" if normalized == "auto" else normalized
        toolchain = identify_cpp_toolchain(executable, expected_family=expected, env=environment)
        if target:
            validate_target_compatibility(toolchain, target)
        return toolchain

    if normalized not in {"auto", *SUPPORTED_COMPILER_FAMILIES}:
        toolchain = identify_cpp_toolchain(requested, env=environment)
        if target:
            validate_target_compatibility(toolchain, target)
        return toolchain

    families: list[str]
    if normalized != "auto":
        families = [normalized]
    else:
        families = []
        cxx = environment.get("CXX", "").strip()
        cxx_family = infer_compiler_family(cxx) if cxx else ""
        if cxx_family:
            families.append(cxx_family)
        target_abi = target_info(target).abi_family if target else ""
        preferred = ["clang", "msvc", "gcc"] if target_abi == "msvc" else ["clang", "gcc", "msvc"]
        for family in preferred:
            if family not in families:
                families.append(family)

    attempted: list[str] = []
    last_error: CppToolchainError | None = None
    for family in families:
        for candidate in _candidate_sequence(family, environment, target):
            resolved = _resolve_candidate(candidate)
            if not resolved or resolved in attempted:
                continue
            attempted.append(resolved)
            try:
                toolchain = identify_cpp_toolchain(
                    resolved, expected_family=family, env=environment
                )
                if target:
                    validate_target_compatibility(toolchain, target)
                return toolchain
            except CppToolchainError as exc:
                last_error = exc
                continue
    if normalized != "auto" and last_error is not None:
        raise last_error
    requested_label = normalized if normalized != "auto" else "Clang, GCC or MSVC"
    suffix = f" for target {canonical_target(target)!r}" if target else ""
    raise CppToolchainError(f"C++ compiler not found: {requested_label}{suffix}")


# Compatibility aliases for callers that prefer find/probe terminology.
find_cpp_toolchain = discover_cpp_toolchain
probe_cpp_toolchain = identify_cpp_toolchain


def _triples_equivalent(lhs: str, rhs: str) -> bool:
    try:
        left = target_info(lhs)
        right = target_info(rhs)
    except CppToolchainError:
        return False
    return (
        left.architecture == right.architecture
        and left.platform == right.platform
        and left.abi_family == right.abi_family
    )


def target_compatibility_error(toolchain: CppToolchain, target: str) -> str:
    try:
        info = target_info(target)
    except CppToolchainError as exc:
        return str(exc)
    if not info.architecture or not info.platform or not info.abi_family:
        return f"unsupported C++ target triple {info.triple!r}"
    if toolchain.family == "msvc":
        if info.platform != "windows" or info.abi_family != "msvc":
            return "MSVC only supports Windows targets using the Microsoft C++ ABI"
        if not toolchain.native_target:
            return "cannot determine the MSVC driver target architecture"
        if not _triples_equivalent(toolchain.native_target, info.triple):
            return (
                f"MSVC driver target {toolchain.native_target!r} does not match "
                f"requested target {info.triple!r}"
            )
        return ""
    if toolchain.family == "gcc":
        if info.abi_family != "itanium":
            return "GCC does not produce the Microsoft C++ ABI; select GCC/MinGW or an Itanium-ABI target"
        if not toolchain.native_target:
            return "cannot determine the GCC driver target; `g++ -dumpmachine` failed"
        if not _triples_equivalent(toolchain.native_target, info.triple):
            return (
                f"GCC driver target {toolchain.native_target!r} does not match requested "
                f"target {info.triple!r}; use a matching cross-G++ executable"
            )
        return ""
    if toolchain.family == "clang":
        if toolchain.driver_mode == "clang-cl" and (
            info.platform != "windows" or info.abi_family != "msvc"
        ):
            return "clang-cl only supports Windows targets using the Microsoft C++ ABI"
        return ""
    return f"unsupported C++ compiler family {toolchain.family!r}"


def validate_target_compatibility(toolchain: CppToolchain, target: str) -> TargetInfo:
    error = target_compatibility_error(toolchain, target)
    if error:
        raise CppToolchainError(error)
    return target_info(target)


def compiler_identity_metadata(
    toolchain: CppToolchain,
    *,
    flags: Sequence[str] | None = None,
) -> dict[str, Any]:
    """Return descriptive compiler identity for a contract.

    The identity is a *snapshot annotation*, never a consume-time equality
    check.  It records the vendor family, the compiler's own version string,
    the descriptive target triple and the layout-affecting flags used at
    generation.  ``name``/``build`` are preserved for existing consumers;
    ``vendor`` is the stable alias of the family used by newer tooling.
    """
    identity: dict[str, Any] = {"name": toolchain.family, "vendor": toolchain.family}
    if toolchain.version:
        identity["version"] = toolchain.version
    if toolchain.version_line:
        identity["build"] = toolchain.version_line
    if toolchain.native_target:
        identity["target_triplet"] = toolchain.native_target
    identity["flags"] = list(flags) if flags else []
    return identity


def adapter_identity_name(toolchain: CppToolchain, target: str) -> str:
    info = validate_target_compatibility(toolchain, target)
    return f"cpp-{toolchain.family}-{info.abi_family}"


def producer_metadata(
    toolchain: CppToolchain,
    target: str,
    *,
    flags: Sequence[str] | None = None,
) -> dict[str, Any]:
    info = validate_target_compatibility(toolchain, target)
    return {
        "kind": "adapter",
        "tool": toolchain.family,
        "backend": info.abi_family,
        "source_language": "cpp",
        "compiler": compiler_identity_metadata(toolchain, flags=flags),
        "driver_mode": toolchain.driver_mode,
    }


def source_metadata(
    toolchain: CppToolchain,
    target: str,
    *,
    headers: Sequence[str] = (),
    adapter_version: str = "1.0",
    flags: Sequence[str] | None = None,
) -> dict[str, Any]:
    info = validate_target_compatibility(toolchain, target)
    adapter: dict[str, str] = {"name": adapter_identity_name(toolchain, target)}
    if adapter_version:
        adapter["version"] = adapter_version
    return {
        "language": "cpp",
        "compiler": compiler_identity_metadata(toolchain, flags=flags),
        "adapter": adapter,
        "headers": list(headers),
        "target": {
            "triple": info.triple,
            "architecture": info.architecture,
            "abi": info.abi_family,
        },
    }


def profile_extension_metadata(
    toolchain: CppToolchain,
    target: str,
    *,
    rtti_enabled: bool = True,
    flags: Sequence[str] | None = None,
) -> dict[str, Any]:
    info = validate_target_compatibility(toolchain, target)
    key = f"{toolchain.family}-{info.abi_family}"
    extension: dict[str, Any] = {
        "compiler": compiler_identity_metadata(toolchain, flags=flags),
        "driver_mode": toolchain.driver_mode,
        "record_layout": info.abi_family,
        "name_mangling": "microsoft" if info.abi_family == "msvc" else "itanium",
        "data_model": info.data_model,
        "long_width": info.long_width,
    }
    if info.abi_family == "msvc":
        extension["virtual_base_table"] = {
            "entry_size": 4,
            "entry_signed": True,
            "displacement_origin": "table_pointer_field",
        }
        extension["runtime_type"] = {
            "representation": "msvc-col",
            "checked_cast_runtime": "__RTDynamicCast",
            "rtti_enabled": rtti_enabled,
        }
    else:
        extension["runtime_type"] = {
            "representation": "itanium-typeinfo",
            "checked_cast_runtime": "__dynamic_cast",
            "rtti_enabled": rtti_enabled,
        }
    return {key: extension}


def descriptor_metadata(
    toolchain: CppToolchain,
    target: str,
    *,
    headers: Sequence[str] = (),
    adapter_version: str = "1.0",
    rtti_enabled: bool = True,
    flags: Sequence[str] | None = None,
) -> dict[str, Any]:
    info = validate_target_compatibility(toolchain, target)
    return {
        "target": info.descriptor(),
        "producer": producer_metadata(toolchain, target, flags=flags),
        "profile_extension": profile_extension_metadata(
            toolchain, target, rtti_enabled=rtti_enabled, flags=flags
        ),
        "source": source_metadata(
            toolchain,
            target,
            headers=headers,
            adapter_version=adapter_version,
            flags=flags,
        ),
    }


__all__ = [
    "CppToolchain",
    "CppToolchainError",
    "SUPPORTED_COMPILER_FAMILIES",
    "TARGET_ALIASES",
    "TargetInfo",
    "adapter_identity_name",
    "canonical_target",
    "compiler_driver_mode",
    "compiler_identity_metadata",
    "descriptor_metadata",
    "discover_cpp_toolchain",
    "find_cpp_toolchain",
    "identify_cpp_toolchain",
    "infer_compiler_family",
    "normalize_compiler_family",
    "probe_cpp_toolchain",
    "producer_metadata",
    "profile_extension_metadata",
    "source_metadata",
    "target_compatibility_error",
    "target_info",
    "validate_target_compatibility",
]
