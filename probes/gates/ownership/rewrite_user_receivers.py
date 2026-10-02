#!/usr/bin/env python3
"""Migrate Cacao/Zyn instance methods onto explicit receivers + &T handle params."""
from __future__ import annotations

import re
import sys
from pathlib import Path

HANDLE_TYPES = [
    "DescriptorSetLayout",
    "DescriptorPool",
    "DescriptorSet",
    "GraphicsPipeline",
    "ComputePipeline",
    "RayTracingPipeline",
    "AccelerationStructure",
    "ShaderBindingTable",
    "PipelineLayout",
    "ShaderCompiler",
    "ShaderModule",
    "CommandEncoder",
    "TimelineSemaphore",
    "Synchronization",
    "TextureView",
    "DeviceQueue",
    "QueryPool",
    "Swapchain",
    "Instance",
    "Adapter",
    "Surface",
    "Sampler",
    "Texture",
    "Buffer",
    "Device",
]

HANDLE_SET = set(HANDLE_TYPES)

CMD_ENCODER_SHARED = {"clone", "isValid"}
HANDLE_MUT_NAMES = {
    "destroy",
    "drop",
    "reset",
    "begin",
    "end",
    "endRendering",
    "map",
    "unmap",
    "write",
    "writeBytes",
    "writeRaw",
    "writeMapped",
    "flush",
    "update",
    "setCacheDirectory",
    "resetFrameFence",
    "writeBuffer",
    "writeBufferElement",
    "writeTexture",
    "writeTextureElement",
    "writeStorageImage",
    "writeStorageImageElement",
    "writeSampler",
    "writeSamplerElement",
    "writeAccelerationStructure",
}

IFACE_MUT_NAMES = {"update", "enter", "leave", "start", "stop"}

FN_START = re.compile(
    r"^(\s*)((?:public|private|protected)\s+)?(static\s+)?fn\s+(\w+)(<[^>]*>)?\s*\((.*)\)(.*)$"
)
IFACE_FN = re.compile(
    r"^(\s*)fn\s+(\w+)(<[^>]*>)?\s*\((.*)\)(.*);$"
)
CLASS_START = re.compile(r"^\s*(public\s+)?class\s+(\w+)\b")
IFACE_START = re.compile(r"^\s*(public\s+)?interface\s+(\w+)\b")
STRUCT_START = re.compile(r"^\s*(public\s+)?struct\s+(\w+)\b")

RECEIVER_PREFIXES = (
    "&mut self",
    "&self",
    "mut self",
    "own self",
)


def first_brace_delta(s: str) -> int:
    return s.count("{") - s.count("}")


def has_receiver(args: str) -> bool:
    a = args.strip()
    if not a:
        return False
    if a == "self" or a.startswith("self,") or a.startswith("self:"):
        return True
    return any(a == p or a.startswith(p + ",") or a.startswith(p + ":") for p in RECEIVER_PREFIXES)


def insert_receiver(args: str, recv: str) -> str:
    a = args.strip()
    if not a:
        return recv
    return recv + ", " + a


def convert_handle_params(args: str) -> tuple[str, list[str]]:
    names: list[str] = []
    out = args
    for t in HANDLE_TYPES:
        def repl(m: re.Match[str], ty: str = t) -> str:
            names.append(m.group(1))
            return f"{m.group(1)}: &{ty}"

        out, n = re.subn(rf"\b(\w+): {t}\b", repl, out)
        if n and False:
            pass
    return out, names


def clone_value_uses(body: str, names: list[str]) -> str:
    for name in names:
        body = re.sub(rf"\.push\({re.escape(name)}\)", f".push({name}.clone())", body)
        body = re.sub(rf": {re.escape(name)},", f": {name}.clone(),", body)
        body = re.sub(rf": {re.escape(name)} \}}", f": {name}.clone() }}", body)
        body = re.sub(rf": {re.escape(name)}\n", f": {name}.clone()\n", body)
        body = re.sub(rf"= {re.escape(name)};", f"= {name}.clone();", body)
        body = re.sub(rf"= {re.escape(name)},", f"= {name}.clone(),", body)
    return body


