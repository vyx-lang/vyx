#!/usr/bin/env python3
"""
Vyx C Binding Generator
Scans C header files and generates Vyx extern "C" bindings.

Usage:
    python gen_c_bindings.py <header.h> [-o output.vyx] [--prefix filter] [--exclude pattern]
    python gen_c_bindings.py <directory/> [-o output.vyx] [--recursive]

Examples:
    python gen_c_bindings.py /usr/include/math.h -o std/math_c.vyx
    python gen_c_bindings.py mylib/include/ -o bindings/mylib.vyx --recursive
    python gen_c_bindings.py SDL2/SDL.h -o bindings/sdl2.vyx --prefix SDL_
"""

import re
import sys
import os
import argparse
from pathlib import Path
from typing import List, Tuple, Optional

C_TO_VYX_TYPES = {
    "void": "void",
    "int": "i32",
    "unsigned int": "u32",
    "unsigned": "u32",
    "long": "i64",
    "unsigned long": "u64",
    "long long": "i64",
    "unsigned long long": "u64",
    "short": "i16",
    "unsigned short": "u16",
    "char": "i8",
    "unsigned char": "u8",
    "signed char": "i8",
    "float": "f32",
    "double": "f64",
    "size_t": "u64",
    "ssize_t": "i64",
    "int8_t": "i8",
    "int16_t": "i16",
    "int32_t": "i32",
    "int64_t": "i64",
    "uint8_t": "u8",
    "uint16_t": "u16",
    "uint32_t": "u32",
    "uint64_t": "u64",
    "bool": "bool",
    "_Bool": "bool",
    "BOOL": "i32",
    "DWORD": "u32",
    "WORD": "u16",
    "BYTE": "u8",
    "HANDLE": "rawptr",
    "HWND": "rawptr",
    "HINSTANCE": "rawptr",
    "LPVOID": "rawptr",
    "LPCSTR": "rawptr",
    "LPSTR": "rawptr",
    "LPCWSTR": "rawptr",
    "LPWSTR": "rawptr",
    "HRESULT": "i32",
    "FILE": "rawptr",
    "SOCKET": "u64",
}

def map_c_type(c_type: str, is_param: bool = False) -> str:
    """Map a C type string to a Vyx type.

    is_param: when True, a bare `void` is treated as `rawptr` because `void`
    is not a first-class parameter type in Vyx (nor in C; `f(void)` is an
    argumentless-call marker that the parser strips earlier, not a real
    `void` parameter). Missing this guard produced `param: void`
    declarations that LLVM's verifier rejects with:
        "Function arguments must have first-class types!"
    """
    c_type = c_type.strip()
    c_type = re.sub(r'\b(const|volatile|restrict|__restrict)\b', '', c_type).strip()
    c_type = re.sub(r'\s+', ' ', c_type).strip()

    if '*' in c_type or c_type.endswith('[]') or c_type.endswith('*'):
        return "rawptr"

    if c_type.startswith("const "):
        inner = c_type[6:].strip()
        if inner in C_TO_VYX_TYPES:
            mapped = C_TO_VYX_TYPES[inner]
            return "rawptr" if (is_param and mapped == "void") else mapped

    if c_type in C_TO_VYX_TYPES:
        mapped = C_TO_VYX_TYPES[c_type]
        return "rawptr" if (is_param and mapped == "void") else mapped

    if c_type.startswith("struct ") or c_type.startswith("enum ") or c_type.startswith("union "):
        return "rawptr"

    for base, vyx in C_TO_VYX_TYPES.items():
        if c_type.endswith(base):
            return "rawptr" if (is_param and vyx == "void") else vyx

    return "rawptr"


C_KEYWORDS = {
    "if", "else", "for", "while", "do", "switch", "case", "break", "continue",
    "return", "goto", "sizeof", "typedef", "struct", "union", "enum",
    "default", "register", "auto", "extern", "static", "volatile", "const",
}

VYX_KEYWORDS = {
    "fn", "let", "var", "if", "else", "elif", "for", "while", "match", "case",
    "return", "break", "continue", "struct", "class", "import", "export",
    "true", "false", "null", "self", "pub", "public", "private",
    # Also reserve Vyx soft-keywords that cause ambiguity inside function
    # bodies (param named `in` would collide with `for x in coll` use sites).
    "in", "use", "module", "interface", "error", "type", "defer", "do",
    "async", "await", "static", "extern", "where", "as", "mut", "unsafe",
}


