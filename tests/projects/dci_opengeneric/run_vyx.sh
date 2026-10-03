#!/usr/bin/env bash
# dci_opengeneric — 以 Vyx 为消费方的端到端门。
#
# 流程：两份契约**都由 adapter 从 provider 源码现场产出**（点对点 .dcib，生成
# 即 provenance —— 不存在手写契约这回事；契约只带不变事实，泛型实例零点名）->
# merge 实例布局声明并交叉核对 -> vyxc build（消费方降级缺实例就写 .dci_open
# 请求，构建内闭合循环派生 dci_close_instances.py 让生产端 adapter 现场闭合，
# rustc / clang++ 裁决边界并单态化）-> 运行产物断言输出。
#
# 构建两个 target，它们跑同一套泛型调用点（src/open_generic.vyx 与
# src/open_generic_cpp.vyx 的开放泛型与 record 调用点逐行相同，只 import 的
# 契约不同），只差 external backend 的 --provider：dci_opengeneric 由 rustc
# 物化 native/lib.rs，dci_opengeneric_cpp 由 clang++ 物化 native/lib.hpp
# （语言跟着扩展名走）。
#
# 两个 provider 都是**纯泛型源码**：lib.rs 里没有一个 #[no_mangle] 入口，lib.hpp
# 里没有一条显式实例化 —— 闭合符号与实例方法在两端都带 semantic_id，由
# rustc / clang++ 经 Active Adapter 在构建期现场物化。类型组合矩阵覆盖标量
# 宽度对（f32/f64、i32/i64、u64/u32、char/i64）与 **record 作类型实参**
# （Pair2<i32,Vec2>，24 字节嵌套聚合）—— 没有任何一张「支持类型」的硬表，
# 布局与约束全部由 rustc / clang++ 逐组合实测裁决。
#
# 两份源**逐行相同**（只差 `@[dci_import]` 那一行），都调用 `v.norm1()` ——
# 一个**非泛型** record 的成员函数，也就是「泛型 record 成员函数」旁边的对照。
# 它在两端各按自己产物的 ABI 导出：cpp 侧是 clang 的成员 mangling
# （`?norm1@Vec2@@QEBANXZ`），rust 侧是稳定链接名 `vyx_vec2_norm1`（rustc 不会
# 替一个没人引用的函数发射符号，所以它走 semantic_id 物化请求）。两份源发出的
# 请求（开放泛型 + instance 实例方法）只由调用点决定，与谁去物化无关，所以
# 两份 .dci_open sidecar 仍须逐字相同 —— 下面确实这么断言。
# 因此这里不导出 VYX_DCI_STUB_* —— 那些变量是全局覆盖，会把两个 target 都
# 钉死成同一个生产端；按 target 选生产端只能靠 Vyx.toml（见 Vyx.toml 注释）。
# env 仍然优先这一点在脚本末尾单独验证一次。
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
PROJ="$REPO/tests/projects/dci_opengeneric"
BOOT="$REPO/bootstrap_compiler/out/boot.exe"

w() { cygpath -m "$1"; }

# LLVM 工具链必须是 **Windows** 形式：编译器拼 clang++ 命令行时会把 `/` 朴素换成
# `\`，MSYS 形式（`/e/Dev/...`）会变成 `\e\Dev\...`，三个 C++ job 一起
# `failed to start compile job`。--emit=mir/obj 这两步要用它。
LLVM_ROOT_WIN="$(w "$REPO/clang")"

PY="$(command -v python)"
[ -n "$PY" ] || { echo "FAIL python not found"; exit 1; }
[ -f "$BOOT" ] || { echo "FAIL boot.exe not found: $BOOT"; exit 1; }
command -v rustc >/dev/null || { echo "FAIL rustc not found"; exit 1; }

# ============ 每个生产端一份契约（.dcib 是点对点的，adapter 产出） ============
# rust 那份由 rust adapter 直接从 native/lib.rs 产出：闭合符号（vyx_vec2_*）与
# 非泛型方法（Vec2::norm1）作为 Active 物化请求进契约（带 semantic_id，构建期
# 由 rustc 物化），开放泛型**不进契约** —— 它们由调用点实参闭合，经 .dci_open
# sidecar 交给 Active Adapter。**泛型实例方法也不进契约**：消费方降级时缺哪个
# 实例就往 .dci_open 写一行 `instance <type-text>`，构建内闭合循环派生
# dci_close_instances.py 现场让生产端闭合（dcib 溯源 -> rust adapter），零手写。
echo "== derive the rust contract from native/lib.rs =="
DCIB_RUST="$(w "$PROJ/dci/open_generic.dcib")"
if ! "$PY" "$(w "$REPO/tools/dci/dci_adapter_rust.py")" "$(w "$PROJ/native/lib.rs")" \
     -o "$DCIB_RUST" --export-active-requests; then
  echo "FAIL the rust adapter could not derive a contract"; exit 1
fi

echo "== derive the C++ contract from native/lib.hpp =="
DCIB_CPP="$(w "$PROJ/dci/open_generic.cpp.dcib")"
# 与 rust 完全同构：契约只带不变事实（闭合符号、非泛型方法、声明的实例布局），
# 泛型实例方法留给消费方发现 —— 头文件里没有任何 `template struct ...;` 显式
# 实例化，脚本里也没有任何 `--export-instance`。
if ! "$PY" "$(w "$REPO/tools/dci/dci_adapter_msvc.py")" \
     --out "$DCIB_CPP" --include "$(w "$PROJ/native/lib.hpp")"; then
  echo "FAIL the cpp adapter could not derive a contract"; exit 1
fi

# 泛型类型的**实例**布局声明与拼写映射：adapter 实测的布局按 provider 拼写命名，
# merge_instance_layouts.py 把 type_name 重拼成消费方拼写（Pair2<f64,i32>），
# 并与声明逐字段交叉核对（一致才算数）。两份契约都过 merge：对 rust 那份它
# 顺带完成 provider 拼写 -> 消费方拼写的改写；对 cpp 那份还把 MSVC 布局名
# （`Pair2<double, int>`）映射到同一个键。任何一侧实测与声明漂移即红。
echo "== merge the declared instance layouts into both contracts =="
"$PY" "$(w "$REPO/tools/dci/merge_instance_layouts.py")" \
  "$DCIB_RUST" "$(w "$PROJ/dci/instance_layouts.json")" \
  || { echo "FAIL could not merge instance layouts into the rust contract"; exit 1; }
"$PY" "$(w "$REPO/tools/dci/merge_instance_layouts.py")" \
  "$DCIB_CPP" "$(w "$PROJ/dci/instance_layouts.json")" \
  || { echo "FAIL could not merge instance layouts into the cpp contract"; exit 1; }

