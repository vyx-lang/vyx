<script setup>
/*
 * 教程分节的图示。全部由页面时钟驱动：--motion-progress 走连续位移，phase 走离散步骤，
 * 所以暂停或减少动效时整页图示一起停下，不会各自为政。
 * 图示只解释机制，不冒充实测输出；数字都来自对应课程里的示例。
 */
import { computed } from 'vue'

const props = defineProps({
  name: { type: String, required: true },
  locale: { type: String, default: 'zh' },
  phase: { type: Number, default: 0 },
})

const isZh = computed(() => props.locale === 'zh')
const pick = value => (isZh.value ? value.zh : value.en)

const figures = {
  pipeline: {
    label: 'SOURCE → NATIVE',
    caption: {
      zh: '源码经解析、类型与所有权检查，再由 LLVM 生成目标代码；自举编译器本身也用 Vyx 写成。',
      en: 'Source is parsed, checked for types and ownership, then lowered by LLVM. The compiler itself is written in Vyx.',
    },
    nodes: ['Source', 'Parse', 'Analyze', 'LLVM', 'Native'],
    details: ['.vyx', 'AST', 'HIR / MIR', 'IR', 'exe'],
  },
  result: {
    label: 'RESULT → CALLER',
    caption: {
      zh: '成功与失败都在返回值里；? 遇到错误立刻交给调用方，每一层都可以选择处理或继续上抛。',
      en: 'Success and failure both travel in the return value; ? hands an error to the caller, which may handle it or pass it on.',
    },
  },
  containers: {
    label: 'PUSH → GROW → RELEASE',
    caption: {
      zh: 'Vec 先按容量存放，写满才重新分配更大的块并搬移元素；用完显式调用 destroy()。',
      en: 'A Vec fills up to its capacity, then reallocates a larger block and moves the elements. Release is explicit: destroy().',
    },
  },
  ownership: {
    label: 'MOVE / BORROW / DROP',
    caption: {
      zh: '同一时刻只有一个所有者；借用是临时的只读或可写视图；离开作用域时释放。搬走后不要再碰源变量。',
      en: 'One owner at a time; a borrow is a temporary view; the owner releases at the end of the scope. Never touch the source after a move.',
    },
  },
  languages: {
    label: 'RUST / C++ → VYX',
    caption: {
      zh: '迁移不是重写：原来的类型、库与 ABI 通过 DCI 契约继续使用，不需要手写绑定层。',
      en: 'Migration is not a rewrite: existing types, libraries, and ABIs stay reachable through DCI contracts.',
    },
    rows: [
      ['let mut x', 'int x', 'var x'],
      ['match x {}', 'switch (x)', 'match x {}'],
      ['Result<T, E>', 'std::expected', 'Result<T, E>'],
      ['Arc<T>', 'shared_ptr<T>', 'Ref<T>'],
      ['async / await', 'coroutine', 'async / Task'],
      ['extern "C"', 'extern "C"', 'extern "C"'],
    ],
  },
}

const space = computed(() => figures[props.name] ?? figures.pipeline)
const order = ['pipeline', 'result', 'containers', 'ownership', 'languages', 'loop']
const index = computed(() => Math.max(0, order.indexOf(props.name)))

/* 与第五课的示例一致：0、1、3、4 累加，2 被 continue 跳过，5 触发 break。 */
const loopSteps = [
  { value: 0, state: 'add' },
  { value: 1, state: 'add' },
  { value: 2, state: 'skip' },
  { value: 3, state: 'add' },
  { value: 4, state: 'add' },
  { value: 5, state: 'stop' },
]
const total = computed(() => loopSteps.reduce((sum, step, i) => (
  step.state === 'add' && props.phase > i ? sum + step.value : sum
), 0))
</script>

