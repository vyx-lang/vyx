<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import {
  ArrowUpRight, ArrowDown, ArrowRight, Check, CheckCheck,
  Copy, ExternalLink, Github, Layers3,
  Menu, Moon, Sun, Pause, Play, X,
} from 'lucide-vue-next'
import { messages, repository, releases, downloads } from './content'
import FeatureShowcase from './FeatureShowcase.vue'
import TechnicalGlyph from './TechnicalGlyph.vue'
import { usePageMotion } from './usePageMotion'

const logo = import.meta.env.BASE_URL + 'vyx.png'
const readPreference = (key, fallback, allowed) => {
  try {
    const value = localStorage.getItem(key)
    return allowed.includes(value) ? value : fallback
  } catch { return fallback }
}
const locale = ref(readPreference('vyx-v2-language', 'zh', ['en', 'zh']))
const theme = ref(readPreference('vyx-v2-theme', 'light', ['dark', 'light']))
const t = computed(() => messages[locale.value])
/* 教程已经独立成站（/tutorial/），站内所有教程入口都指过去。 */
const tutorialUrl = './tutorial/'
const mospUrl = './mosp.html'
const learningLinks = computed(() => locale.value === 'zh' ? [
  { title: '语言教程', description: '从第一个程序到高级特性，四章课程，逐课讲清为什么。', href: tutorialUrl },
  { title: '高级特性', description: '所有权、异步、trait、comptime 与运行时反射。', href: './tutorial/#advanced' },
  { title: '从 Rust / C++ 迁移', description: '按图索骥对照语法、内存与构建方式。', href: './tutorial/#migration' },
] : [
  { title: 'Language tutorial', description: 'Four chapters, from the first program to advanced features, each lesson explaining why.', href: tutorialUrl },
  { title: 'Advanced features', description: 'Ownership, async, traits, comptime, and runtime reflection.', href: './tutorial/#advanced' },
  { title: 'Coming from Rust or C++', description: 'Map syntax, memory, and builds onto Vyx side by side.', href: './tutorial/#migration' },
])
const menuOpen = ref(false)
const pageRoot = ref(null)
const { paused: motionPaused, state: motionState, phase: compilePhase } = usePageMotion(pageRoot)
const copied = ref('')
const copyError = ref(false)
const dialog = ref(null)
const featureShowcase = ref(null)
const command = 'vyxc --src=file hello.vyx --run=aot'
const hello = 'fn main() -> i32 {\n    print("Hello, Vyx!");\n    return 0;\n}'
let copyTimeout
let returnFocus
function syncPreferences() {
  document.documentElement.dataset.theme = theme.value
  document.documentElement.lang = locale.value === 'zh' ? 'zh-CN' : 'en'
  document.title = locale.value === 'zh' ? 'Vyx 编程语言' : 'Vyx programming language'
  document.querySelector('meta[name="theme-color"]').content = getComputedStyle(document.documentElement).getPropertyValue('--bg').trim()
  try {
    localStorage.setItem('vyx-v2-language', locale.value)
    localStorage.setItem('vyx-v2-theme', theme.value)
  } catch { /* Preferences are optional in private browsing. */ }
}
watch([theme, locale], syncPreferences, { immediate: true })

async function copyText(text, id) {
  copyError.value = false
  try {
    await navigator.clipboard.writeText(text)
    copied.value = id
    clearTimeout(copyTimeout)
    copyTimeout = setTimeout(() => { copied.value = '' }, 2200)
  } catch {
    copied.value = ''
    copyError.value = true
  }
}
async function openModal() {
  menuOpen.value = false
  returnFocus = document.activeElement
  await nextTick()
  dialog.value.showModal()
  document.body.classList.add('modal-open')
}
function closeModal() { dialog.value?.close() }
function onDialogClose() {
  document.body.classList.remove('modal-open')
  returnFocus?.focus()
}
function backdropClose(event) {
  if (event.target !== dialog.value) return
  const rect = dialog.value.getBoundingClientRect()
  if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) closeModal()
}
function escapeMenu(event) { if (event.key === 'Escape') menuOpen.value = false }

onMounted(() => {
  window.addEventListener('keydown', escapeMenu)
})
onBeforeUnmount(() => {
  clearTimeout(copyTimeout)
  document.body.classList.remove('modal-open')
  window.removeEventListener('keydown', escapeMenu)
})
</script>

