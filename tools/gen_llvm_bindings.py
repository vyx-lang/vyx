#!/usr/bin/env python3
"""Generate Vyx extern "C" bindings from LLVM C API headers.

Usage:
    python gen_llvm_bindings.py <llvm-include-dir> [output.vyx]

Example:
    python gen_llvm_bindings.py C:/llvm/include std/llvm.vyx
    python gen_llvm_bindings.py /usr/include/llvm-c std/llvm.vyx

This script parses LLVM's C API header files (llvm-c/*.h) and generates
Vyx `extern "C" { }` declarations for all public functions.
"""

import re
import sys
import os
from pathlib import Path
from typing import Dict, List, Tuple

# C type → Vyx type mapping
TYPE_MAP: Dict[str, str] = {
    "void": "void",
    "int": "i32",
    "unsigned": "i32",
    "unsigned int": "i32",
    "long long": "i64",
    "unsigned long long": "i64",
    "long": "i64",
    "size_t": "i64",
    "uint64_t": "i64",
    "int64_t": "i64",
    "uint32_t": "i32",
    "int32_t": "i32",
    "uint8_t": "i32",
    "double": "f64",
    "float": "f32",
    "char": "i32",
    "const char *": "rawptr",
    "const char*": "rawptr",
    "char *": "rawptr",
    "char*": "rawptr",
    "LLVMBool": "i32",
}

# All LLVM opaque handle types → rawptr
LLVM_HANDLE_TYPES = {
    "LLVMModuleRef", "LLVMTypeRef", "LLVMValueRef", "LLVMBasicBlockRef",
    "LLVMBuilderRef", "LLVMContextRef", "LLVMMemoryBufferRef",
    "LLVMPassManagerRef", "LLVMPassManagerBuilderRef",
    "LLVMTargetRef", "LLVMTargetMachineRef", "LLVMTargetDataRef",
    "LLVMExecutionEngineRef", "LLVMGenericValueRef",
    "LLVMAttributeRef", "LLVMDiagnosticInfoRef", "LLVMMetadataRef",
    "LLVMNamedMDNodeRef", "LLVMUseRef", "LLVMObjectFileRef",
    "LLVMSectionIteratorRef", "LLVMSymbolIteratorRef",
    "LLVMRelocationIteratorRef", "LLVMBinaryRef",
    "LLVMComdatRef", "LLVMModuleFlagEntry",
    "LLVMJITEventListenerRef", "LLVMPassBuilderOptionsRef",
    "LLVMErrorRef", "LLVMOrcThreadSafeContextRef",
    "LLVMOrcThreadSafeModuleRef", "LLVMOrcJITDylibRef",
    "LLVMOrcLLJITRef", "LLVMOrcLLJITBuilderRef",
    "LLVMDIBuilderRef",
}

# LLVM enum types → i32
LLVM_ENUM_TYPES = {
    "LLVMOpcode", "LLVMTypeKind", "LLVMLinkage", "LLVMVisibility",
    "LLVMDLLStorageClass", "LLVMCallConv", "LLVMValueKind",
    "LLVMIntPredicate", "LLVMRealPredicate", "LLVMLandingPadClauseTy",
    "LLVMThreadLocalMode", "LLVMAtomicOrdering", "LLVMAtomicRMWBinOp",
    "LLVMDiagnosticSeverity", "LLVMInlineAsmDialect",
    "LLVMModuleFlagBehavior", "LLVMAttributeIndex",
    "LLVMVerifierFailureAction", "LLVMCodeGenOptLevel",
    "LLVMRelocMode", "LLVMCodeModel", "LLVMCodeGenFileType",
    "LLVMDWARFSourceLanguage", "LLVMDWARFEmissionKind",
    "LLVMDWARFMacinfoRecordType",
}


def map_c_type(c_type: str, is_param: bool = False) -> str:
    """Map a C type to a Vyx type.

    is_param=True demotes bare `void` to `rawptr` because Vyx doesn't
    accept `void` as a parameter type (LLVM verifier rejects it).
    Typically reached when the C declaration was `f(void *x)` but the
    regex split misplaced the `*` onto the name side.
    """
    c_type = c_type.strip()
    c_type = re.sub(r'\s+', ' ', c_type)

    if c_type in TYPE_MAP:
        mapped = TYPE_MAP[c_type]
        return "rawptr" if (is_param and mapped == "void") else mapped

    if c_type.rstrip(" *") in LLVM_HANDLE_TYPES:
        return "rawptr"
    if c_type.replace("*", "").strip() in LLVM_HANDLE_TYPES:
        return "rawptr"

    if c_type in LLVM_ENUM_TYPES:
        return "i32"

    if "*" in c_type:
        return "rawptr"

    if c_type.startswith("LLVM"):
        return "rawptr"

    if c_type.startswith("enum "):
        return "i32"

    return "rawptr"