# 人工声明必须被生产端兑现：探针按**契约里的数字**断言 sizeof/alignof/offsetof，
# 由 rustc / clang++ 裁决。声明与产物漂移就红 —— 否则「契约写 16 字节」和
# 「编译器摆成 16 字节」只是两句恰好同时成立的话。
echo "== verify the declared instance layouts with each producer =="
for L in rust cpp; do
  if [ "$L" = rust ]; then
    CONTRACT="$DCIB_RUST"; PROVIDER="$(w "$PROJ/native/lib.rs")"
  else
    CONTRACT="$DCIB_CPP"; PROVIDER="$(w "$PROJ/native/lib.hpp")"
  fi
  "$PY" "$(w "$REPO/tools/dci/verify_instance_layouts.py")" \
    "$CONTRACT" "$(w "$PROJ/dci/instance_layouts.json")" \
    --lang "$L" --provider "$PROVIDER" \
    || { echo "FAIL $L: the declared instance layout is not what the producer emits"
         exit 1; }
done

# 两份契约**不做逐字段比对**，也不该比：它们各自描述一个生产端的产物
# （rustc 编 lib.rs / clang++ 编 lib.hpp），provenance、mangling、布局记录
# 本来就不同。点对点契约的验收标准是「它服务的那个 target 能编能跑」，
# 那由下面两步回答。拿两份契约互比会把「点对点」重新读成「通用」。
#
# 认实例符号的 mangling 标记 `__`（twice__i32），而不是裸名字 —— 裸 `identity`
# 会撞上 profile 里的 `type_identity`，byte grep 就变成永远为真的空断言。
for ONE in "$DCIB_RUST" "$DCIB_CPP"; do
  for SYM in vyx_vec2_make vyx_vec2_x vyx_vec2_y; do
    grep -qs "$SYM" "$ONE" || { echo "FAIL $ONE lacks closed symbol $SYM"; exit 1; }
  done
  for INST in 'twice__' 'max_of__' 'identity__' 'swap__'; do
    grep -qs "$INST" "$ONE" && { echo "FAIL $ONE carries instance $INST"; exit 1; }
  done
  # 每个消费者用到的实例布局都必须落在契约里（merge 后按消费方拼写）。
  for LAYOUT in 'Pair2<f64,i32>' 'Pair2<i32,f64>' 'Pair2<f64,f32>' \
                'Pair2<i64,i32>' 'Pair2<u32,u64>' 'Pair2<i64,char>' \
                'Pair2<i32,Vec2>'; do
    grep -qsF "$LAYOUT" "$ONE" \
      || { echo "FAIL $ONE lacks the $LAYOUT instance layout"; exit 1; }
  done
done
# 方法符号（record 的成员函数）两条通道各断一次：
#   * **非泛型** record：adapter 能命名 `Vec2`，两端各按自己产物的 ABI 导出 ——
#     cpp 那条 link_name 就是 clang 的成员 mangling（`kind=method, owner=Vec2`）；
#     rust 那条是稳定链接名 `vyx_vec2_norm1`（同一个 `kind=method, owner=Vec2`，
#     外加 semantic_id 物化请求，见下）；
#   * **泛型** record 的实例：**不进契约**。消费方降级时缺哪个实例就在
#     .dci_open 里写一行 `instance <type-text>`，构建内闭合循环派生
#     dci_close_instances.py 让生产端闭合，产物进 supplement（下面构建后断言
#     supplement 里 semantic_id 齐全）。零手写点名 —— 契约里出现任何 Pair2
#     实例方法都算失败。
# 两条断的都是「符号真的过边界了」而不是「文件里恰好有这几个字节」—— 链接期少
# 了它们会以 `undefined symbol: ...` 直接炸掉。它们和下面 provider 侧那条
# llvm-nm 断言是**分开**的：这里证明「契约里有」，那里证明「目标文件里有」，
# 两边都成立，消费方引用的名字才真的能被链接器兑现。
grep -qs "?norm1@Vec2@@QEBANXZ" "$DCIB_CPP" \
  || { echo "FAIL $DCIB_CPP lacks the adapter-derived Vec2::norm1 method symbol"; exit 1; }
grep -qs "vyx_vec2_norm1" "$DCIB_RUST" \
  || { echo "FAIL $DCIB_RUST lacks the adapter-derived Vec2::norm1 method symbol"; exit 1; }
# cpp 契约：实例方法带 semantic_id，link_name 仍是最终 MSVC mangled 成员符号。
# cpp 契约：实例方法**不**预导出 —— 开放泛型实例由消费方在 .dci_open 里发现、
# 构建内闭合循环补进 supplement。反向锁：预导出了任何一个 Pair2 实例方法都算
# 破坏「零手写点名」。
if grep -qF 'dci.active.cpp.Pair2::' "$DCIB_CPP" \
   || grep -qF '?get_first@?$Pair2@' "$DCIB_CPP" \
   || grep -qF '?swapped@?$Pair2@' "$DCIB_CPP"; then
  echo "FAIL $DCIB_CPP pre-exports a closed Pair2 instance (open generics must"
  echo "      be consumer-discovered via .dci_open, not hand-listed)"
  exit 1
fi
# 非泛型方法照旧在契约里：clang 的成员 mangling。
grep -qs "?norm1@Vec2@@QEBANXZ" "$DCIB_CPP" \
  || { echo "FAIL $DCIB_CPP lacks the adapter-derived Vec2::norm1 method symbol"; exit 1; }
# rust 契约：泛型实例方法**不**预导出（消费方发现 -> 构建内闭合），只有非泛型
# record 的方法带 semantic_id 物化请求：它的请求面里**不许出现接收者**
# （`norm1()` 而不是 `norm1(&Vec2)`）—— 接收者是方法查询的隐式部分，写进实参
# 列表会被生产端当成一个名叫 `&Vec2` 的 record 去找。
if grep -qF 'dci.active.rust.Pair2::' "$DCIB_RUST"; then
  echo "FAIL $DCIB_RUST pre-exports a closed Pair2 instance method (open"
  echo "      generics must be consumer-discovered via .dci_open, not hand-listed)"
  exit 1
fi
grep -qsF 'dci.active.rust.Vec2::norm1()' "$DCIB_RUST" \
  || { echo "FAIL $DCIB_RUST lacks the Vec2::norm1 active materialization request"
       exit 1; }
# 反向锁：provider 已经实例化/已经物化的符号不能躺在 rejected 里。
# cpp 的 rejected 必须为空（每个点名的实例方法都可导出 —— DCI_SPEC §12，
# 设计文档 §2.1/§5.2）；rust 的 rejected 必须恰好是 4 个开放泛型函数本身
# （它们由调用点闭合，不属于静态契约 —— 但少一个多一个都说明解析漂了）。
DECODE_RUST="$(mktemp -d)/rust_contract.json"
DECODE_CPP="$(mktemp -d)/cpp_contract.json"
"$PY" "$(w "$REPO/tools/dci/dcib.py")" decode "$DCIB_RUST" "$(w "$DECODE_RUST")" >/dev/null
"$PY" "$(w "$REPO/tools/dci/dcib.py")" decode "$DCIB_CPP" "$(w "$DECODE_CPP")" >/dev/null
REJ_CPP=$("$PY" -c "import json,sys; d=json.load(open(sys.argv[1],encoding='utf-8')); print(','.join(sorted(r.get('name','') for r in (d.get('exports',{}).get('rejected_symbols') or []))))" "$(w "$DECODE_CPP")") || true
[ -z "$REJ_CPP" ] \
  || { echo "FAIL the cpp contract rejects producer-instantiated symbol(s): $REJ_CPP"
       echo "     (an instance the producer's own toolchain already closed must be"
       echo "      exportable -- DCI_SPEC §12; design doc §2.1/§5.2)"
       exit 1; }