def classify_cacao(class_name: str, name: str, body: str, is_handle_class: bool) -> str:
    if name in ("destroy", "drop"):
        return "&mut self"
    if "return self;" in body:
        return "mut self"
    if class_name == "CommandEncoder" and name not in CMD_ENCODER_SHARED:
        return "&mut self"
    if name in HANDLE_MUT_NAMES:
        return "&mut self"
    if is_handle_class:
        return "&self"
    if re.search(r"\bself\.\w+\s*=", body) or ".push(" in body or ".set(" in body:
        return "&mut self"
    return "&self"


def classify_zyn(name: str, body: str) -> str:
    if name in ("destroy", "drop"):
        return "&mut self"
    if "return self;" in body:
        return "mut self"
    if re.search(r"\bself\.\w+\s*=", body):
        return "&mut self"
    if ".push(" in body or ".pop(" in body or ".set(" in body or ".clear(" in body:
        if "self." in body:
            return "&mut self"
    return "&self"


def rewrite_fn_line(line: str, recv: str, convert_handles: bool) -> tuple[str, list[str]]:
    m = FN_START.match(line.rstrip("\r\n"))
    if not m:
        return line, []
    indent, pub, static, name, gens, args, rest = m.groups()
    pub = pub or ""
    static = static or ""
    gens = gens or ""
    names: list[str] = []
    if convert_handles:
        args, names = convert_handle_params(args)
    if not static and not has_receiver(args):
        args = insert_receiver(args, recv)
    return f"{indent}{pub}{static}fn {name}{gens}({args}){rest}\n", names


def process_source(text: str, cacao: bool) -> str:
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    i = 0
    brace = 0
    class_name = ""
    in_iface = False
    class_brace = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        stripped = line.lstrip()

        if brace == 0 or (class_name == "" and not in_iface):
            cm = CLASS_START.match(line)
            im = IFACE_START.match(line)
            sm = STRUCT_START.match(line)
            if cm:
                class_name = cm.group(2)
                in_iface = False
                class_brace = brace + first_brace_delta(line)
                out.append(line)
                brace += first_brace_delta(line)
                i += 1
                continue
            if im:
                class_name = im.group(2)
                in_iface = True
                class_brace = brace + first_brace_delta(line)
                out.append(line)
                brace += first_brace_delta(line)
                i += 1
                continue
            if sm:
                out.append(line)
                brace += first_brace_delta(line)
                i += 1
                continue

        if in_iface and class_name:
            im = IFACE_FN.match(line.rstrip("\r\n"))
            if im:
                indent, name, gens, args, rest = im.groups()
                gens = gens or ""
                if not has_receiver(args):
                    recv = "&mut self" if name in IFACE_MUT_NAMES else "&self"
                    args = insert_receiver(args, recv)
                out.append(f"{indent}fn {name}{gens}({args}){rest};\n")
                i += 1
                continue

        m = FN_START.match(line.rstrip("\r\n"))
        if m and class_name and not in_iface:
            static = m.group(3) or ""
            name = m.group(4)
            combined = line
            j = i + 1
            while "{" not in combined and j < n:
                combined += lines[j]
                j += 1
            if "{" not in combined:
                out.append(line)
                brace += first_brace_delta(line)
                i += 1
                continue
            depth = 0
            k = i
            started = False
            while k < n:
                delta = first_brace_delta(lines[k])
                if "{" in lines[k]:
                    started = True
                depth += delta
                k += 1
                if started and depth <= 0:
                    break
            body_text = "".join(lines[i:k])
            is_handle = cacao and class_name in HANDLE_SET
            if static:
                recv = ""
            elif cacao:
                recv = classify_cacao(class_name, name, body_text, is_handle)
            else:
                recv = classify_zyn(name, body_text)
            new_sig, converted_names = rewrite_fn_line(lines[i], recv, cacao)
            suffix = body_text[len(lines[i]) :]
            if cacao and converted_names:
                suffix = clone_value_uses(suffix, converted_names)
            out.append(new_sig + suffix)
            for chunk in lines[i:k]:
                brace += first_brace_delta(chunk)
            if class_name and brace <= 0:
                class_name = ""
                in_iface = False
            i = k
            continue

        brace += first_brace_delta(line)
        if class_name and brace <= 0:
            class_name = ""
            in_iface = False
        out.append(line)
        i += 1
    return "".join(out)


