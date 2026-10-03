import re
import pathlib

ROOT = pathlib.Path(r"E:\Dev\C++\VyxLan-selfhost-yolo\Zyn\src\nut")
FILES = [
    "RenderGraph.vyx",
    "RenderPlan.vyx",
    "Atlas.vyx",
    "Text.vyx",
    "Types.vyx",
    "Batch.vyx",
    "Path.vyx",
    "Image.vyx",
    "Canvas.vyx",
    "Scene.vyx",
]

fn_re = re.compile(
    r"^(\s+)((?:public|private|protected)\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\((.*)\)(.*)$"
)
recv_re = re.compile(r"^(?:&(?:mut\s+)?|mut\s+|own\s+)?self(?:\s*:|$)")
class_re = re.compile(r"^\s*(?:public\s+|private\s+)?(?:class|struct)\s+")


def first_param(args: str) -> str:
    args = args.strip()
    if not args:
        return ""
    depth = 0
    for i, ch in enumerate(args):
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth = max(0, depth - 1)
        elif ch == "," and depth == 0:
            return args[:i].strip()
    return args


def transform(text: str) -> tuple[str, int]:
    lines = text.splitlines(keepends=True)
    out = []
    depth = 0
    class_stack: list[int] = []
    extern_stack: list[int] = []
    nchg = 0
    for line in lines:
        opens = line.count("{")
        closes = line.count("}")
        in_extern = bool(extern_stack)
        in_class = bool(class_stack) and not in_extern
        m = fn_re.match(line.rstrip("\n\r"))
        did = False
        if m and in_class and "static" not in line:
            indent, vis, name, args, rest = m.group(1), m.group(2) or "", m.group(3), m.group(4), m.group(5)
            fp = first_param(args)
            if not recv_re.match(fp):
                new_args = "&self" if args.strip() == "" else "&self, " + args
                nl = "\n" if line.endswith("\n") else ""
                out.append(f"{indent}{vis}fn {name}({new_args}){rest}{nl}")
                nchg += 1
                did = True
        if not did:
            out.append(line)
        if "extern \"C\"" in line:
            extern_stack.append(depth)
        if class_re.match(line) and "fn " not in line:
            class_stack.append(depth)
        depth += opens - closes
        while class_stack and depth <= class_stack[-1]:
            class_stack.pop()
        while extern_stack and depth <= extern_stack[-1]:
            extern_stack.pop()
    return "".join(out), nchg


for name in FILES:
    path = ROOT / name
    if not path.exists():
        print("missing", name)
        continue
    new, n = transform(path.read_text(encoding="utf-8"))
    if n:
        path.write_text(new, encoding="utf-8")
    print(f"{n:4d}  {name}")