REJ_RUST=$("$PY" -c "import json,sys; d=json.load(open(sys.argv[1],encoding='utf-8')); print(','.join(sorted(r.get('name','') for r in (d.get('exports',{}).get('rejected_symbols') or []))))" "$(w "$DECODE_RUST")") || true
[ "$REJ_RUST" = "identity,max_of,swap,twice" ] \
  || { echo "FAIL the rust contract's rejected set drifted: expected exactly the"
       echo "     4 open generics (identity,max_of,swap,twice), got: $REJ_RUST"
       exit 1; }
echo "CONTRACT ok (both .dcib adapter-derived: 3 closed symbols, no open-generic"
echo "           instances, the full instance-layout matrix incl. Pair2<i32,Vec2>;"
echo "           NO instance method pre-exported on either side -- Pair2 instances"
echo "           are consumer-discovered via .dci_open and closed inside the build;"
echo "           both keep the Vec2::norm1 method symbol --"
echo "           ?norm1@Vec2@@QEBANXZ on cpp, vyx_vec2_norm1 on rust; rust"
echo "           rejects exactly the 4 open generics, cpp rejects nothing)"

# 契约的**合法性**单独钉一次，别让它只在构建期以 I0100 的形式爆出来。
# clang 把 16 字节 POD 按值参数降成 `ptr dead_on_return`（借用指针），对应
# passing=indirect；adapter 若机械翻成 coerce→ptr，载体（8 字节）与 storage
# （16 字节）不等宽 —— 那是 dci_rust_generic/run.ps1 的 coerce_parameter_width
# 负例已经定义过的非法契约，报错却长成编译器 bug 的样子，容易查错方向。
if ! "$PY" "$(w "$PROJ/tools/check_cpp_contract.py")" "$(w "$DECODE_CPP")"; then
  echo "FAIL the cpp contract is not a legal by-value aggregate lowering"
  exit 1
fi

echo "== vyxc build (two targets, one Vyx source, two producers) =="
# 清理走 mv 移开（工具会话的安全删除层会拦截 rm -rf）；stash 放系统临时目录
STASH="$(mktemp -d)/dci_og_stash"
mkdir -p "$STASH"
mv "$(w "$PROJ/.cache")" "$STASH/cache" 2>/dev/null || true
mv "$(w "$PROJ/target")" "$STASH/target" 2>/dev/null || true
mkdir -p "$(w "$PROJ/target")"
BUILD_LOG="$(w "$PROJ/target/build.log")"
# -j 1 是硬要求：开放泛型的闭合重试（compile -> .dci_open -> closer -> 重降级）
# 挂在单线程驱动器的即时任务路径上；并行路径会把第一次失败的编译直接判死。
if ! ( cd "$(w "$PROJ")" && "$BOOT" build -j 1 ) > "$BUILD_LOG" 2>&1; then
  echo "FAIL vyxc build"; tail -40 "$BUILD_LOG"; exit 1
fi

# 每个生产端一份 adapter 日志，按日志里写的语言定位（不猜文件名）
RUST_LOG="$(grep -l '\[dci-active\] rust' "$(w "$PROJ")"/.cache/*.err.log | head -1)"
CPP_LOG="$(grep -l '\[dci-active\] cpp' "$(w "$PROJ")"/.cache/*.err.log | head -1)"
[ -n "$RUST_LOG" ] || { echo "FAIL no rust adapter log"; tail -20 "$BUILD_LOG"; exit 1; }
[ -n "$CPP_LOG" ] || { echo "FAIL no cpp adapter log"; tail -20 "$BUILD_LOG"; exit 1; }

# rust：3 个闭合契约符号 + 全部开放泛型实例 + 实例方法物化，经 resolve_batch 裁决
RUST_CLOSED=$(grep -cs "CLOSED" "$RUST_LOG")
[ "$RUST_CLOSED" -ge 13 ] \
  || { echo "FAIL rust: expected >=13 closed requests, got $RUST_CLOSED"; \
       cat "$RUST_LOG"; exit 1; }

# cpp：全部开放泛型实例经 clang++ 裁决 + 12 条实例方法物化请求（6 个方法
# 接收者实例 × get_first/swapped）+ 3 个闭合契约符号（非模板，会话不裁决，
# 由契约直接给出 ABI）。只数总数会放过「少闭合一个」，所以下面还有逐符号断言。
CPP_CLOSED=$(grep -cs "CLOSED" "$CPP_LOG")
CPP_CONTRACT=$(grep -cs "CONTRACT" "$CPP_LOG")
[ "$CPP_CLOSED" -ge 34 ] \
  || { echo "FAIL cpp: expected >=34 adjudicated requests, got $CPP_CLOSED"; \
       cat "$CPP_LOG"; exit 1; }
[ "$CPP_CONTRACT" -ge 3 ] \
  || { echo "FAIL cpp: expected 3 contract symbols, got $CPP_CONTRACT"; \
       cat "$CPP_LOG"; exit 1; }

# 两个生产端必须各自真的物化出这些实例 —— 只数总数会放过「少闭合一个」。
for SYM in twice__i32 twice__f64 max_of__i32 swap__i32_f64 \
           swap__f32_f64 swap__i32_i64 swap__u64_u32 swap__char_i64 swap__Vec2_i32; do
  grep -qs "$SYM" "$RUST_LOG" \
    || { echo "FAIL rust: $SYM was never materialized"; cat "$RUST_LOG"; exit 1; }
  grep -qs "$SYM" "$CPP_LOG" \
    || { echo "FAIL cpp: $SYM was never materialized"; cat "$CPP_LOG"; exit 1; }
done
grep -qs "identity__" "$RUST_LOG" \
  || { echo "FAIL rust: identity<Vec2> was never materialized"; cat "$RUST_LOG"; exit 1; }
grep -qs "identity__" "$CPP_LOG" \
  || { echo "FAIL cpp: identity<Vec2> was never materialized"; cat "$CPP_LOG"; exit 1; }
echo "BUILD ok (rust closed $RUST_CLOSED, cpp closed $CPP_CLOSED + $CPP_CONTRACT contract)"

