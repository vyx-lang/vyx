# IR Regression Guard — Report Template

<!-- 由 scripts/ir_diff.ps1 -Mode check 在每次跑后写入 last_report.md。
     本模板说明列含义：
     - level     : O0 / O2
     - case-id   : 来自 manifest.txt 的衍生 id
     - sha       : 'ok' 或 'got_prefix / want_prefix'
     - counts    : 'ok' / '(no counts baseline)' / 'WARN <op> <base>→<cur> (<pct>%)'
     - micro     : '-'(无 dsl) / 'ok' / 'WARN <DSL 报错>' -->