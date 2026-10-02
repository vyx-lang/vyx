<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { ArrowUpRight, Check, Code2, Copy, RotateCcw, ShieldCheck, Terminal } from 'lucide-vue-next'
import { features } from './features'

const props = defineProps({ locale: String, copied: String, paused: Boolean })
const emit = defineEmits(['copy'])
const active = ref(0)
defineExpose({ selectFeature: index => { active.value = index } })
const selectedFile = ref(0)
const selectedCase = ref(0)
const selectedProvider = ref(0)
const feature = computed(() => features[active.value])
const dciCase = computed(() => feature.value.cases?.[selectedCase.value])
const provider = computed(() => dciCase.value?.providers[selectedProvider.value])
const files = computed(() => provider.value?.files || feature.value.files)
const file = computed(() => files.value[selectedFile.value])
const area = ref(null)
const phase = ref(0)
const flow = computed(() => ({ migrate: ['1.0.0', '@migrate', '2.0.0'], reflection: ['Type', 'Method', 'Invoke'], dci: [provider.value?.label || 'Native', '.dcib', 'Vyx'], dce: ['Roots', 'Reachable', 'Native'] })[feature.value.id])
const phaseNames = computed(() => props.locale === 'zh'
  ? ['收集程序', '标记根与副作用', '分析可达性', '裁剪与生成']
  : ['Collect', 'Mark roots & effects', 'Trace reachability', 'Eliminate & emit'])
const nodes = [
  { name: 'main', role: 'entry', keep: true },
  { name: 'public API', role: 'export', keep: true },
  { name: 'WelcomeCommand.run', role: 'reflection root', keep: true },
  { name: 'DCI symbol', role: 'native boundary', keep: true },
  { name: 'drop / side effect', role: 'observable', keep: true },
  { name: 'unused helper', role: 'unreachable', keep: false },
  { name: 'dead branch', role: 'unreachable', keep: false },
]
let timer, observer, media, visible = false
const escape = text => text.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;')
function highlight(line) {
  const tokens = /\/\/.*$|"(?:[^"\\]|\\.)*"|(?:@|#)!?\[[a-zA-Z_]+|#pragma\b|\b(?:fn|let|var|return|use|extern|struct|class|module|public|private|mut|if|else|true|false|self|pub|impl|where|template|typename|const|noexcept|virtual|override|unsafe|trait|dyn|as|explicit)\b|\b(?:i32|i64|f64|bool|void|double|int|T|U|A|B|Vec2|Pair2|Copy|PartialOrd|WelcomeCommand|AbstractSink|NativeDriver|VyxSink|VyxHost|Sink)\b|\b\d+(?:\.\d+)?\b/g
  let html = '', end = 0
  for (const match of line.matchAll(tokens)) {
    html += escape(line.slice(end, match.index))
    const value = match[0]
    const kind = value.startsWith('//') ? 'comment' : value.startsWith('"') ? 'string'
      : /^[#@]/.test(value) ? 'attribute' : /^\d/.test(value) ? 'number'
        : /^(i32|i64|f64|bool|void|double|int|T|U|A|B|Vec2|Pair2|Copy|PartialOrd|WelcomeCommand|AbstractSink|NativeDriver|VyxSink|VyxHost|Sink)$/.test(value) ? 'type' : 'keyword'
    html += `<span class="token-${kind}">${escape(value)}</span>`
    end = match.index + value.length
  }
  return html + escape(line.slice(end))
}
function onTabsKey(event) {
  if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.key)) return
  event.preventDefault()
  active.value = event.key === 'Home' ? 0 : event.key === 'End' ? features.length - 1
    : (active.value + (['ArrowRight', 'ArrowDown'].includes(event.key) ? 1 : -1) + features.length) % features.length
  nextTick(() => document.getElementById('tab-' + features[active.value].id)?.focus())
}
function sync() {
  clearInterval(timer)
  if (media?.matches) { phase.value = 3; return }
  if (feature.value.id !== 'dce' || !visible || document.hidden || props.paused) return
  timer = setInterval(() => { phase.value = (phase.value + 1) % 4 }, 2400)
}
function replay() { phase.value = 0; sync() }
watch(active, () => { selectedFile.value = 0; selectedCase.value = 0; selectedProvider.value = 0; phase.value = 0; sync() })
watch(selectedCase, () => { selectedFile.value = 0; selectedProvider.value = 0 })
watch(selectedProvider, () => { selectedFile.value = 0 })
watch(() => props.paused, sync)
onMounted(() => {
  media = matchMedia('(prefers-reduced-motion: reduce)')
  media.addEventListener('change', sync)
  document.addEventListener('visibilitychange', sync)
  observer = new IntersectionObserver(([entry]) => { visible = entry.isIntersecting; sync() }, { threshold: .15 })
  observer.observe(area.value)
})
onBeforeUnmount(() => { clearInterval(timer); observer?.disconnect(); media?.removeEventListener('change', sync); document.removeEventListener('visibilitychange', sync) })
</script>