# ============ 构建内闭合：消费方发现，生产端补齐 ============
# 消费方降级时缺哪个实例就在 .dci_open 里记一行 `instance <type-text>`，构建
# 重试循环派生 dci_close_instances.py（dcib 溯源 -> 对应语言的 adapter）闭合进
# supplement。这两个文件就是「零手写点名」的直接证据：它们必须存在，且带上
# 消费方用到的全部实例方法（semantic_id / MSVC mangled，消费方拼写）。
RUST_SUPP="$(w "$PROJ")/.cache/dci_closed_facts_dci_opengeneric.dcib"
CPP_SUPP="$(w "$PROJ")/.cache/dci_closed_facts_dci_opengeneric_cpp.dcib"
for SUPP in "$RUST_SUPP" "$CPP_SUPP"; do
  [ -f "$SUPP" ] \
    || { echo "FAIL the build did not close the discovered instances internally (missing $SUPP)"
         exit 1; }
done
# 实例方法的 semantic_id：**只**断消费方调用点真正逼出来的实例 —— 6 个方法
# 接收者（swap 返回 Pair2<U,T>，消费方在其上调用 get_first/swapped）。
# 反序实例（Pair2<i32,f64> 等）只作为 swapped() 的**返回类型**出现：它们要
# 的是布局（契约里 merge 过的声明矩阵），不是方法符号 —— 消费方没调过它们的
# 方法，supplement 里就不该有。这正是「开放泛型只闭合被逼出来的事实」。
for SEM in 'dci.active.rust.Pair2::get_first(f64,i32)' \
           'dci.active.rust.Pair2::swapped(f64,i32)' \
           'dci.active.rust.Pair2::get_first(f64,f32)' \
           'dci.active.rust.Pair2::get_first(i64,i32)' \
           'dci.active.rust.Pair2::get_first(u32,u64)' \
           'dci.active.rust.Pair2::get_first(i64,char)' \
           'dci.active.rust.Pair2::get_first(i32,Vec2)'; do
  grep -qsF "$SEM" "$RUST_SUPP" \
    || { echo "FAIL the closure supplement lacks the materialization request $SEM"; exit 1; }
done
for SEM in 'dci.active.cpp.Pair2::get_first(f64,i32)' \
           'dci.active.cpp.Pair2::swapped(f64,i32)' \
           'dci.active.cpp.Pair2::get_first(f64,f32)' \
           'dci.active.cpp.Pair2::get_first(i64,i32)' \
           'dci.active.cpp.Pair2::get_first(u32,u64)' \
           'dci.active.cpp.Pair2::get_first(i64,char)' \
           'dci.active.cpp.Pair2::get_first(i32,Vec2)'; do
  grep -qsF "$SEM" "$CPP_SUPP" \
    || { echo "FAIL the closure supplement lacks the materialization request $SEM"; exit 1; }
done
# 返回**另一个实例**的方法（`swapped() -> Pair2<B, A>`）也要在 supplement 里。
# 这条断言对应的正是"名字毫无歧义、却被拒"的那个缺口：符号名里接收者是
# `@NH@`、返回是 `?AU?$Pair2@HN@@`。
for SYM in '?get_first@?$Pair2@NH@@QEBANXZ' \
           '?swapped@?$Pair2@NH@@QEBA?AU?$Pair2@HN@@XZ'; do
  grep -qsF "$SYM" "$CPP_SUPP" \
    || { echo "FAIL the closure supplement lacks the mangled member symbol $SYM"; exit 1; }
done
echo "CLOSURE ok (both builds closed the consumer-discovered instances internally)"

# ============ provider 侧：物化符号真的进了目标文件 ============
# 「契约里有」和「目标文件里有」是两种故障：这里把后者钉住。clang++ 必须把
# 全部实例的成员符号发射进 shim 对象（force-use 体触发 clang 隐式实例化，
# 发射的就是真实成员符号本身 —— 不是我们手写的包装），rustc 必须把闭合符号
# 与实例方法（vyx_pair2_*，UFCS 物化）发射进 rust shim 对象。
NM="$(w "$REPO/clang/bin/llvm-nm.exe")"
[ -f "$NM" ] || { echo "FAIL llvm-nm not found: $NM"; exit 1; }

# --- cpp shim 对象 ---
CPP_SHIM_OBJS=()
for one in "$(w "$PROJ")"/.cache/dci_opengeneric_cpp_*dci_stubs_external*.obj; do
  [ -f "$one" ] && CPP_SHIM_OBJS+=("$one")
done
[ "${#CPP_SHIM_OBJS[@]}" -ge 1 ] \
  || { echo "FAIL no cpp shim object to inspect; the nm check would be vacuous"; exit 1; }
CPP_NM="$(mktemp -d)/cpp_shim.nm"
: > "$CPP_NM"
for one in "${CPP_SHIM_OBJS[@]}"; do "$NM" "$one" >> "$CPP_NM" 2>&1; done
CPP_NM_LINES=$(wc -l < "$CPP_NM")
[ "$CPP_NM_LINES" -ge 4 ] \
  || { echo "FAIL llvm-nm read only $CPP_NM_LINES line(s) from ${#CPP_SHIM_OBJS[@]} object(s)"; \
       cat "$CPP_NM"; exit 1; }
# MSVC mangling: N=double, H=int。`Pair2@NH@` = Pair2<double,int>（消费方的
# Pair2<f64,i32>）；`Pair2@HN@` = 反之。
for SYM in \
    '?get_first@?$Pair2@NH@@QEBANXZ' \
    '?swapped@?$Pair2@NH@@QEBA?AU?$Pair2@HN@@XZ' \
    '?get_first@?$Pair2@HN@@QEBAHXZ' \
    '?swapped@?$Pair2@HN@@QEBA?AU?$Pair2@NH@@XZ' \
    '?norm1@Vec2@@QEBANXZ'; do
  grep -qF "$SYM" "$CPP_NM" \
    || { echo "FAIL the cpp object export set lacks $SYM"
         echo "     (inspected ${#CPP_SHIM_OBJS[@]} object(s), $CPP_NM_LINES symbol line(s))"
         exit 1; }
done
# 组合矩阵里每个**被调用过方法**的实例都必须真的存在（不手写 mangling，按
# 模板名数）：6 个方法接收者实例各一条 get_first。swapped 只有消费方调用的
# 那个实例（f64,i32）需要。
CPP_GET_FIRST=$(grep -cF '?get_first@?$Pair2@' "$CPP_NM")
[ "$CPP_GET_FIRST" -ge 6 ] \
  || { echo "FAIL cpp object has only $CPP_GET_FIRST Pair2<N,M>::get_first symbols"
       cat "$CPP_NM"; exit 1; }
echo "PROVIDER ok (cpp: $CPP_GET_FIRST instance get_first symbols + norm1 in ${#CPP_SHIM_OBJS[@]} object(s))"

# --- rust shim 对象：闭合符号 + 实例方法全部由 rustc 物化 ---
RUST_SHIM_OBJS=()
for one in "$(w "$PROJ")"/.cache/dci_opengeneric_*dci_stubs_external*.obj; do
  [ -f "$one" ] && RUST_SHIM_OBJS+=("$one")
done
[ "${#RUST_SHIM_OBJS[@]}" -ge 1 ] \
  || { echo "FAIL no rust shim object to inspect; the nm check would be vacuous"; exit 1; }