def parse_function(line: str) -> Tuple[str, str, List[Tuple[str, str]]] | None:
    """Parse a C function declaration into (name, return_type, [(param_name, param_type)])."""
    # Match patterns like: LLVMTypeRef LLVMIntType(unsigned NumBits);
    # or: void LLVMDisposeModule(LLVMModuleRef M);
    pattern = r'^\s*(?:LLVM_C_EXTERN_C_BEGIN\s+)?(?:LLVM_FUNC_ABI\s+)?(\w[\w\s\*]+?)\s+(LLVM\w+)\s*\(([^)]*)\)\s*;'
    m = re.match(pattern, line)
    if not m:
        return None

    ret_type = m.group(1).strip()
    func_name = m.group(2).strip()
    params_str = m.group(3).strip()

    if params_str == "void" or params_str == "":
        params = []
    else:
        params = []
        for p in params_str.split(","):
            p = p.strip()
            if not p:
                continue
            # Handle "..." (varargs)
            if p == "...":
                continue
            # Split into type and name: "LLVMTypeRef Ty" → ("LLVMTypeRef", "Ty")
            parts = p.rsplit(None, 1)
            if len(parts) == 2:
                ptype, pname = parts
                # If the C source wrote `void *P`, rsplit gave us
                # ptype="void" and pname="*P". Reattach the `*` to the
                # type BEFORE stripping it off the name — without this,
                # `map_c_type("void")` downstream emits the illegal
                # `void` param type.
                if pname.startswith("*"):
                    ptype = ptype + " *"
                pname = pname.lstrip("*")
                params.append((pname if pname else f"p{len(params)}", ptype))
            elif len(parts) == 1:
                params.append((f"p{len(params)}", parts[0]))

    return func_name, ret_type, params


def generate_bindings(include_dir: str) -> str:
    """Generate Vyx bindings from LLVM C API headers."""
    headers = [
        "Core.h", "Types.h", "Target.h", "TargetMachine.h",
        "Analysis.h", "BitWriter.h", "BitReader.h",
        "ExecutionEngine.h", "Transforms/PassBuilder.h",
        "DebugInfo.h", "Error.h",
    ]

    functions: List[Tuple[str, str, List[Tuple[str, str]]]] = []
    seen_names = set()

    for header in headers:
        header_path = Path(include_dir) / "llvm-c" / header
        if not header_path.exists():
            header_path = Path(include_dir) / header
        if not header_path.exists():
            print(f"  [skip] {header} not found", file=sys.stderr)
            continue

        print(f"  [parse] {header}", file=sys.stderr)
        try:
            content = header_path.read_text(encoding='utf-8', errors='replace')
        except Exception as e:
            print(f"  [error] {header}: {e}", file=sys.stderr)
            continue

        # Join multi-line declarations
        content = re.sub(r'\\\n', ' ', content)

        for line in content.split('\n'):
            line = line.strip()
            if not line or line.startswith("//") or line.startswith("#"):
                continue

            result = parse_function(line)
            if result and result[0] not in seen_names:
                functions.append(result)
                seen_names.add(result[0])

    # Generate Vyx output
    out = []
    out.append("/// std/llvm.vyx — LLVM C API Bindings (auto-generated)")
    out.append(f"/// Generated from: {include_dir}")
    out.append(f"/// Functions: {len(functions)}")
    out.append("")
    out.append('extern "C" {')

    for func_name, ret_type, params in sorted(functions, key=lambda x: x[0]):
        vyx_ret = map_c_type(ret_type)
        vyx_params = []
        param_names_seen = set()
        for pname, ptype in params:
            vyx_ptype = map_c_type(ptype, is_param=True)
            # Deduplicate parameter names
            if pname in param_names_seen or pname in ("type", "fn", "let", "var", "if", "for", "in", "is"):
                pname = pname + "_"
            param_names_seen.add(pname)
            vyx_params.append(f"{pname}: {vyx_ptype}")

        params_str = ", ".join(vyx_params)
        if vyx_ret == "void":
            out.append(f"    fn {func_name}({params_str});")
        else:
            out.append(f"    fn {func_name}({params_str}) -> {vyx_ret};")

    out.append("}")
    out.append("")

    return "\n".join(out)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    include_dir = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None

    if not os.path.isdir(include_dir):
        print(f"Error: '{include_dir}' is not a directory", file=sys.stderr)
        sys.exit(1)

    print(f"Generating LLVM C API bindings from {include_dir}...", file=sys.stderr)
    bindings = generate_bindings(include_dir)

    if output_file:
        Path(output_file).write_text(bindings, encoding='utf-8')
        print(f"Wrote {output_file} ({bindings.count(chr(10))} lines)", file=sys.stderr)
    else:
        print(bindings)


if __name__ == "__main__":
    main()