<template>
  <section id="playground" ref="area" class="code-section core-features" :data-feature="feature.id">
    <div class="section-width section-label feature-section-label"><span>03 / MOSP</span><a href="./mosp.html">{{ locale === 'zh' ? '语言机制与完整示例' : 'Language architecture and examples' }}<ArrowUpRight :size="14" /></a></div>
    <div class="section-width code-layout">
      <div class="code-intro reveal">
        <p class="micro-label">{{ locale === 'zh' ? '语法与编译机制' : 'SYNTAX AND COMPILATION' }}</p>
        <h2>{{ locale === 'zh' ? '四项核心特性' : 'Four core features' }}</h2>
        <div class="code-tabs" role="tablist" :aria-label="locale === 'zh' ? '四个核心特性' : 'Four core features'" @keydown="onTabsKey">
          <button v-for="(item, index) in features" :id="'tab-' + item.id" :key="item.id" role="tab" :aria-selected="active === index" aria-controls="feature-panel" :tabindex="active === index ? 0 : -1" :class="{ selected: active === index }" @click="active = index"><span class="feature-tab-index">0{{ index + 1 }}</span><span>{{ item.label }}</span><ArrowUpRight :size="16" /></button>
        </div>
        <Transition name="feature-description" mode="out-in">
          <div :key="feature.id" class="feature-explainer"><h3>{{ feature.title[locale] }}</h3><p>{{ feature.description[locale] }}</p></div>
        </Transition>
        <div v-if="feature.cases" class="dci-case-list" role="group" :aria-label="locale === 'zh' ? 'DCI 三个用例' : 'Three DCI cases'">
          <button v-for="(item, index) in feature.cases" :key="item.id" :class="{ selected: selectedCase === index }" :aria-pressed="selectedCase === index" @click="selectedCase = index"><span class="dci-case-index">0{{ index + 1 }}</span><span><strong>{{ item.label[locale] }}</strong><small>{{ item.detail[locale] }}</small></span><ArrowUpRight :size="14" /></button>
        </div>
        <div class="feature-flow" :aria-label="flow.join(' → ')"><template v-for="(label, index) in flow" :key="feature.id + label"><span :style="{ '--flow-step': index }">{{ label }}</span><i v-if="index < 2" aria-hidden="true"></i></template></div>
        <span class="code-footnote">{{ feature.footnote[locale] }}</span>
      </div>

      <div id="feature-panel" class="editor-wrap reveal" role="tabpanel" :aria-labelledby="'tab-' + feature.id" tabindex="0">
        <Transition name="feature-panel" mode="out-in">
          <div v-if="file" :key="feature.id" class="editor feature-editor">
            <div class="editor-bar"><div class="window-dots" aria-hidden="true"><i></i><i></i><i></i></div><span>{{ feature.label }} <span class="editor-kind">/ {{ file.language }}</span></span><button class="icon-button copy-code" :aria-label="locale === 'zh' ? '复制当前文件代码' : 'Copy current file'" @click="emit('copy', file.code)"><Check v-if="copied === 'feature'" :size="16" /><Copy v-else :size="16" /></button></div>
            <div v-if="provider" class="dci-provider-bar"><div v-if="dciCase.providers.length > 1" class="provider-toggle" role="group" :aria-label="locale === 'zh' ? '原始定义语言' : 'Producer language'"><button v-for="(item, index) in dciCase.providers" :key="item.id" :aria-pressed="selectedProvider === index" :class="{ selected: selectedProvider === index }" @click="selectedProvider = index">{{ item.label }}</button></div><span v-else class="provider-single">{{ provider.label }} / {{ dciCase.label[locale] }}</span><span class="provider-contract">{{ provider.compiler }}<span class="provider-arrow">→</span>{{ provider.contract }}<span class="provider-arrow">→</span>Vyx</span></div>
            <div class="source-files" role="group" :aria-label="locale === 'zh' ? '示例文件' : 'Example files'"><button v-for="(item, index) in files" :key="item.name + index" :aria-pressed="selectedFile === index" :class="{ selected: selectedFile === index }" @click="selectedFile = index"><span v-if="item.role" class="source-step">0{{ index + 1 }}</span><Code2 v-else :size="12" /><span>{{ item.name }}<small v-if="item.role">{{ item.role[locale] }}</small></span></button></div>
            <Transition name="code-swap" mode="out-in"><div :key="(dciCase?.id || '') + (provider?.id || '') + file.name" class="editor-code" tabindex="0" :aria-label="file.name"><div v-for="(line, index) in file.code.split('\n')" :key="index" class="code-line" :style="{ '--line': Math.min(index, 12) }"><span class="line-number" aria-hidden="true">{{ index + 1 }}</span><code v-html="highlight(line) || ' '"></code></div></div></Transition>
            <div class="editor-output"><span><Terminal :size="13" />{{ locale === 'zh' ? '预期结果' : 'Expected result' }}</span><code>{{ dciCase?.output || feature.output }}</code></div>
            <div class="editor-status"><span><span class="status-dot"></span>{{ provider ? provider.contract : feature.label }}</span><span>{{ selectedFile + 1 }} / {{ files.length }}<span>{{ file.language.toUpperCase() }} SOURCE</span></span></div>
          </div>

          <div v-else key="dce" class="editor dce-panel" :data-phase="phase">
            <div class="editor-bar"><div class="window-dots" aria-hidden="true"><i></i><i></i><i></i></div><span>DCE <span class="editor-kind">/ COMPILE TIME</span></span><button class="icon-button replay-dce" :aria-label="locale === 'zh' ? '重播编译期流程' : 'Replay compiler flow'" @click="replay"><RotateCcw :size="15" /></button></div>
            <div class="dce-steps" :aria-label="locale === 'zh' ? '流程阶段' : 'Compiler stages'"><button v-for="(name, index) in phaseNames" :key="name" :class="{ current: phase === index, complete: phase > index }" :aria-pressed="phase === index" @click="phase = index"><span>{{ index + 1 }}</span>{{ name }}</button></div>
            <div class="dce-graph"><div class="dce-scan" aria-hidden="true"></div><div v-for="(node, index) in nodes" :key="node.name" class="dce-symbol" :class="{ kept: node.keep, removed: !node.keep }" :style="{ '--node-index': index }"><span class="symbol-status"><Check v-if="node.keep" :size="12" /><span v-else>−</span></span><code>{{ node.name }}</code><span class="symbol-role">{{ node.role }}</span></div></div>
            <div class="dce-result"><ShieldCheck :size="18" /><div><strong>{{ locale === 'zh' ? phase === 3 ? '可观察行为，完整保留。' : '分析边界，追踪依赖。' : phase === 3 ? 'Observable behavior stays intact.' : 'Trace dependencies and boundaries.' }}</strong><p>{{ locale === 'zh' ? '入口、导出、反射 / DCI 保留根与必要副作用。' : 'Entry, exports, reflection / DCI roots, and required side effects.' }}</p></div><span class="dce-count">{{ phase === 3 ? '7 → 5' : '7' }}</span></div>
            <div class="editor-status"><span><span class="status-dot"></span>{{ phaseNames[phase] }}</span><span>{{ locale === 'zh' ? '流程示意' : 'FLOW ILLUSTRATION' }}</span></div>
          </div>
        </Transition>
        <div class="editor-caption"><span>{{ feature.id === 'dce' ? locale === 'zh' ? '入口与依赖决定代码的保留范围。' : 'Entries and dependencies determine which code remains.' : locale === 'zh' ? '定义、声明与调用' : 'Definitions, declarations, and calls' }}</span><a v-if="dciCase" :href="`https://github.com/vyx-lang/vyx/tree/HEAD/${dciCase.source}`" target="_blank" rel="noopener noreferrer">{{ locale === 'zh' ? '完整示例' : 'Complete example' }}<ArrowUpRight :size="14" /></a><Code2 v-else :size="15" /></div>
      </div>
    </div>
  </section>
</template>