RUST_NM="$(mktemp -d)/rust_shim.nm"
: > "$RUST_NM"
for one in "${RUST_SHIM_OBJS[@]}"; do "$NM" "$one" >> "$RUST_NM" 2>&1; done
RUST_NM_LINES=$(wc -l < "$RUST_NM")
[ "$RUST_NM_LINES" -ge 4 ] \
  || { echo "FAIL llvm-nm read only $RUST_NM_LINES line(s) from ${#RUST_SHIM_OBJS[@]} object(s)"; \
       cat "$RUST_NM"; exit 1; }
# provider 零 #[no_mangle]：所有 vyx_pair2_* 实例方法符号必须由 rustc 在构建期
# 物化进 shim 对象 —— 5 个消费方调用的实例各一条 get_first。
RUST_METHODS=$(grep -cF 'vyx_pair2_' "$RUST_NM")
[ "$RUST_METHODS" -ge 5 ] \
  || { echo "FAIL rust object has only $RUST_METHODS vyx_pair2_* materialized symbols"
       cat "$RUST_NM"; exit 1; }
RUST_CLOSED_NM=$(grep -cF 'vyx_vec2_' "$RUST_NM")
[ "$RUST_CLOSED_NM" -ge 3 ] \
  || { echo "FAIL rust object has only $RUST_CLOSED_NM vyx_vec2_* symbols"
       cat "$RUST_NM"; exit 1; }
# 非泛型 record 的成员函数也必须真的被 rustc 发射：它**不在** provider 源码的
# 契约符号里（`lib.rs` 零 `#[no_mangle]`），靠 Active Adapter 构建期生成 UFCS
# shim 才活下来。少了它消费方那条 `v.norm1()` 会在链接期以 undefined symbol 炸。
grep -qF 'vyx_vec2_norm1' "$RUST_NM" \
  || { echo "FAIL the rust object lacks the non-generic Vec2::norm1 method symbol"
       cat "$RUST_NM"; exit 1; }
echo "PROVIDER ok (rust: $RUST_METHODS materialized instance methods + $RUST_CLOSED_NM closed symbols incl. vyx_vec2_norm1 in ${#RUST_SHIM_OBJS[@]} object(s))"

# ============ 泛型 record 的成员函数：**消费方**那半边 ============
# 直接拿两个端到端消费方源码做符号级断言（单文件 --emit=mir / --emit=obj，
# 不需要 Active Adapter）：
#   * `--emit=mir` 把 dump 写到 **stdout**（不认 -o），所以断言读日志本身。
#     MIR 这一层不做契约符号解析（失败要到 codegen 查符号才发生），所以 mir
#     跑不用带 supplement。
#   * `--emit=obj` 在 codegen 期解析 DCI 符号 —— 消费方的完整输入就是
#     「契约 + 闭合 supplement」（和 vyxc build 同一组 --dci），缺了
#     supplement 实例方法在单文件编译里永远解析不到。断言的是消费方这一侧
#     的符号引用，Active Adapter（生产端）不参与。
#   * cpp 用 open_generic_cpp.vyx，rust 用 open_generic.vyx —— 两个生产端各自
#     钉自己的产物，「cpp 那份对了」推不出「rust 那份也对了」。
OK_LOG="$(w "$PROJ")/target/open_generic_cpp_consumer.log"
OBJ_OUT="$(w "$PROJ")/target/open_generic_cpp_consumer.o"
if ! ( cd "$(w "$PROJ")" \
       && LLVM_ROOT="$LLVM_ROOT_WIN" "$BOOT" --src=file "src/open_generic_cpp.vyx" \
            --emit=mir ) > "$OK_LOG" 2>&1; then
  echo "FAIL the generic-record member function no longer compiles"; cat "$OK_LOG"; exit 1
fi
# 断言实例真的被物化了：抓那条 owner_type 是**实例**的 MIR 函数行。
# flags=172 = foreign_dci(32) | const_method(128) | declaration_only(8) |
# closed_generic_env(4)；对照**非泛型** DCI 方法（Vec2::norm1）是 168 ——
# 差的这 4 就是「所有者是模板实例」这件事本身。block_count=0 才是无体声明。
INST_LINE="$(grep -F 'name=mir2$Pair2_3A_3A_3Cf64_2Ci32_3E_3A_3Aget_5Ffirst' "$OK_LOG" | head -1 || true)"
[ -n "$INST_LINE" ] \
  || { echo "FAIL no closed Pair2<f64,i32>::get_first instance was materialized"
       grep -F 'name=mir2$Pair2' "$OK_LOG" | head -5; exit 1; }
case "$INST_LINE" in
  *"owner_type=Pair2::<f64,i32>"*) ;;
  *) echo "FAIL the instance is not owned by the closed receiver: $INST_LINE"; exit 1 ;;
esac
case "$INST_LINE" in
  *"block_count=0"*) ;;
  *) echo "FAIL the instance is supposed to stay a bodiless declaration: $INST_LINE"; exit 1 ;;
esac
case "$INST_LINE" in
  *"flags=172"*) ;;
  *) echo "FAIL the instance lacks closed_generic_env (flags=172): $INST_LINE"; exit 1 ;;
esac

# 同一个接收者上的**另一个方法**，返回类型是**另一个实例**（`Pair2<B, A>`）。
# 断言拆成两条互补的：MIR 里返回类型必须解析成**第二个实例**（否则"过了"可能
# 只是因为返回类型退化成了标量/裸指针），目标文件里必须留下对**正确 mangled
# 名**的未定义引用。
#
# 注意 `$OK_LOG` 会被下面那次 `--emit=obj` 覆盖，所以 MIR 断言必须在这里做，
# 不能挪到 nm 之后。
SW_LINE="$(grep -F 'name=mir2$Pair2_3A_3A_3Cf64_2Ci32_3E_3A_3Aswapped' "$OK_LOG" | head -1 || true)"
[ -n "$SW_LINE" ] \
  || { echo "FAIL no closed Pair2<f64,i32>::swapped instance was materialized"
       grep -F 'name=mir2$Pair2' "$OK_LOG" | head -5; exit 1; }
case "$SW_LINE" in
  *"Pair2::<i32,f64>"*) ;;
  *) echo "FAIL swapped's return type is not the second instance: $SW_LINE"; exit 1 ;;
esac
case "$SW_LINE" in
  *"owner_type=Pair2::<f64,i32>"*) ;;
  *) echo "FAIL swapped is not owned by the closed receiver: $SW_LINE"; exit 1 ;;
esac
case "$SW_LINE" in
  *"flags=172"*) ;;
  *) echo "FAIL swapped lacks closed_generic_env (flags=172): $SW_LINE"; exit 1 ;;
esac

