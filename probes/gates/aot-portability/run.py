#!/usr/bin/env python3
"""Check baseline AOT ISA selection and an optional real SDK project build."""
import argparse
import ctypes as ct
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess


def check_backend(backend, output):
    directory = os.add_dll_directory(str(backend.parent)) if os.name == "nt" else None
    try:
        api = ct.CDLL(str(backend))
        ptr, text, size, integer = ct.c_void_p, ct.c_char_p, ct.c_uint64, ct.c_int32

        def bind(name, result, *arguments):
            function = getattr(api, "vyx_rt_" + name)
            function.restype, function.argtypes = result, list(arguments)
            return function

        init = bind("init", integer)
        module_new = bind("module_new", ptr, text, size, text, size)
        free = bind("module_free", None, ptr)
        error = bind("get_last_error", size, ptr, ptr, size)
        default_triple = bind("default_target_triple", size, ptr, size)
        int_ty = bind("int_type", ptr, ptr, integer)
        ptr_ty = bind("ptr_ty", ptr, ptr)
        void_ty = bind("void_ty", ptr, ptr)
        fn_ty = bind("fn_type", ptr, ptr, ptr, ct.POINTER(ptr), size, integer)
        add_fn = bind("add_function", ptr, ptr, text, size, ptr)
        param = bind("get_param", ptr, ptr, ptr, size)
        builder = bind("builder_new", integer, ptr)
        block = bind("append_block", ptr, ptr, ptr, text, size)
        position = bind("position_at_end", None, ptr, ptr)
        const = bind("const_int", ptr, ptr, ptr, ct.c_int64, integer)
        gep = bind("build_gep1", ptr, ptr, ptr, ptr, ptr, text, size)
        store = bind("build_store_aligned", ptr, ptr, ptr, ptr, integer)
        ret = bind("build_ret_void", None, ptr)
        verify = bind("verify", integer, ptr)
        cpu = bind("set_target_cpu", integer, ptr, text, size)
        emit = bind("emit_codegen", integer, ptr, text, size, integer, integer)

        def require(code, module):
            if code:
                message = ct.create_string_buffer(4096)
                error(module, message, len(message))
                raise RuntimeError(message.value.decode(errors="replace"))

        require(init(), None)
        host = ct.create_string_buffer(512)
        length = default_triple(host, len(host))
        if not 0 < length < len(host):
            raise RuntimeError("Invalid host triple")
        triples = list(dict.fromkeys([host.value, b"x86_64-unknown-linux-gnu",
                                     b"aarch64-unknown-linux-gnu", b"nvptx64-nvidia-cuda"]))
        for triple in triples:
            module = module_new(b"isa_probe", 9, triple, len(triple))
            if not module:
                raise RuntimeError(f"Cannot create {triple!r} module")
            try:
                if triple.startswith(b"nvptx"):
                    require(cpu(module, b"sm_80", 5), module)
                require(builder(module), module)
                i64 = int_ty(module, 64)
                signature = fn_ty(module, void_ty(module), (ptr * 1)(ptr_ty(module)), 1, 0)
                function = add_fn(module, b"clear64", 7, signature)
                position(module, block(module, function, b"entry", 5))
                destination, zero = param(module, function, 0), const(module, i64, 0, 0)
                # Mirrors StringBuilder's inline buffer initialization. A host
                # CPU target merges these stores into AVX/AVX-512 instructions.
                for index in range(8):
                    address = gep(module, i64, destination, const(module, i64, index, 0), b"", 0)
                    store(module, zero, address, 8)
                ret(module)
                require(verify(module), module)
                path = output / (triple.decode() + ".s")
                encoded = os.fsencode(path)
                require(emit(module, encoded, len(encoded), 2, 1), module)
                assembly = path.read_text()
                if triple.startswith(b"x86_64"):
                    if re.search(r"\b(?:ymm|zmm)\d+\b|\b(?:v[a-z0-9]+)\s", assembly):
                        raise AssertionError(f"{path}: optional AVX ISA leaked into baseline x86-64")
                    if "clear64" not in assembly or "mov" not in assembly:
                        raise AssertionError(f"{path}: zero-store probe was not emitted")
                if triple.startswith(b"nvptx") and ".target sm_80" not in assembly:
                    raise AssertionError("Explicit NVPTX CPU selection was lost")
                print(f"PASS baseline emission: {triple.decode()}", flush=True)
            finally:
                free(module)
    finally:
        if directory is not None:
            directory.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", type=Path, required=True)
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    backend, output = args.backend.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(f"backend={backend} sha256={hashlib.sha256(backend.read_bytes()).hexdigest()}", flush=True)
    check_backend(backend, output)
    if args.compiler:
        compiler = args.compiler.resolve()
        print(f"compiler={compiler} sha256={hashlib.sha256(compiler.read_bytes()).hexdigest()}", flush=True)
        project = output / "project"
        shutil.copytree(Path(__file__).parent / "project", project)
        result = subprocess.run([str(compiler), "build", "--run=aot", "-j1"], cwd=project,
                                text=True, capture_output=True, timeout=120, check=False)
        print(result.stdout, end="", flush=True)
        if result.stderr:
            print(result.stderr, end="", flush=True)
        result.check_returncode()
        if not re.search(r"(?m)^4429\r?$", result.stdout):
            raise AssertionError(f"StringBuilder project did not print 4429: {result.stderr}")
        print("PASS SDK project build and StringBuilder execution", flush=True)


if __name__ == "__main__":
    main()