<template>
  <figure class="tutorial-figure" :data-figure="name">
    <figcaption class="figure-head"><span>{{ space.label }}</span><span v-if="name !== 'loop'">FIG. 0{{ index + 1 }}</span><span v-else>FIG. 06</span></figcaption>

    <ol v-if="name === 'pipeline'" class="pipe-track">
      <li v-for="(node, i) in space.nodes" :key="node" :style="{ '--i': i }" :class="{ lit: phase === i, past: phase > i }">
        <span class="pipe-number">0{{ i + 1 }}</span><strong>{{ node }}</strong><code>{{ space.details[i] }}</code>
      </li>
    </ol>

    <div v-else-if="name === 'result'" class="result-stack">
      <div class="result-frame top"><span>main</span><code>{{ phase < 4 ? '43' : 'InvalidNumber' }}</code></div>
      <div class="result-lane ok" :class="{ lit: phase < 4 }"><span>Ok(43)</span><i><b></b></i><small>{{ pick({ zh: '正常继续', en: 'carries on' }) }}</small></div>
      <div class="result-frame middle"><span>parse_next</span><code>?</code></div>
      <div class="result-lane err" :class="{ lit: phase >= 4 }"><span>Err(InvalidNumber)</span><i><b></b></i><small>{{ pick({ zh: '直接返回调用方', en: 'back to the caller' }) }}</small></div>
      <div class="result-frame bottom"><span>parse_number</span><code>"42"</code></div>
    </div>

    <div v-else-if="name === 'containers'" class="containers">
      <div class="capacity"><span>{{ pick({ zh: '容量', en: 'CAPACITY' }) }}</span><code>8</code></div>
      <div class="cells">
        <span v-for="i in 8" :key="i" :style="{ '--i': i - 1 }" :class="{ filled: phase * .75 >= i - 1, spare: i > 6 }"><template v-if="i <= 6">{{ i }}</template></span>
      </div>
      <div class="capacity-foot"><span>{{ pick({ zh: '长度 6 / 容量 8', en: 'len 6 / cap 8' }) }}</span><span>{{ pick({ zh: '写满时重新分配并搬移', en: 'reallocates and moves when full' }) }}</span></div>
    </div>

    <div v-else-if="name === 'ownership'" class="ownership">
      <div class="owner"><span>a</span><code>Ref&lt;T&gt;</code><small :class="{ off: phase >= 3 }">{{ phase >= 3 ? pick({ zh: '已搬走', en: 'moved' }) : pick({ zh: '持有', en: 'owns' }) }}</small></div>
      <div class="owner-link" :class="{ lit: phase >= 3 }"><i></i><span>{{ pick({ zh: '移动', en: 'move' }) }}</span><i></i></div>
      <div class="owner" :class="{ active: phase >= 1 }"><span>b</span><code>Ref&lt;T&gt;</code><small>{{ phase >= 3 ? pick({ zh: '持有', en: 'owns' }) : pick({ zh: '未拿到', en: 'not yet' }) }}</small></div>
      <div class="borrow" :class="{ lit: phase === 1 || phase === 2 }"><i></i><span>{{ pick({ zh: '借用 &amp;T / &amp;mut T', en: 'borrow &T / &mut T' }) }}</span></div>
      <div class="drop" :class="{ lit: phase >= 5 }"><i></i><span>{{ pick({ zh: '作用域结束 → 释放', en: 'scope ends → drop' }) }}</span></div>
    </div>

    <div v-else-if="name === 'loop'" class="loop">
      <ol class="loop-cells">
        <li v-for="(step, i) in loopSteps" :key="i" :style="{ '--i': i }" :class="[step.state, { lit: phase >= i, skipped: step.state === 'skip' && phase >= i }]">
          <span>{{ i }}</span><code>{{ step.state === 'skip' ? 'continue' : step.state === 'stop' ? 'break' : '+' + step.value }}</code>
        </li>
      </ol>
      <div class="loop-readout"><span>{{ pick({ zh: '累加结果', en: 'ACCUMULATED' }) }}</span><code>{{ total }}</code><small>0+1+3+4 = 8</small></div>
    </div>

    <div v-else class="languages">
      <div class="lang-head"><span>Rust</span><span>C++</span><span>Vyx</span></div>
      <ul><li v-for="(row, i) in space.rows" :key="row[0]" :style="{ '--i': i }" :class="{ lit: phase === i }"><code>{{ row[0] }}</code><code>{{ row[1] }}</code><code>{{ row[2] }}</code></li></ul>
    </div>

    <p class="figure-note">{{ pick(space.caption) }}</p>
  </figure>
</template>