if ! ( cd "$(w "$PROJ")" \
       && LLVM_ROOT="$LLVM_ROOT_WIN" "$BOOT" --src=file "src/open_generic_cpp.vyx" \
            --emit=obj -o "$OBJ_OUT" --dci "$DCIB_CPP" --dci "$CPP_SUPP" ) > "$OK_LOG" 2>&1; then
  echo "FAIL the generic-record member function does not lower to an object"
  cat "$OK_LOG"; exit 1
fi
[ -s "$OBJ_OUT" ] || { echo "FAIL empty consumer object: $OBJ_OUT"; exit 1; }
CONSUMER_NM="$(mktemp -d)/consumer.nm"
"$NM" "$OBJ_OUT" > "$CONSUMER_NM" 2>&1
CONSUMER_LINES=$(wc -l < "$CONSUMER_NM")
[ "$CONSUMER_LINES" -ge 4 ] \
  || { echo "FAIL llvm-nm read only $CONSUMER_LINES line(s) from the consumer object"
       cat "$CONSUMER_NM"; exit 1; }
REF_LINE="$(grep -F '?get_first@?$Pair2@NH@@QEBANXZ' "$CONSUMER_NM" | head -1 || true)"
[ -n "$REF_LINE" ] \
  || { echo "FAIL the consumer object does not reference Pair2<double,int>::get_first"
       echo "     (read $CONSUMER_LINES symbol line(s))"; cat "$CONSUMER_NM"; exit 1; }
# 必须是**未定义**引用（`U`）：已定义就说明消费方误会了边界 —— 它应该在
# codegen 期把这条声明解析成外部符号，而不是自己生成一个实现。
case "$REF_LINE" in
  *" U "*) ;;
  *) echo "FAIL the consumer did not leave the instance method undefined: $REF_LINE"
     exit 1 ;;
esac
# 同一个接收者上返回**另一个实例**的方法：目标文件必须引用正确的 mangled 名，
# 并且同样是**未定义**。`?swapped@?$Pair2@NH@@QEBA?AU?$Pair2@HN@@XZ` 里
# 接收者是 `@NH@` = Pair2<double,int>、返回是 `?AU?$Pair2@HN@@` =
# Pair2<int,double> —— 「接收者 NH / 返回 HN」这层反序正是本用例要钉的：
# 解析错了会写成同一个实例，而那种错误在只查「有没有引用」时完全看不见。
SWAP_REF="$(grep -F '?swapped@?$Pair2@NH@@QEBA?AU?$Pair2@HN@@XZ' "$CONSUMER_NM" | head -1 || true)"
[ -n "$SWAP_REF" ] \
  || { echo "FAIL the consumer object does not reference Pair2<double,int>::swapped"
       echo "     (read $CONSUMER_LINES symbol line(s))"; cat "$CONSUMER_NM"; exit 1; }
case "$SWAP_REF" in
  *" U "*) ;;
  *) echo "FAIL the consumer did not leave swapped undefined: $SWAP_REF"; exit 1 ;;
esac
echo "CONSUMER ok (instance materialized with closed_generic_env, object references $REF_LINE)"
echo "CONSUMER ok2 (swapped returns the second instance, object references $SWAP_REF)"

# rust 生产端的同一组断言：同一份端到端消费方源码，只是契约与符号命名是 rust 那份。
RUST_OK_LOG="$(w "$PROJ")/target/open_generic_rust_consumer.log"
RUST_OBJ_OUT="$(w "$PROJ")/target/open_generic_rust_consumer.o"
if ! ( cd "$(w "$PROJ")" \
       && LLVM_ROOT="$LLVM_ROOT_WIN" "$BOOT" --src=file "src/open_generic.vyx" \
            --emit=mir ) > "$RUST_OK_LOG" 2>&1; then
  echo "FAIL the rust generic-record member fixture no longer compiles"; cat "$RUST_OK_LOG"; exit 1
fi
RUST_INST_LINE="$(grep -F 'name=mir2$Pair2_3A_3A_3Cf64_2Ci32_3E_3A_3Aget_5Ffirst' "$RUST_OK_LOG" | head -1 || true)"
[ -n "$RUST_INST_LINE" ] \
  || { echo "FAIL no closed Pair2<f64,i32>::get_first instance was materialized (rust)"
       grep -F 'name=mir2$Pair2' "$RUST_OK_LOG" | head -5; exit 1; }
case "$RUST_INST_LINE" in
  *"flags=172"*) ;;
  *) echo "FAIL the rust instance lacks closed_generic_env (flags=172): $RUST_INST_LINE"; exit 1 ;;
esac
if ! ( cd "$(w "$PROJ")" \
       && LLVM_ROOT="$LLVM_ROOT_WIN" "$BOOT" --src=file "src/open_generic.vyx" \
            --emit=obj -o "$RUST_OBJ_OUT" --dci "$DCIB_RUST" --dci "$RUST_SUPP" ) > "$RUST_OK_LOG" 2>&1; then
  echo "FAIL the rust generic-record member fixture does not lower to an object"
  cat "$RUST_OK_LOG"; exit 1
fi
[ -s "$RUST_OBJ_OUT" ] || { echo "FAIL empty rust consumer object: $RUST_OBJ_OUT"; exit 1; }
RUST_CONSUMER_NM="$(mktemp -d)/consumer_rust.nm"
"$NM" "$RUST_OBJ_OUT" > "$RUST_CONSUMER_NM" 2>&1
RUST_REF="$(grep -F 'vyx_pair2_f64_i32_get_first' "$RUST_CONSUMER_NM" | head -1 || true)"
[ -n "$RUST_REF" ] \
  || { echo "FAIL the rust consumer object does not reference vyx_pair2_f64_i32_get_first"
       cat "$RUST_CONSUMER_NM"; exit 1; }
case "$RUST_REF" in
  *" U "*) ;;
  *) echo "FAIL the rust consumer did not leave the instance method undefined: $RUST_REF"
     exit 1 ;;
esac
RUST_SWAP_REF="$(grep -F 'vyx_pair2_f64_i32_swapped' "$RUST_CONSUMER_NM" | head -1 || true)"
[ -n "$RUST_SWAP_REF" ] \
  || { echo "FAIL the rust consumer object does not reference vyx_pair2_f64_i32_swapped"
       cat "$RUST_CONSUMER_NM"; exit 1; }
case "$RUST_SWAP_REF" in
  *" U "*) ;;
  *) echo "FAIL the rust consumer did not leave swapped undefined: $RUST_SWAP_REF"; exit 1 ;;
esac
# 非泛型 record 的成员函数：rust 消费方也要留下对 `vyx_vec2_norm1` 的**未定义**
# 引用。这条与 cpp 侧那句 `?norm1@Vec2@@QEBANXZ` 是对称的 —— 一边断「MSVC
# 成员 mangling 真的被引用」，一边断「稳定链接名真的被引用」，合起来才说明
# 「非泛型 record 方法能跨边界」不是某一份契约的巧合。
RUST_NORM_REF="$(grep -F 'vyx_vec2_norm1' "$RUST_CONSUMER_NM" | head -1 || true)"
[ -n "$RUST_NORM_REF" ] \
  || { echo "FAIL the rust consumer object does not reference vyx_vec2_norm1"
       cat "$RUST_CONSUMER_NM"; exit 1; }
