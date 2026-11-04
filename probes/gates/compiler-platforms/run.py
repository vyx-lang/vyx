"""Check target-selected globals/consts against independent target expectations."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

TARGETS = {
    "x86_64-pc-windows-msvc": (11, 101),
    "x86_64-unknown-linux-gnu": (22, 102),
    "aarch64-apple-macosx15.0": (33, 103),
    "aarch64-unknown-linux-android21": (44, 104),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name("selection.vyx").resolve()
    record = {"compiler": str(compiler), "compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "results": []}
    for target, values in TARGETS.items():
        for optimization in (0, 2):
            name = target + f"-O{optimization}"
            output = (args.output / (name + ".ll")).resolve()
            output.unlink(missing_ok=True)
            command = [str(compiler), "--src=file", str(source), "--emit=ir", "--export-root-names",
                       "selected_global,selected_constant", "--triplet", target, f"-O{optimization}", "-o", str(output)]
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            (args.output / (name + ".log")).write_bytes(result.stdout)
            ir = output.read_text() if output.is_file() else ""
            # Separate native loads/constants must contain the selected initializer/return.
            # The deliberately distinct values exclude retained inactive declarations.
            global_value, constant_value = values
            selected = bool(re.search(r"(?:global|constant) i32 " + str(global_value) + r"\b", ir))
            folded = bool(re.search(r"ret i32 " + str(global_value) + r"\b", ir))
            constant = bool(re.search(r"constant i32 " + str(constant_value) + r"\b", ir)
                            or re.search(r"ret i32 " + str(constant_value) + r"\b", ir))
            passed = result.returncode == 0 and (selected or folded) and constant
            record["results"].append({"target": target, "opt": optimization, "command": command,
                                      "exit": result.returncode, "passed": passed})
            print(name, "PASS" if passed else "FAIL", flush=True)
    (args.output / "verification.json").write_text(json.dumps(record, indent=2) + "\n")
    raise SystemExit(not all(row["passed"] for row in record["results"]))


if __name__ == "__main__":
    main()