<template>
  <div ref="pageRoot" class="site-shell" :data-motion="motionState" :class="{ 'locale-zh': locale === 'zh', 'motion-paused': motionState !== 'running' }">
    <div class="page-atmosphere" aria-hidden="true"></div>
    <a class="skip-link" href="#main">{{ t.skip }}</a>
    <header class="site-header">
      <nav class="navigation" aria-label="Main navigation">
        <a class="brand brand-icon" href="#" aria-label="Vyx home" @click="menuOpen = false"><img :src="logo" class="brand-mark" width="58" height="58" alt="" /><span class="brand-label">VYX</span></a>
        <div class="desktop-links"><a href="#language">{{ t.nav[0] }}</a><a :href="tutorialUrl">{{ locale === 'zh' ? '教程' : 'Tutorial' }}</a><a href="#playground">{{ locale === 'zh' ? '核心特性' : 'Core features' }}</a><a :href="mospUrl">MOSP / DCI<ArrowUpRight :size="13" /></a><button @click="openModal()">{{ t.docs }}<ArrowUpRight :size="13" /></button></div>
        <div class="nav-actions">
          <button class="locale-button" :aria-label="t.language" @click="locale = locale === 'en' ? 'zh' : 'en'">{{ locale === 'en' ? '中' : 'EN' }}</button>
          <button class="icon-button theme-button" :aria-label="t.theme" @click="theme = theme === 'dark' ? 'light' : 'dark'"><Sun v-if="theme === 'dark'" :size="17" /><Moon v-else :size="17" /></button>
          <button class="icon-button motion-control" :aria-pressed="motionPaused" :aria-label="locale === 'zh' ? motionPaused ? '恢复动效' : '暂停动效' : motionPaused ? 'Resume motion' : 'Pause motion'" @click="motionPaused = !motionPaused"><Play v-if="motionPaused" :size="15" /><Pause v-else :size="15" /></button>
          <a class="nav-github" :href="repository" target="_blank" rel="noopener noreferrer"><Github :size="16" /><span>GitHub</span><ArrowUpRight :size="14" /></a>
          <button class="icon-button mobile-menu-button" :aria-label="menuOpen ? t.close : t.menu" :aria-expanded="menuOpen" aria-controls="mobile-menu" @click="menuOpen = !menuOpen"><X v-if="menuOpen" :size="21" /><Menu v-else :size="21" /></button>
        </div>
      </nav>
      <div v-if="menuOpen" id="mobile-menu" class="mobile-menu glass"><a href="#language" @click="menuOpen = false">{{ t.nav[0] }}<ArrowUpRight :size="16" /></a><a :href="tutorialUrl" @click="menuOpen = false">{{ locale === 'zh' ? '教程' : 'Tutorial' }}<ArrowUpRight :size="16" /></a><a :href="mospUrl" @click="menuOpen = false">MOSP / DCI<ArrowUpRight :size="16" /></a><a href="#start" @click="menuOpen = false">{{ t.nav[2] }}<ArrowUpRight :size="16" /></a><button @click="openModal()">{{ t.docs }}<ArrowUpRight :size="16" /></button></div>
      <div class="reading-progress" aria-hidden="true"></div>
    </header>

    <main id="main">
      <section class="hero section-width">
        <div class="hero-composition">
          <div class="hero-heading">
            <p class="release-pill">VYX / {{ locale === 'zh' ? '系统编程语言' : 'SYSTEMS LANGUAGE' }}</p>
            <h1 class="hero-title" aria-label="Vyx — Systems, connected"><span>SYSTEMS,</span><span>CONNECTED.</span></h1>
            <p class="hero-definition">{{ locale === 'zh' ? '编译为原生程序。' : 'Compile to native code.' }}</p>
            <p class="hero-description">{{ locale === 'zh' ? 'Vyx 是一门系统编程语言。编译器用 Vyx 编写，后端使用 LLVM；通过 DCI 接入 C++ 与 Rust 库。' : 'A systems programming language with a compiler written in Vyx and an LLVM backend. Use C++ and Rust libraries through DCI.' }}</p>
            <div class="hero-buttons"><a :href="tutorialUrl" class="button button-primary">{{ locale === 'zh' ? '开始写 Vyx' : 'Start writing Vyx' }}<ArrowRight :size="15" /></a><a :href="releases" target="_blank" rel="noopener noreferrer" class="button button-text">{{ locale === 'zh' ? '下载 SDK' : 'Download SDK' }}<ArrowUpRight :size="15" /></a></div>
          </div>
          <div class="hero-art">
            <div class="art-heading"><span>VYX / NATIVE CODE</span><span>FIG. 01</span></div>
            <TechnicalGlyph />
            <div class="art-caption"><span class="art-caption-label">{{ locale === 'zh' ? '从语言到机器' : 'FROM LANGUAGE TO MACHINE' }}</span><span>COMPILE <i>→</i> LINK <i>→</i> RUN</span></div>
          </div>
        </div>
        <div class="hero-feature-index">
          <a v-for="(name, index) in ['Migrate', 'Reflection', 'DCI', 'DCE']" :key="name" href="#playground" @click="featureShowcase?.selectFeature(index)"><span class="index-number">0{{ index + 1 }}</span><div><strong>{{ name }}</strong><span>{{ (locale === 'zh' ? ['模块版本与迁移', '类型与成员反射', '原生跨语言接口', '编译期代码裁剪'] : ['Module versions & migration', 'Type & member reflection', 'Native language interfaces', 'Compile-time code elimination'])[index] }}</span></div><ArrowUpRight :size="15" /></a>
        </div>
        <div class="hero-baseline"><span>SELF-HOSTED / LLVM / MOSP</span><a href="#language" :aria-label="locale === 'zh' ? '探索语言特性' : 'Explore language features'"><ArrowDown :size="17" /></a><span>01 — VYX</span></div>
      </section>

      <section id="language" class="language-section section-space">
        <div class="section-width">
        <div class="section-label"><span>{{ locale === 'zh' ? '写代码与构建项目' : 'WRITING AND BUILDING' }}</span><span>Vyx / LLVM</span></div>
        <div class="language-layout">
          <div class="language-intro reveal"><span class="section-index" aria-hidden="true">01</span><h2>{{ locale === 'zh' ? '从源码，\n到原生程序。' : 'SOURCE IN.\nNATIVE OUT.' }}</h2><p>{{ locale === 'zh' ? '保存一个 .vyx 文件，用 vyxc 编译并运行。多模块工程通过 Vyx.toml 管理构建目标、依赖与原生库。' : 'Save a .vyx file, then compile and run it with vyxc. Vyx.toml defines targets, dependencies, and native libraries for larger projects.' }}</p><a :href="tutorialUrl" class="text-link">{{ locale === 'zh' ? '阅读语言教程' : 'Read the language tutorial' }}<ArrowRight :size="15" /></a></div>
          <div class="hello-example">
            <div class="hello-file"><span>hello.vyx</span><button class="icon-button" :aria-label="locale === 'zh' ? '复制 hello.vyx' : 'Copy hello.vyx'" @click="copyText(hello, 'hello')"><Check v-if="copied === 'hello'" :size="15" /><Copy v-else :size="15" /></button></div>
            <pre class="hello-source"><code><span class="syntax-keyword">fn</span> main() <span class="syntax-punctuation">-&gt;</span> <span class="syntax-type">i32</span> {
    print(<span class="syntax-string">"Hello, Vyx!"</span>);
    <span class="syntax-keyword">return</span> <span class="syntax-number">0</span>;
}</code></pre>
            <div class="hello-terminal"><span class="terminal-label">{{ locale === 'zh' ? '编译并运行' : 'Compile and run' }}</span><code><span class="terminal-prompt">$</span> {{ command }}</code><samp>Hello, Vyx!</samp></div>
            <div class="hello-note"><span>LLVM / AOT</span><span>hello.vyx → hello</span></div>
          </div>
        </div>
        </div>
      </section>

      <section class="compile-section section-width section-space">
        <div class="compile-heading"><p class="micro-label">02 / COMPILATION</p><h2>{{ locale === 'zh' ? '编译路径' : 'THE COMPILATION\nPIPELINE' }}</h2><p>{{ locale === 'zh' ? '解析源码，检查类型与所有权，再由 LLVM 生成目标代码。' : 'Parse the source, check types and ownership, then generate target code through LLVM.' }}</p></div>
        <ol class="compile-track" :data-phase="compilePhase" aria-label="Compilation pipeline">
          <li v-for="(stage, index) in [{ name: 'Source', detail: '.vyx' }, { name: 'Parse', detail: 'AST' }, { name: 'Analyze', detail: 'HIR / MIR' }, { name: 'LLVM', detail: 'IR' }, { name: 'Native', detail: 'Object / Executable' }]" :key="stage.name" class="compile-stage" :class="{ 'active-phase': compilePhase === index, complete: compilePhase > index }"><span class="compile-number">0{{ index + 1 }}</span><strong>{{ stage.name }}</strong><code>{{ stage.detail }}</code><ArrowRight v-if="index < 4" :size="16" /></li>
        </ol>
          <dl class="language-facts">
            <div><dt>LLVM AOT</dt><dd>{{ locale === 'zh' ? '输出原生可执行文件、对象文件与库。' : 'Generate native executables, object files, and libraries.' }}</dd></div>
            <div><dt>{{ locale === 'zh' ? '显式内存操作' : 'Explicit memory operations' }}</dt><dd>{{ locale === 'zh' ? '区分值、共享借用 &T、可变借用 &mut T 与指针。' : 'Distinguish values, shared &T and mutable &mut T borrows, and pointers.' }}</dd></div>
            <div><dt>{{ locale === 'zh' ? '自举编译器' : 'Self-hosted compiler' }}</dt><dd>{{ locale === 'zh' ? '前端、HIR/MIR 与构建系统由 Vyx 编写，源码在仓库中。' : 'The frontend, HIR/MIR, and build system are written in Vyx. Their source is in the repository.' }}</dd></div>
          </dl>
      </section>

      <FeatureShowcase ref="featureShowcase" :locale="locale" :copied="copied" :paused="motionPaused" @copy="code => copyText(code, 'feature')" />

      <section id="interop" class="interop-section section-width section-space">
        <div class="section-label"><span>04 / {{ locale === 'zh' ? '跨语言互操作' : 'INTEROPERABILITY' }}</span><span>DECLARATIVE CODE INTERFACE</span></div>
        <div class="interop-heading reveal"><h2>{{ locale === 'zh' ? '原生库，\n直接进入工程。' : 'NATIVE LIBRARIES.\nCONNECTED.' }}</h2><div class="interop-copy"><p>{{ locale === 'zh' ? '使用 std::vector<T> 管理容器，用 ICU 处理 Unicode 文本，调用 Cargo 库计算校验和。DCI 将原工具链的 ABI 信息带入 Vyx 项目。' : 'Use std::vector<T> for containers, ICU for Unicode text, and Cargo libraries for checksums. DCI brings the original toolchain’s ABI information into a Vyx project.' }}</p><a class="text-link" :href="mospUrl">{{ locale === 'zh' ? '了解 DCI' : 'Explore DCI' }}<ArrowRight :size="17" /></a></div></div>
        <div class="interop-canvas reveal">
          <div class="interop-journey" :data-phase="compilePhase" role="img" aria-label="C, C++ and Rust connect to Vyx through an ABI contract">
            <div class="producer-group"><span class="producer-word">C</span><span class="producer-word cpp">C++</span><span class="producer-word rust">Rust</span></div>
            <div class="flow-segment first" aria-hidden="true"><div class="flow-track"><span class="flow-packet"></span></div><ArrowRight :size="19" /></div>
            <div class="contract-node" :class="{ 'active-phase': compilePhase === 2 }"><Layers3 :size="32" stroke-width="1.5" /><strong>DCI</strong><span>{{ locale === 'zh' ? 'ABI 契约' : 'ABI CONTRACT' }}</span></div>
            <div class="flow-segment second" aria-hidden="true"><div class="flow-track"><span class="flow-packet"></span></div><ArrowRight :size="19" /></div>
            <div class="vyx-node" :class="{ 'active-phase': compilePhase === 4 }"><span>VYX</span></div>
          </div>
          <div class="interop-captions"><span>{{ locale === 'zh' ? '原语言实现' : 'ORIGINAL IMPLEMENTATION' }}</span><span>LAYOUT / SYMBOL / LIFETIME</span><span>{{ locale === 'zh' ? 'Vyx 原生程序' : 'VYX NATIVE PROGRAM' }}</span></div>
        </div>
      </section>

      <section id="start" class="start-section">
        <div class="start-content section-width">
          <div class="start-intro"><p class="micro-label">05 / {{ locale === 'zh' ? '开始使用' : 'GET STARTED' }}</p><h2>{{ locale === 'zh' ? '写下\n第一个程序。' : 'WRITE YOUR\nNEXT PROGRAM.' }}</h2><p>{{ locale === 'zh' ? 'Windows 与 Linux SDK 均内置 LLVM 后端。下载、解压并将 vyxc 加入 PATH，即可开始编译。' : 'Windows and Linux SDKs include the LLVM backend. Download, extract, and add vyxc to PATH to start compiling.' }}</p><div class="command-box"><span class="command-prompt">$</span><code>{{ command }}</code><button class="icon-button" :aria-label="copied === 'command' ? t.copied : t.commandCopy" @click="copyText(command, 'command')"><Check v-if="copied === 'command'" :size="16" /><Copy v-else :size="16" /></button></div><button class="button button-primary quickstart-button" @click="openModal()">{{ locale === 'zh' ? '安装步骤' : 'Installation steps' }}<ArrowRight :size="15" /></button></div>
          <div class="learning-links"><a v-for="link in learningLinks" :key="link.title" :href="link.href"><div><strong>{{ link.title }}</strong><p>{{ link.description }}</p></div><ArrowRight :size="17" /></a></div>
        </div>
      </section>
    </main>

    <footer class="site-footer section-width"><div class="footer-top"><a class="brand brand-wordmark" href="#" aria-label="Vyx home"><span>vyx</span></a><p>{{ locale === 'zh' ? 'Vyx 编程语言' : 'The Vyx programming language' }}</p><a :href="repository" target="_blank" rel="noopener noreferrer">GitHub<ArrowUpRight :size="14" /></a><button @click="openModal()">{{ t.docs }}<ArrowUpRight :size="14" /></button><a href="#" class="back-top" :aria-label="locale === 'zh' ? '返回顶部' : 'Back to top'"><ArrowUpRight :size="18" /></a></div><div class="footer-bottom"><span>© {{ new Date().getFullYear() }} Vyx Language</span><span><span class="status-dot"></span>{{ t.footerStatus }}</span><span>Vyx Language Project</span></div></footer>
    <div class="toast" :class="{ visible: copied || copyError }" role="status" aria-live="polite"><CheckCheck v-if="copied" :size="16" />{{ copyError ? t.copyFail : t.copied }}</div>
    <dialog ref="dialog" class="docs-dialog" aria-labelledby="dialog-title" @close="onDialogClose" @click="backdropClose">
      <div class="dialog-head"><span class="dialog-brand">VYX / QUICK START</span><button class="icon-button" :aria-label="t.close" autofocus @click="closeModal"><X :size="20" /></button></div>
      <div class="dialog-content"><p class="micro-label">INSTALLATION</p><h2 id="dialog-title">{{ t.modalTitle }}</h2><p class="dialog-intro">{{ t.modalIntro }}</p>
        <div v-for="(step, index) in t.steps" :key="step.title" class="guide-step"><span class="step-number">0{{ index + 1 }}</span><div><h3>{{ step.title }}</h3><p>{{ step.body }}</p>
          <div v-if="index === 0" class="sdk-downloads"><a v-for="sdk in downloads" :key="sdk.label" :href="sdk.href" class="text-link" target="_blank" rel="noopener noreferrer">{{ sdk.label }}<ArrowUpRight :size="15" /></a></div>
          <pre v-if="index === 1"><code>{{ hello }}</code></pre>
          <div v-if="index === 2" class="guide-command"><code>{{ command }}</code><button class="icon-button" :aria-label="t.commandCopy" @click="copyText(command, 'dialog')"><Check v-if="copied === 'dialog'" :size="16" /><Copy v-else :size="16" /></button></div>
        </div></div>
        <a class="button button-primary dialog-repo" :href="tutorialUrl"><ExternalLink :size="17" />{{ t.repoLink }}<ArrowRight :size="15" /></a>
      </div>
    </dialog>
  </div>
</template>