case "$RUST_NORM_REF" in
  *" U "*) ;;
  *) echo "FAIL the rust consumer did not leave Vec2::norm1 undefined: $RUST_NORM_REF"
     exit 1 ;;
esac
echo "CONSUMER ok (rust fixture: object references $RUST_REF and $RUST_SWAP_REF, both undefined)"
echo "CONSUMER ok3 (rust fixture: object references $RUST_NORM_REF, undefined)"

# 负例：provider **不提供**的方法仍然必须被拒。
# 正例只证明「声明得对就能过」；这条证明「声明得不对仍然过不去」。少了它，一个
# 「把任何不可解析的声明都静默解析成某个符号」的回归会全绿。
#
# 必须用 `--emit=obj` 而不是 `--emit=mir`：mono 修好之后，MIR 这一层**见不到**
# 这个失败 —— 实例照旧被物化，失败要等 codegen 查契约符号时才发生。用 mir 跑
# 会得到 rc=0，门就静默变绿了。
#
# 期望失败**不能**写成裸命令：`set -euo pipefail` 下会在读到 $? 之前就退出，
# 断言从未被评估（门报 FAIL 却从没查过原因）。rc 单独取。
FIX_LOG="$(w "$PROJ")/target/missing_contract_method.log"
FIX_OBJ="$(w "$PROJ")/target/missing_contract_method.o"
if ( cd "$(w "$PROJ")" \
     && LLVM_ROOT="$LLVM_ROOT_WIN" "$BOOT" --src=file "src/unsupported/missing_contract_method.vyx" \
          --emit=obj -o "$FIX_OBJ" ) > "$FIX_LOG" 2>&1; then
  echo "FAIL a method the provider does not export now compiles"
  cat "$FIX_LOG"; exit 1
fi
if grep -qF 'unresolved call' "$FIX_LOG"; then
  echo "FAIL the fixture still fails in mono, not in the DCI symbol lookup"
  cat "$FIX_LOG"; exit 1
fi
grep -qF 'DCI Descriptor has no matching symbol for external declaration' "$FIX_LOG" \
  || { echo "FAIL the fixture did not report a DCI descriptor miss"; cat "$FIX_LOG"; exit 1; }
grep -qF 'member=tag, owner=Pair2::<f64,i32>)' "$FIX_LOG" \
  || { echo "FAIL the fixture did not blame the right member/owner"; cat "$FIX_LOG"; exit 1; }
FIX_ERRORS=$(grep -c ': error: ' "$FIX_LOG")
FIX_NOTES=$(grep -c ': note: ' "$FIX_LOG")
[ "$FIX_ERRORS" = "4" ] && [ "$FIX_NOTES" = "2" ] \
  || { echo "FAIL the fixture no longer fails with exactly 4 errors + 2 notes (got ${FIX_ERRORS}/${FIX_NOTES})"
       cat "$FIX_LOG"; exit 1; }
echo "BOUNDARY ok (a provider-less method is rejected in symbol lookup, not in mono)"