FLUENT_METHODS = (
    "addBinding",
    "addSetLayout",
    "addPushConstants",
    "addShader",
    "addVertexBinding",
    "addVertexAttribute",
    "addColorFormat",
    "addColorAttachmentDefault",
    "setCullMode",
    "setStencilReplace",
    "setStencilEqual",
    "setDepthStencilFormat",
    "setTopology",
    "setLineWidth",
    "setDepthTest",
    "setDepthCompare",
)
FLUENT_RE = re.compile(
    r"^(\s*)((?:self\.)?[A-Za-z_][A-Za-z0-9_]*)\.("
    + "|".join(FLUENT_METHODS)
    + r")\("
)


def reassign_fluent(text: str) -> str:
    out = []
    for line in text.splitlines(keepends=True):
        raw = line.rstrip("\n")
        nl = "\n" if line.endswith("\n") else ""
        m = FLUENT_RE.match(raw)
        if m and "=" not in raw.split(m.group(2) + ".")[0]:
            ident = m.group(2)
            if f"{ident} = {ident}." not in raw:
                raw = f"{m.group(1)}{ident} = {ident}.{m.group(3)}(" + raw[m.end() :]
        out.append(raw + nl)
    return "".join(out)


def replace_cacao_borrow_args(text: str) -> str:
    pairs = [
        (r"DeviceDesc\.forSurface\(([^()]+)\.clone\(\)\)", r"DeviceDesc.forSurface(&\1)"),
        (r"\.createSwapchain\(([^,]+)\.clone\(\)", r".createSwapchain(&\1"),
        (r"\.capabilities\(([^()]+)\.clone\(\)\)", r".capabilities(&\1)"),
        (r"\.compileOrLoad\(([^,]+)\.clone\(\)", r".compileOrLoad(&\1"),
        (r"\.submit\(([^,]+)\.clone\(\), ([^,]+)\.clone\(\)", r".submit(&\1, &\2"),
        (r"\.submitSimple\(([^()]+)\.clone\(\)", r".submitSimple(&\1"),
        (r"\.transitionImage\(([^,]+)\.clone\(\)", r".transitionImage(&\1"),
        (r"\.copyBufferToImage\(([^,]+)\.clone\(\), ([^,]+)\.clone\(\)", r".copyBufferToImage(&\1, &\2"),
        (r"\.allocate\(([^()]+)\.clone\(\)", r".allocate(&\1"),
        (r"\.writeTexture\(([^,]+), ([^,]+)\.clone\(\), ([^,]+)\.clone\(\)", r".writeTexture(\1, &\2, &\3"),
        (r"\.writeSampler\(([^,]+), ([^()]+)\.clone\(\)", r".writeSampler(\1, &\2"),
        (r"\.addSetLayout\(([^()]+)\.clone\(\)", r".addSetLayout(&\1"),
        (r"\.addShader\(([^()]+)\.clone\(\)", r".addShader(&\1"),
        (r"GraphicsPipelineDesc\.create\(([^()]+)\.clone\(\)", r"GraphicsPipelineDesc.create(&\1"),
        (r"\.bindDescriptorSets\(([^,]+)\.clone\(\)", r".bindDescriptorSets(&\1"),
        (r"\.pushConstants\(([^,]+)\.clone\(\)", r".pushConstants(&\1"),
        (r"\.bindGraphicsPipeline\(([^()]+)\.clone\(\)", r".bindGraphicsPipeline(&\1"),
    ]
    for pat, repl in pairs:
        text = re.sub(pat, repl, text)
    return text


def patch_cacao_internal_calls(text: str) -> str:
    text = text.replace(
        "RenderingAttachmentInfo.color(empty_tex,",
        "RenderingAttachmentInfo.color(&empty_tex,",
    )
    return text


def main() -> int:
    root = Path(__file__).resolve().parents[3]
    cacao = root / "bootstrap_compiler" / "std_packages" / "cacao" / "src" / "cacao.vyx"
    src = cacao.read_text(encoding="utf-8")
    dst = process_source(src, cacao=True)
    dst = patch_cacao_internal_calls(dst)
    cacao.write_text(dst, encoding="utf-8", newline="\n")
    print(f"cacao: {len(src)} -> {len(dst)} bytes")

    zyn_root = root / "Zyn" / "src"
    for path in sorted(zyn_root.rglob("*.vyx")):
        old = path.read_text(encoding="utf-8")
        new = process_source(old, cacao=False)
        if path.name == "Renderer.vyx":
            new = replace_cacao_borrow_args(new)
            new = reassign_fluent(new)
        if new != old:
            path.write_text(new, encoding="utf-8", newline="\n")
            print(f"zyn: {path.relative_to(root)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