def sanitize_param_name(name: str, index: int) -> str:
    if not name or not name[0].isalpha() and name[0] != '_':
        return f"arg{index}"
    if name in C_KEYWORDS or name in VYX_KEYWORDS:
        return f"{name}_"
    if name in ("long", "short", "int", "char", "float", "double", "unsigned", "signed"):
        return f"arg{index}"
    return name


def parse_function_decl(line: str) -> Optional[Tuple[str, str, List[Tuple[str, str]]]]:
    line = re.sub(r'__attribute__\s*\(\(.*?\)\)', '', line)
    line = re.sub(r'__declspec\s*\(.*?\)', '', line)
    line = re.sub(r'__cdecl|__stdcall|__fastcall|WINAPI|APIENTRY|CALLBACK', '', line)
    line = re.sub(r'\b(extern|static|inline|__inline|__forceinline|_Check_return_)\b', '', line)
    line = re.sub(r'_In_|_Out_|_Inout_|_In_opt_|_Out_opt_|_Ret_maybenull_', '', line)
    line = re.sub(r'_CRT_STDIO_INLINE|__CRTDECL', '', line)
    line = line.strip()

    match = re.match(
        r'^(.+?)\s+(\*?\s*\w+)\s*\((.*?)\)\s*;',
        line, re.DOTALL
    )
    if not match:
        return None

    ret_type = match.group(1).strip()
    func_name = match.group(2).strip().lstrip('*').strip()
    params_str = match.group(3).strip()

    if func_name in C_KEYWORDS:
        return None
    if not re.match(r'^[a-zA-Z_]\w*$', func_name):
        return None

    params = []
    if params_str and params_str != "void":
        param_parts = split_params(params_str)
        for i, p in enumerate(param_parts):
            p = p.strip()
            if p == "..." or p == "":
                continue

            p = re.sub(r'\b(const|volatile|restrict|__restrict|register)\b', '', p).strip()
            p = re.sub(r'_In_|_Out_|_Inout_|_In_opt_|_Out_opt_', '', p).strip()
            p = re.sub(r'\s+', ' ', p)

            arr_match = re.match(r'^(.*?)\s+(\w+)\s*\[.*?\]$', p)
            if arr_match:
                params.append((sanitize_param_name(arr_match.group(2), i), "rawptr"))
                continue

            fptr_match = re.match(r'^.*?\(\s*\*\s*(\w+)\s*\)\s*\(.*?\)$', p)
            if fptr_match:
                params.append((sanitize_param_name(fptr_match.group(1), i), "rawptr"))
                continue

            # Detect whether the name side of the param carries a leading `*`
            # (`void *userdata` has `*` attached to `userdata`, not to `void`).
            # If so, the type was originally a pointer even though the regex
            # stripped the `*` into the name group; reattach it before mapping
            # so `void *` resolves to `rawptr` rather than bare `void`.
            name_match = re.match(r'^(.*?)\s+(\*?\s*\w+)$', p)
            if name_match:
                ptype = name_match.group(1).strip()
                raw_name = name_match.group(2).strip()
                if raw_name.startswith('*'):
                    ptype = ptype + "*"
                pname = raw_name.lstrip('*').strip()
                if not pname or pname in ("long", "short", "int", "char", "unsigned", "signed"):
                    full_type = p
                    pname = f"arg{i}"
                    ptype = full_type
                else:
                    pname = sanitize_param_name(pname, i)
            else:
                ptr_match = re.match(r'^(.*?\*)\s*(\w+)$', p)
                if ptr_match:
                    ptype = ptr_match.group(1)
                    pname = sanitize_param_name(ptr_match.group(2), i)
                else:
                    ptype = p
                    pname = f"arg{i}"

            params.append((pname, map_c_type(ptype, is_param=True)))

    return (func_name, map_c_type(ret_type), params)


def split_params(s: str) -> List[str]:
    parts = []
    depth = 0
    current = ""
    for c in s:
        if c == '(':
            depth += 1
            current += c
        elif c == ')':
            depth -= 1
            current += c
        elif c == ',' and depth == 0:
            parts.append(current)
            current = ""
        else:
            current += c
    if current.strip():
        parts.append(current)
    return parts


