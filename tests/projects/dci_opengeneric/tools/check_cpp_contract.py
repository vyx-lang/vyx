"""cpp 契约的按值聚合降级必须合法。

clang 把 16 字节 POD 的按值参数降成 ``ptr dead_on_return``（调用者提供存储、
被调者不得持有 = 借用指针），契约里对应的 passing 是 ``indirect``。adapter 若
把它机械地翻成 ``coerce -> ptr``，coerce 的载体（8 字节指针）与 storage（16
字节）不等宽 —— 编译器会直接拒绝：

    I0100: DCI coerce size mismatch for parameter #0 of `vyx_vec2_x`:
           storage=16 coerce=8

这个形态正是 ``tests/projects/dci_rust_generic/run.ps1`` 的
``coerce_parameter_width`` 负例钉死的非法契约，所以它必须在契约产出这一步被
抓住，而不是等到构建期报一个看着像编译器 bug 的 I0100。

用法：check_cpp_contract.py <decode 出来的契约 JSON>
"""

from __future__ import annotations

import json
import sys

# 契约里 coerce 载体可能用到的定宽标量（字节数）
PRIMITIVE_BYTES = {
    "i8": 1,
    "u8": 1,
    "i16": 2,
    "u16": 2,
    "i32": 4,
    "u32": 4,
    "f32": 4,
    "i64": 8,
    "u64": 8,
    "f64": 8,
    "ptr": 8,
}

# 本项目契约应当出现的三个闭合符号，以及每个的按值聚合参数个数
EXPECTED_AGGREGATE_PARAMS = {
    "vyx_vec2_x": 1,
    "vyx_vec2_y": 1,
}


def carrier_bytes(coerce_to: object) -> int | None:
    if not isinstance(coerce_to, dict):
        return None
    name = coerce_to.get("name")
    if not isinstance(name, str):
        return None
    if name in PRIMITIVE_BYTES:
        return PRIMITIVE_BYTES[name]
    if name == "ptr":
        return 8
    return None


def main(path: str) -> int:
    with open(path, encoding="utf-8") as handle:
        descriptor = json.load(handle)

    symbols = descriptor.get("exports", {}).get("symbols", [])
    problems: list[str] = []
    by_name: dict[str, dict] = {}
    coerce_seen = 0
    aggregate_seen = 0

    for symbol in symbols:
        name = symbol.get("link_name") or symbol.get("name")
        if isinstance(name, str):
            by_name[name] = symbol
        abi = symbol.get("abi") or {}
        for where, position in (
            ("parameter", abi.get("parameters") or []),
            ("return", [abi.get("return")] if abi.get("return") else []),
        ):
            for entry in position:
                if not isinstance(entry, dict):
                    continue
                passing = entry.get("passing")
                size = entry.get("size")
                if passing == "coerce":
                    coerce_seen += 1
                    want = carrier_bytes(entry.get("coerce_to"))
                    if size is None or want is None or size != want:
                        problems.append(
                            f"{name}: {where} coerce carrier "
                            f"{entry.get('coerce_to')!r} is not the same width as "
                            f"storage (size={size})"
                        )
                elif passing in {"indirect", "byval"} and isinstance(size, int):
                    aggregate_seen += 1

    for name, expected in EXPECTED_AGGREGATE_PARAMS.items():
        symbol = by_name.get(name)
        if symbol is None:
            problems.append(f"missing closed symbol {name}")
            continue
        params = (symbol.get("abi") or {}).get("parameters") or []
        aggregates = [
            param
            for param in params
            if param.get("passing") in {"indirect", "byval"}
        ]
        if len(aggregates) != expected:
            problems.append(
                f"{name}: expected {expected} by-value aggregate parameter(s), "
                f"found {len(aggregates)}"
            )
            continue
        param = aggregates[0]
        if param.get("size") != 16:
            problems.append(
                f"{name}: aggregate parameter size is {param.get('size')}, "
                "expected 16"
            )
        # 裸 coerce 到指针 = 非法（载体 8 字节 ≠ storage 16 字节）
        if param.get("coerce_to") is not None:
            problems.append(
                f"{name}: aggregate parameter carries a coerce carrier "
                f"{param.get('coerce_to')!r}"
            )

    if coerce_seen == 0 and aggregate_seen == 0:
        problems.append(
            "no coerce or indirect/byval parameter was inspected at all -- "
            "the descriptor shape changed and this check is vacuous"
        )

    if problems:
        print("FAIL cpp contract is not a legal lowering")
        for problem in problems:
            print("  - " + problem)
        return 1

    print(
        f"CONTRACT lowering ok (coerce params inspected {coerce_seen}, "
        f"by-value aggregates inspected {aggregate_seen})"
    )
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: check_cpp_contract.py <decoded-contract.json>")
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1]))
