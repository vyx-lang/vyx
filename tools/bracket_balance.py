#!/usr/bin/env python3
"""Vyx 源码括号平衡扫描 —— 用于 parser 在 EOF 报 `expected ')' / '{' / '}'` 时定位起始失衡点。

用法:
    python bracket_balance.py <file.vyx> [more.vyx ...]

对每行的 `(`/`)`、`{`/`}`、`[`/`]` 计数并累计深度（跳过行注释 `//`、块注释、字符串与字符字面量）。
给出「深度首次回到异常 / 文件尾仍不平衡」的行号，即最可能的未闭合起点。
"""
import sys

PAIRS = {')': '(', '}': '{', ']': '['}
OPEN = set(PAIRS.values())
CLOSE = set(PAIRS.keys())


def mask(src: str) -> list:
    """返回与 src 等长的状态列表: ' ' 表示普通代码, 其它字符表示该位置被吞掉的字面量/注释首字符。"""
    n = len(src)
    flags = [' '] * n
    i = 0
    line_start = True
    while i < n:
        c = src[i]
        if c == '\n':
            line_start = True
            i += 1
            continue
        # 行注释
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                flags[k] = '/'
            i = j
            continue
        # 块注释（支持嵌套）
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            depth = 1
            j = i + 2
            while j < n and depth > 0:
                if src[j] == '/' and j + 1 < n and src[j + 1] == '*':
                    depth += 1
                    j += 2
                elif src[j] == '*' and j + 1 < n and src[j + 1] == '/':
                    depth -= 1
                    j += 2
                else:
                    j += 1
            for k in range(i, min(j, n)):
                flags[k] = '*'
            i = j
            continue
        # 字符串
        if c == '"':
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == '"':
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                flags[k] = '"'
            i = j
            continue
        # 字符字面量
        if c == "'":
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == "'":
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                flags[k] = "'"
            i = j
            continue
        if not c.isspace():
            line_start = False
        i += 1
    return flags


def scan(path: str) -> int:
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        src = f.read()
    flags = mask(src)
    lines = src.split('\n')
    off = 0
    stack = []          # (char, line, col)
    problems = []
    for ln, line in enumerate(lines, start=1):
        L = len(line)
        for col in range(L):
            c = line[col]
            if flags[off + col] != ' ':
                continue
            if c in OPEN:
                stack.append((c, ln, col + 1))
            elif c in CLOSE:
                want = PAIRS[c]
                if stack and stack[-1][0] == want:
                    stack.pop()
                else:
                    got = stack[-1][0] if stack else '<nothing>'
                    problems.append(
                        f'  {path}:{ln}:{col+1}: mismatched `{c}` (innermost open is `{got}` '
                        f'opened at {stack[-1][1]}:{stack[-1][2]})' if stack else
                        f'  {path}:{ln}:{col+1}: stray `{c}` with empty stack')
        off += L + 1

    print(f'== {path}: {len(lines)} lines')
    if problems:
        print('  mismatches:')
        for p in problems[:20]:
            print(p)
        if len(problems) > 20:
            print(f'  ... ({len(problems) - 20} more)')
    if stack:
        print(f'  UNCLOSED at EOF: {len(stack)} level(s). '
              f'Vyx reports these DEEPEST-FIRST, so scan them top-down:')
        for i in range(len(stack) - 1, -1, -1):
            (ch, ln, cl) = stack[i]
            exp = {'(': ')', '{': '}', '[': ']'}[ch]
            tag = '  <== START HERE (the innermost thing never got closed)'
            if i == len(stack) - 1:
                tag = tag.lstrip()
            print(f'    level {len(stack)-i:>2}: `{ch}` at {path}:{ln}:{cl}  -> parser wanted `{exp}`')
        (ch, ln, cl) = stack[-1]
        print(f'  --> culprit line: {path}:{ln}  (something opened around {ln}:{cl} '
              f'is missing its closer)')
    elif not problems:
        print('  OK: balanced')
    return len(stack) + len(problems)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    rc = 0
    for p in sys.argv[1:]:
        try:
            rc += scan(p)
        except OSError as e:
            print(f'cannot read {p}: {e}')
            rc += 1
    sys.exit(1 if rc else 0)