def parse_struct_decl(text: str) -> Optional[Tuple[str, List[Tuple[str, str]]]]:
    match = re.match(
        r'typedef\s+struct\s*\w*\s*\{(.*?)\}\s*(\w+)\s*;',
        text, re.DOTALL
    )
    if not match:
        return None

    body = match.group(1)
    name = match.group(2)
    fields = []

    for line in body.split(';'):
        line = line.strip()
        if not line:
            continue
        line = re.sub(r'\b(const|volatile)\b', '', line).strip()
        parts = re.match(r'^(.*?)\s+(\w+)$', line)
        if parts:
            ftype = parts.group(1)
            fname = parts.group(2)
            fields.append((fname, map_c_type(ftype)))

    return (name, fields)


def parse_define_const(line: str) -> Optional[Tuple[str, str]]:
    match = re.match(r'#define\s+(\w+)\s+(\d+|0x[0-9a-fA-F]+)', line)
    if match:
        return (match.group(1), match.group(2))
    return None


def scan_header(filepath: str, prefix_filter: str = "", exclude_pattern: str = "") -> dict:
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)
    content = re.sub(r'//[^\n]*', '', content)
    content = re.sub(r'#\s*if\s+0\b.*?#\s*endif', '', content, flags=re.DOTALL)

    result = {
        "functions": [],
        "structs": [],
        "constants": [],
        "source": filepath,
    }

    lines = content.split('\n')
    joined = ""
    for line in lines:
        line = line.strip()
        if line.startswith('#define'):
            c = parse_define_const(line)
            if c:
                name, val = c
                if prefix_filter and not name.startswith(prefix_filter):
                    continue
                if exclude_pattern and re.search(exclude_pattern, name):
                    continue
                result["constants"].append(c)
            continue

        if line.startswith('#'):
            continue

        joined += " " + line

        if ';' in joined:
            decl = joined.strip()
            joined = ""

            if exclude_pattern and re.search(exclude_pattern, decl):
                continue

            fn = parse_function_decl(decl)
            if fn:
                name = fn[0]
                if prefix_filter and not name.startswith(prefix_filter):
                    continue
                result["functions"].append(fn)

    struct_matches = re.finditer(
        r'typedef\s+struct\s*\w*\s*\{.*?\}\s*\w+\s*;',
        content, re.DOTALL
    )
    for m in struct_matches:
        s = parse_struct_decl(m.group(0))
        if s:
            name = s[0]
            if prefix_filter and not name.startswith(prefix_filter):
                continue
            if exclude_pattern and re.search(exclude_pattern, name):
                continue
            result["structs"].append(s)

    return result