# 两个 target 各自的 .dci_open sidecar 必须逐字相同：两份 Vyx 源是同一套
# 调用点，闭合请求只由调用点实参决定，与谁去物化无关。这条断的是「同一套
# 调用点」这句话本身 —— 只数总数会放过「少闭合一个」，所以也断言行数下限。
# 5 个原有闭合（twice i32/f64、max_of i32、identity Vec2、swap i32,f64）
# + 5 个新组合的 swap 闭合 = 10，再加消费方声明记录带来的 3 条
# （identity<TestS>、swap<TestS,i32>、swap<TestF,i32>）= 13。
SIDECARS=()
for f in "$(w "$PROJ")"/.cache/*.obj.dci_open; do
  [ -f "$f" ] && SIDECARS+=("$f")
done
[ "${#SIDECARS[@]}" -eq 2 ] \
  || { echo "FAIL expected 2 .dci_open sidecars, got ${#SIDECARS[@]}"; exit 1; }
OPEN_LINES=$(grep -cs "^open" "${SIDECARS[0]}")
[ "$OPEN_LINES" -ge 13 ] \
  || { echo "FAIL sidecar has $OPEN_LINES requests, expected >=13"; cat "${SIDECARS[0]}"; exit 1; }
for SYM in swap__TestS_i32 swap__TestF_i32 identity__TestS; do
  grep -qsP "^open\t${SYM}\t" "${SIDECARS[0]}" \
    || { echo "FAIL sidecar lacks the consumer-declared closure $SYM"; cat "${SIDECARS[0]}"; exit 1; }
done
# 泛型实例方法的 `instance` 请求行：消费方发现的直接证据。降级缺一个实例就记
# 一行 `instance <type-text>`；记的正是**方法接收者** —— 6 个被调用了
# get_first/swapped 的 Pair2 实例（:::<consumer 拼写>）。反序实例只作返回
# 类型，不会被记 instance 行（它们的布局在契约里，方法没被逼出来）。
INSTANCE_LINES=$(grep -csP '^instance\t' "${SIDECARS[0]}" || true)
[ "$INSTANCE_LINES" -ge 6 ] \
  || { echo "FAIL sidecar records $INSTANCE_LINES instance requests, expected >=6"
       cat "${SIDECARS[0]}"; exit 1; }
grep -qsP '^instance\tPair2::<f64,i32>' "${SIDECARS[0]}" \
  || { echo "FAIL sidecar lacks the Pair2::<f64,i32> instance request"; cat "${SIDECARS[0]}"; exit 1; }
cmp -s "${SIDECARS[0]}" "${SIDECARS[1]}" \
  || { echo "FAIL the two producers were handed different instances"; \
       diff "${SIDECARS[0]}" "${SIDECARS[1]}"; exit 1; }

# 两份契约各服务一个 target，**互不比对**：它们描述的是两个不同产物。
echo "REQUESTS identical (both producers were handed the same $((OPEN_LINES)) closures)"

# ============ 消费方声明的 @[repr(C)] 记录作类型实参 ============
# 契约里没有 `TestS`，instance 请求清单里也没有 `Pair2<i32,TestS>` ——
# 这个实例完全是消费方在调用点闭合出来的。要让生产端（rustc / clang++）能
# 物化它，请求文件必须同时带上两样东西：
#   `record` 让生产端**真的定义**这个类型（否则它连实例的名字都拼不出来）；
#   `layout` 把消费方**推导**出来的实例布局交回生产端。
# 只有消费方能推：`@[repr(C)]` 让记录的布局就是 C 布局，于是 `Pair2<i32,TestS>`
# 的字段偏移可由字段序列算出（first@0、second@4、size 12、align 4）。
grep -qsP '^record\tTestS\t8\t4\twidth:u32:0,height:u32:4$' "${SIDECARS[0]}" \
  || { echo "FAIL sidecar lacks the derived TestS record definition"; cat "${SIDECARS[0]}"; exit 1; }
grep -qsP '^layout\tPair2<i32,TestS>\t12\t4$' "${SIDECARS[0]}" \
  || { echo "FAIL sidecar lacks the derived Pair2<i32,TestS> layout"; cat "${SIDECARS[0]}"; exit 1; }
grep -qsP '^record\tTestF\t16\t8\tv:Vec2:0$' "${SIDECARS[0]}" \
  || { echo "FAIL sidecar lacks the derived TestF record definition (nested DCI record)"; cat "${SIDECARS[0]}"; exit 1; }
grep -qsP '^layout\tPair2<i32,TestF>\t24\t8$' "${SIDECARS[0]}" \
  || { echo "FAIL sidecar lacks the derived Pair2<i32,TestF> layout"; cat "${SIDECARS[0]}"; exit 1; }

# 「消费方推导」之所以诚实，是因为生产端会把同一个数字**实测**回来：生成的
# shim 源码里带 `#[repr(C)]` 定义 + size/align/offset 断言，对不上即编译失败。
# 所以断言真的出现在两边源码里，而不是只信「构建成功」。
# rust 側的记录**不在** .rs 里：`mod provider;` 指向的 `<stub>.rs_provider.rs`
# 才是 provider 源码 + 记录定义合并出来的那个文件（记录必须和它要引用的
# provider 类型同处一个 module，所以只能并进 provider 文件）。
RUST_SHIMS=()
for one in "$(w "$PROJ")"/.cache/*dci_stubs_external.rs_provider.rs; do
  [ -f "$one" ] && RUST_SHIMS+=("$one")
done
[ "${#RUST_SHIMS[@]}" -ge 1 ] \
  || { echo "FAIL no rust provider source to inspect"; ls "$(w "$PROJ")"/.cache/ | head -20; exit 1; }
for one in "${RUST_SHIMS[@]}"; do
  grep -qF 'pub struct TestS' "$one" \
    || { echo "FAIL the rust shim does not define the consumer record TestS"; exit 1; }
  grep -qF 'size_of::<Pair2<i32,TestS>>() == 12' "$one" \
    || { echo "FAIL the rust shim does not assert the derived instance layout"; exit 1; }
done
CPP_SHIMS=()
for one in "$(w "$PROJ")"/.cache/*dci_stubs_external.cpp; do
  [ -f "$one" ] && CPP_SHIMS+=("$one")
done
[ "${#CPP_SHIMS[@]}" -ge 1 ] \
  || { echo "FAIL no cpp shim source to inspect"; ls "$(w "$PROJ")"/.cache/ | head -20; exit 1; }
for one in "${CPP_SHIMS[@]}"; do
  grep -qF 'struct TestS {' "$one" \
    || { echo "FAIL the cpp shim does not define the consumer record TestS"; exit 1; }
  grep -qF 'sizeof(Pair2<int,TestS>) == 12' "$one" \
    || { echo "FAIL the cpp shim does not assert the derived instance layout"; exit 1; }
done
echo "CONSUMER-RECORD ok (consumer-declared @[repr(C)] records crossed the boundary on"
echo "                    both producers; the derived layouts were measured back)"

echo "== run both consumer exes =="
for NAME in dci_opengeneric dci_opengeneric_cpp; do
  EXE="$(w "$PROJ/target/$NAME.exe")"
  [ -f "$EXE" ] || { echo "FAIL exe missing: $EXE"; exit 1; }
  RUN_LOG="$(w "$PROJ/target/$NAME.run.log")"
  if ! "$EXE" > "$RUN_LOG" 2>&1; then
    echo "FAIL $NAME run rc=$?"; cat "$RUN_LOG"; exit 1
  fi
  grep -qx "dci_opengeneric OK" "$RUN_LOG" \
    || { echo "FAIL unexpected $NAME output"; cat "$RUN_LOG"; exit 1; }
  echo "RUN ok ($NAME): $(cat "$RUN_LOG")"
done

# ============ 端到端：泛型 record 的成员函数真的被调用过 ============
# 「跑通」有两种：调用点真的过边界，或者编译器把这条调用**优化没了**/走错了
# 实现。钉住 `vyxc build` 真编出来的 cpp target 对象：它必须留下一条对
# `?get_first@?$Pair2@NH@@QEBANXZ` 的**未定义**引用 —— 未定义才说明这个名字是
# 由 provider 的 shim 对象兑现的，而不是消费方自己编了一个。
CONSUMER_OBJS=()
for one in "$(w "$PROJ")"/.cache/crate_dci_opengeneric_cpp*.obj; do
  [ -f "$one" ] && CONSUMER_OBJS+=("$one")
done
[ "${#CONSUMER_OBJS[@]}" -ge 1 ] \
  || { echo "FAIL no cpp consumer object to inspect; the nm check would be vacuous"
       ls "$(w "$PROJ")"/.cache/ | head -20; exit 1; }
TARGET_NM="$(mktemp -d)/target_consumer.nm"
: > "$TARGET_NM"
for one in "${CONSUMER_OBJS[@]}"; do "$NM" "$one" >> "$TARGET_NM" 2>&1; done
TARGET_LINES=$(wc -l < "$TARGET_NM")
[ "$TARGET_LINES" -ge 4 ] \
  || { echo "FAIL llvm-nm read only $TARGET_LINES line(s) from ${#CONSUMER_OBJS[@]} target object(s)"
       cat "$TARGET_NM"; exit 1; }
TARGET_REF="$(grep -F '?get_first@?$Pair2@NH@@QEBANXZ' "$TARGET_NM" | head -1 || true)"
[ -n "$TARGET_REF" ] \
  || { echo "FAIL the built cpp target does not reference Pair2<double,int>::get_first"
       echo "     (inspected ${#CONSUMER_OBJS[@]} object(s), $TARGET_LINES symbol line(s))"
       cat "$TARGET_NM"; exit 1; }
case "$TARGET_REF" in
  *" U "*) ;;
  *) echo "FAIL the target left the instance method defined instead of undefined: $TARGET_REF"
     exit 1 ;;
esac
echo "BOUNDARY ok (the built cpp target references $TARGET_REF)"

echo "== env still overrides the manifest =="
OVR_LOG="$(w "$PROJ/target/env_override.log")"
if ( cd "$(w "$PROJ")" \
     && VYX_DCI_STUB_BACKEND_TOOL="vyx_no_such_tool_xyz" "$BOOT" build -j 4 ) \
     > "$OVR_LOG" 2>&1; then
  echo "FAIL env override did not take effect (build succeeded anyway)"
  tail -20 "$OVR_LOG"; exit 1
fi
grep -qs "vyx_no_such_tool_xyz" "$OVR_LOG" \
  || { echo "FAIL env override failed for an unrelated reason"; tail -20 "$OVR_LOG"; exit 1; }
echo "OVERRIDE ok (env tool wins over Vyx.toml)"

echo "PASS dci_opengeneric (vyx consumer, rust + cpp producers)"
