"""SemVer ordering and explicit supported requirement syntax."""
from __future__ import annotations

from functools import total_ordering
import re

from common import PackageError


@total_ordering
class Version:
    def __init__(self, value: str):
        if not isinstance(value, str):
            raise PackageError("SemVer version must be a string")
        match = re.fullmatch(
            r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
            r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
            r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?", value)
        if not match:
            raise PackageError(f"invalid SemVer version: {value!r}")
        self.text = value
        self.numbers = tuple(int(match[i]) for i in (1, 2, 3))
        self.pre = tuple((match[4] or "").split(".")) if match[4] else ()
        if any(part.isdigit() and len(part) > 1 and part[0] == "0" for part in self.pre):
            raise PackageError(f"invalid numeric prerelease identifier: {value!r}")

    def __eq__(self, other):
        return isinstance(other, Version) and (self.numbers, self.pre) == (other.numbers, other.pre)

    def __lt__(self, other):
        if self.numbers != other.numbers:
            return self.numbers < other.numbers
        if not self.pre or not other.pre:
            return bool(self.pre) and not other.pre
        for a, b in zip(self.pre, other.pre):
            if a == b:
                continue
            if a.isdigit() and b.isdigit():
                return int(a) < int(b)
            if a.isdigit() != b.isdigit():
                return a.isdigit()
            return a < b
        return len(self.pre) < len(other.pre)


def _partial(value):
    value = value.strip()
    if re.fullmatch(r"\d+(?:\.\d+){0,2}", value):
        pieces = value.split(".")
        return Version(".".join(pieces + ["0"] * (3 - len(pieces)))), len(pieces)
    return Version(value), 3


class Requirement:
    def __init__(self, expression: str):
        if not isinstance(expression, str) or not expression.strip():
            raise PackageError("dependency version requirement must be a nonempty string")
        self.text = expression.strip()
        self.arms = []
        for arm in self.text.split("||"):
            arm = arm.strip()
            if not arm:
                raise PackageError(f"invalid version requirement: {expression!r}")
            tests = []
            tokens = re.findall(r"(?:\^|~|>=|<=|>|<|=)?\s*[^\s,]+", arm)
            if not tokens:
                raise PackageError(f"invalid version requirement: {expression!r}")
            for raw in tokens:
                raw = re.sub(r"\s+", "", raw)
                match = re.fullmatch(r"(\^|~|>=|<=|>|<|=)?(.+)", raw)
                op, val = match.groups()
                op = op or "="
                if val.lower() in ("*", "x"):
                    if op != "=":
                        raise PackageError(f"invalid wildcard comparison {raw!r}")
                    continue
                if "*" in val or re.search(r"(?:^|\.)[xX](?:\.|$)", val):
                    parts = val.split(".")
                    if op != "=" or len(parts) > 3:
                        raise PackageError(f"invalid wildcard requirement {raw!r}")
                    fixed = []
                    wildcard = False
                    for item in parts:
                        if item.lower() in ("x", "*"):
                            wildcard = True
                        elif wildcard or not item.isdigit():
                            raise PackageError(f"invalid wildcard requirement {raw!r}")
                        else:
                            fixed.append(int(item))
                    if fixed:
                        lo = fixed + [0] * (3 - len(fixed))
                        hi = list(lo)
                        hi[len(fixed) - 1] += 1
                        tests.extend([(">=", Version(".".join(map(str, lo)))),
                                      ("<", Version(".".join(map(str, hi))))])
                    continue
                version, count = _partial(val)
                if op in ("^", "~"):
                    hi = list(version.numbers)
                    if op == "~":
                        pivot = 0 if count == 1 else 1
                    else:
                        pivot = next((i for i, n in enumerate(hi) if n), max(0, count - 1))
                    hi[pivot] += 1
                    for i in range(pivot + 1, 3):
                        hi[i] = 0
                    tests.extend([(">=", version), ("<", Version(".".join(map(str, hi))))])
                elif op == "=" and count < 3:
                    hi = list(version.numbers)
                    hi[count - 1] += 1
                    tests.extend([(">=", version), ("<", Version(".".join(map(str, hi))))])
                else:
                    tests.append((op, version))
            self.arms.append(tests)

    def accepts(self, version: Version) -> bool:
        for tests in self.arms:
            if version.pre and not any(v.pre and v.numbers == version.numbers for _, v in tests):
                continue
            okay = True
            for op, expected in tests:
                match = {"=": version == expected, ">=": version >= expected,
                         "<=": version <= expected, ">": version > expected,
                         "<": version < expected}[op]
                if not match:
                    okay = False
                    break
            if okay:
                return True
        return False