def generate_vyx(result: dict, module_name: str = "") -> str:
    lines = []
    lines.append(f"/// Auto-generated Vyx bindings for {result['source']}")
    lines.append(f"/// Generated by gen_c_bindings.py")
    lines.append("")

    if result["constants"]:
        lines.append("// Constants")
        for name, val in result["constants"]:
            lines.append(f"let {name}: i32 = {val};")
        lines.append("")

    if result["structs"]:
        lines.append("// Structs")
        for name, fields in result["structs"]:
            field_strs = "; ".join(f"{fn}: {ft}" for fn, ft in fields)
            lines.append(f"struct {name} {{ {field_strs}; }}")
        lines.append("")

    if result["functions"]:
        lines.append('extern "C" {')
        for func_name, ret_type, params in result["functions"]:
            param_strs = ", ".join(f"{pn}: {pt}" for pn, pt in params)
            if ret_type == "void":
                lines.append(f"    fn {func_name}({param_strs});")
            else:
                lines.append(f"    fn {func_name}({param_strs}) -> {ret_type};")
        lines.append("}")
        lines.append("")

    lines.append(f"// Total: {len(result['functions'])} functions, "
                 f"{len(result['structs'])} structs, "
                 f"{len(result['constants'])} constants")

    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(
        description="Generate Vyx extern bindings from C header files"
    )
    parser.add_argument("input", help="C header file or directory")
    parser.add_argument("-o", "--output", help="Output .vyx file (default: stdout)")
    parser.add_argument("--prefix", default="", help="Only include symbols starting with prefix")
    parser.add_argument("--exclude", default="", help="Regex pattern to exclude symbols")
    parser.add_argument("--recursive", action="store_true", help="Scan directories recursively")
    parser.add_argument("--module", default="", help="Module name for generated file")
    parser.add_argument("--preprocess", action="store_true", help="Run clang -E before scanning")
    parser.add_argument("--clang", default="clang", help="Path to clang for preprocessing")
    parser.add_argument("-I", "--include-dir", action="append", default=[], help="Include directory for preprocessing")

    args = parser.parse_args()
    input_path = Path(args.input)

    all_results = {
        "functions": [],
        "structs": [],
        "constants": [],
        "source": args.input,
    }

    def preprocess_file(filepath):
        import subprocess
        cmd = [args.clang, "-E", "-P", str(filepath)]
        for inc in args.include_dir:
            cmd.extend(["-I", inc])
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            if result.returncode == 0:
                import tempfile
                tmp = tempfile.NamedTemporaryFile(mode='w', suffix='.h', delete=False)
                tmp.write(result.stdout)
                tmp.close()
                return tmp.name
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass
        return str(filepath)

    if input_path.is_dir():
        pattern = "**/*.h" if args.recursive else "*.h"
        headers = list(input_path.glob(pattern))
        if not headers:
            print(f"No .h files found in {input_path}", file=sys.stderr)
            return 1

        for h in sorted(headers):
            print(f"Scanning {h}...", file=sys.stderr)
            scan_path = preprocess_file(h) if args.preprocess else str(h)
            r = scan_header(scan_path, args.prefix, args.exclude)
            all_results["functions"].extend(r["functions"])
            all_results["structs"].extend(r["structs"])
            all_results["constants"].extend(r["constants"])
    elif input_path.is_file():
        print(f"Scanning {input_path}...", file=sys.stderr)
        scan_path = preprocess_file(input_path) if args.preprocess else str(input_path)
        all_results = scan_header(scan_path, args.prefix, args.exclude)
    else:
        print(f"Error: {input_path} not found", file=sys.stderr)
        return 1

    seen_fns = set()
    unique_fns = []
    for fn in all_results["functions"]:
        if fn[0] not in seen_fns:
            seen_fns.add(fn[0])
            unique_fns.append(fn)
    all_results["functions"] = unique_fns

    output = generate_vyx(all_results, args.module)

    if args.output:
        os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
        with open(args.output, 'w') as f:
            f.write(output)
        print(f"Wrote {args.output} ({len(all_results['functions'])} functions, "
              f"{len(all_results['structs'])} structs, "
              f"{len(all_results['constants'])} constants)", file=sys.stderr)
    else:
        print(output)

    return 0


def self_test() -> int:
    """Regression checks: `void *X` must become `rawptr`, `void` as a bare
    parameter type must be demoted to `rawptr`, and `void` as a return type
    must stay `void`. A failure here means a future change will reintroduce
    LLVM's "Function arguments must have first-class types!" error.
    """
    failures = []

    # map_c_type: return-type mapping — void stays void.
    if map_c_type("void") != "void":
        failures.append(f"void (return) should map to 'void', got {map_c_type('void')!r}")
    # map_c_type: parameter-type mapping — bare void demotes to rawptr.
    if map_c_type("void", is_param=True) != "rawptr":
        failures.append(f"void (param) should demote to 'rawptr', got {map_c_type('void', is_param=True)!r}")
    # map_c_type: void* always maps to rawptr, param or not.
    if map_c_type("void*") != "rawptr":
        failures.append(f"void* should map to 'rawptr'")
    if map_c_type("void *") != "rawptr":
        failures.append(f"'void *' should map to 'rawptr'")
    # parse_function_decl: the classic 'void *userdata' param.
    fn = parse_function_decl("int cb(void *userdata);")
    if fn != ("cb", "i32", [("userdata", "rawptr")]):
        failures.append(f"cb(void *userdata): expected rawptr param, got {fn}")
    # Multiple void* params.
    fn = parse_function_decl("void do_many(void *a, void *b, int n);")
    if fn != ("do_many", "void", [("a", "rawptr"), ("b", "rawptr"), ("n", "i32")]):
        failures.append(f"do_many: {fn}")
    # void (as argumentless-marker) is not a real param.
    fn = parse_function_decl("int ticks(void);")
    if fn != ("ticks", "i32", []):
        failures.append(f"ticks(void): expected no params, got {fn}")

    if failures:
        print("SELF-TEST FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print("self-test: all checks passed", file=sys.stderr)
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--self-test":
        sys.exit(self_test())
    sys.exit(main())
