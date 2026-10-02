<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { ArrowDown, ArrowLeft, ArrowUpRight, Check, Github, Menu, Moon, Sun, Pause, Play, X } from 'lucide-vue-next'
import { repository } from './content'
import { mospContent } from './mosp-content'
import { mospFeatures } from './mosp-features'
import MospDciPanel from './MospDciPanel.vue'
import MospFeaturePanel from './MospFeaturePanel.vue'
import TechnicalGlyph from './TechnicalGlyph.vue'
import { usePageMotion } from './usePageMotion'

const readPreference = (key, fallback, allowed) => {
  try {
    const value = localStorage.getItem(key)
    return allowed.includes(value) ? value : fallback
  } catch { return fallback }
}
const featureIds = ['migrate', 'reflection', 'dci', 'dce']
const dciAnchors = ['pipeline', 'vector', 'facts', 'ecosystem', 'failure', 'scope', 'reading']
function featureForHash(hash) {
  const id = hash.slice(1)
  return featureIds.includes(id) ? id : dciAnchors.includes(id) ? 'dci' : null
}
const locale = ref(readPreference('vyx-v2-language', 'zh', ['en', 'zh']))
const theme = ref(readPreference('vyx-v2-theme', 'light', ['dark', 'light']))
const active = ref(featureForHash(location.hash) || 'dci')
const pageRoot = ref(null)
const { paused: motionPaused, state: motionState } = usePageMotion(pageRoot)
const tabStyle = computed(() => {
  const index = featureIds.indexOf(active.value)
  return { '--active-index': index, '--active-mobile-column': index % 2, '--active-mobile-row': Math.floor(index / 2) }
})
const t = computed(() => mospContent[locale.value])
const details = computed(() => mospFeatures[locale.value])
const isZh = computed(() => locale.value === 'zh')
const logo = import.meta.env.BASE_URL + 'vyx.png'
const menuOpen = ref(false)
const copied = ref('')
const copyFailed = ref(false)
const readingSection = ref('dci')
const readingLinks = computed(() => (isZh.value ? [
  ['dci', 'DCI 概览'], ['pipeline', '构建流程'], ['vector', '原始 std::vector<T>'],
  ['facts', 'ABI 契约'], ['ecosystem', '原生库'], ['failure', '异常与清理'],
  ['scope', '接入配置'], ['reading', '文档与示例'],
] : [
  ['dci', 'Overview'], ['pipeline', 'Build pipeline'], ['vector', 'Original std::vector<T>'],
  ['facts', 'ABI contract'], ['ecosystem', 'Native libraries'], ['failure', 'Exceptions & cleanup'],
  ['scope', 'Integration'], ['reading', 'Further reading'],
]).map(([id, label]) => ({ id, label })))
let copyTimer
let readingFrame

function updateReadingSection() {
  cancelAnimationFrame(readingFrame)
  readingFrame = requestAnimationFrame(() => {
    if (active.value !== 'dci') return
    let current = 'dci'
    const scrollPadding = parseFloat(getComputedStyle(document.documentElement).scrollPaddingTop) || 0
    for (const { id } of readingLinks.value) {
      const element = document.getElementById(id)
      if (!element) continue
      const anchorLine = scrollPadding + (parseFloat(getComputedStyle(element).scrollMarginTop) || 0) + 2
      if (element.getBoundingClientRect().top <= Math.max(innerHeight * .24, anchorLine)) current = id
    }
    readingSection.value = current
  })
}

watch([locale, theme], () => {
  document.documentElement.lang = isZh.value ? 'zh-CN' : 'en'
  document.documentElement.dataset.theme = theme.value
  document.title = 'MOSP · Vyx'
  document.querySelector('meta[name="theme-color"]').content = getComputedStyle(document.documentElement).getPropertyValue('--bg').trim()
  document.querySelector('meta[name="description"]').content = t.value.intro
  try {
    localStorage.setItem('vyx-v2-language', locale.value)
    localStorage.setItem('vyx-v2-theme', theme.value)
  } catch { /* Reading the page does not require saved preferences. */ }
}, { immediate: true })
watch(active, () => { clearTimeout(copyTimer); copied.value = ''; copyFailed.value = false; updateReadingSection() })
watch(readingSection, async id => {
  await nextTick()
  const directory = pageRoot.value?.querySelector('.mosp-directory')
  const link = directory?.querySelector(`a[href="#${id}"]`)
  if (!link || directory.scrollWidth <= directory.clientWidth) return
  const frame = directory.getBoundingClientRect(), item = link.getBoundingClientRect()
  const shift = item.left < frame.left ? item.left - frame.left : item.right > frame.right ? item.right - frame.right : 0
  if (shift) directory.scrollBy({ left: shift, behavior: motionState.value === 'running' ? 'smooth' : 'instant' })
})

async function copyCode(code, id) {
  copied.value = ''
  copyFailed.value = false
  try {
    await navigator.clipboard.writeText(code)
    copied.value = id
    clearTimeout(copyTimer)
    copyTimer = setTimeout(() => { copied.value = '' }, 2200)
  } catch { copyFailed.value = true }
}
async function selectFeature(id, { focus = false, scroll = false } = {}) {
  active.value = id
  menuOpen.value = false
  if (location.hash !== `#${id}`) history.pushState(null, '', `#${id}`)
  await nextTick()
  if (scroll) document.getElementById('mosp-features')?.scrollIntoView({ block: 'start' })
  if (focus) document.getElementById(`mosp-tab-${id}`)?.focus({ preventScroll: true })
}
function onTabsKey(event) {
  if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return
  event.preventDefault()
  const index = featureIds.indexOf(active.value)
  const next = event.key === 'Home' ? 0 : event.key === 'End' ? featureIds.length - 1
    : (index + (event.key === 'ArrowRight' ? 1 : -1) + featureIds.length) % featureIds.length
  selectFeature(featureIds[next], { focus: true })
}
async function syncLocation() {
  const feature = featureForHash(location.hash)
  if (feature) active.value = feature
  else if (!location.hash) active.value = 'dci'
  menuOpen.value = false
  await nextTick()
  const id = location.hash.slice(1)
  const target = featureIds.includes(id) ? 'mosp-features' : id
  if (target) document.getElementById(target)?.scrollIntoView({ block: 'start' })
}
function closeMenu(event) { if (event.key === 'Escape') menuOpen.value = false }
onMounted(() => {
  window.addEventListener('keydown', closeMenu)
  window.addEventListener('hashchange', syncLocation)
  window.addEventListener('popstate', syncLocation)
  window.addEventListener('scroll', updateReadingSection, { passive: true })
  window.addEventListener('resize', updateReadingSection)
  updateReadingSection()
  if (location.hash) syncLocation()
})
onBeforeUnmount(() => {
  clearTimeout(copyTimer)
  window.removeEventListener('keydown', closeMenu)
  window.removeEventListener('hashchange', syncLocation)
  window.removeEventListener('popstate', syncLocation)
  window.removeEventListener('scroll', updateReadingSection)
  window.removeEventListener('resize', updateReadingSection)
  cancelAnimationFrame(readingFrame)
})
</script>

<template>
  <div ref="pageRoot" class="site-shell mosp-page" :data-motion="motionState" :class="{ 'locale-zh': isZh, 'motion-paused': motionState !== 'running' }">
    <div class="page-atmosphere" aria-hidden="true"></div>
    <a class="skip-link" href="#main">{{ isZh ? '跳至正文' : 'Skip to content' }}</a>
    <header class="site-header">
      <nav class="navigation" :aria-label="isZh ? '主导航' : 'Main navigation'">
        <a class="brand brand-icon" href="./" :aria-label="isZh ? '返回 Vyx 首页' : 'Vyx home'"><img :src="logo" class="brand-mark" width="58" height="58" alt="" /><span class="brand-label">VYX</span></a>
        <div class="desktop-links"><a href="./tutorial/">{{ isZh ? '教程' : 'Tutorial' }}</a><a v-for="feature in t.features" :key="feature.id" :href="`#${feature.id}`" :aria-current="active === feature.id ? 'location' : undefined" @click.prevent="selectFeature(feature.id, { scroll: true })">{{ feature.name }}</a></div>
        <div class="nav-actions">
          <button class="locale-button" :aria-label="isZh ? 'Switch to English' : '切换为中文'" @click="locale = isZh ? 'en' : 'zh'">{{ isZh ? 'EN' : '中' }}</button>
          <button class="icon-button theme-button" :aria-label="isZh ? '切换明暗主题' : 'Switch color theme'" @click="theme = theme === 'dark' ? 'light' : 'dark'"><Sun v-if="theme === 'dark'" :size="17" /><Moon v-else :size="17" /></button>
          <button class="icon-button motion-control" :aria-pressed="motionPaused" :aria-label="isZh ? motionPaused ? '恢复动效' : '暂停动效' : motionPaused ? 'Resume motion' : 'Pause motion'" @click="motionPaused = !motionPaused"><Play v-if="motionPaused" :size="15" /><Pause v-else :size="15" /></button>
          <a class="nav-github" :href="repository" target="_blank" rel="noopener noreferrer"><Github :size="16" /><span>GitHub</span><ArrowUpRight :size="14" /></a>
          <button class="icon-button mobile-menu-button" :aria-label="isZh ? menuOpen ? '关闭菜单' : '打开菜单' : menuOpen ? 'Close menu' : 'Open menu'" :aria-expanded="menuOpen" aria-controls="mobile-menu" @click="menuOpen = !menuOpen"><X v-if="menuOpen" :size="21" /><Menu v-else :size="21" /></button>
        </div>
      </nav>
      <div v-if="menuOpen" id="mobile-menu" class="mobile-menu glass"><a href="./">{{ isZh ? 'Vyx 首页' : 'Vyx home' }}<ArrowLeft :size="16" /></a><a href="./tutorial/">{{ isZh ? '语言教程' : 'Language tutorial' }}<ArrowUpRight :size="16" /></a><a v-for="feature in t.features" :key="feature.id" :href="`#${feature.id}`" :aria-current="active === feature.id ? 'location' : undefined" @click.prevent="selectFeature(feature.id, { scroll: true })">{{ feature.name }}<ArrowDown :size="16" /></a></div>
      <div class="reading-progress" aria-hidden="true"></div>
    </header>

    <main id="main" class="section-width mosp-main">
      <section class="mosp-hero">
        <a class="mosp-back text-link" href="./"><ArrowLeft :size="14" />{{ isZh ? 'Vyx 首页' : 'Vyx home' }}</a>
        <div class="mosp-masthead"><p class="micro-label">{{ t.label }} / {{ isZh ? '语言机制' : 'LANGUAGE ARCHITECTURE' }}</p><span>VYX / 01—04</span></div>
        <div class="mosp-hero-layout">
          <div><h1>MOSP<span class="mosp-title-line">{{ t.title }}</span></h1><p class="mosp-lead">{{ t.intro }}</p><a class="button button-primary" href="#dci" @click.prevent="selectFeature('dci', { scroll: true })">{{ isZh ? '了解 DCI 原生互操作' : 'Explore native interop with DCI' }}<ArrowDown :size="15" /></a></div>
          <div class="hero-art mosp-art"><div class="art-heading"><span>METADATA → NATIVE CODE</span><span>FIG. 02</span></div><TechnicalGlyph compact /><div class="art-caption"><span>MIGRATE / REFLECTION / DCI / DCE</span></div></div>
        </div>
      </section>

      <section id="mosp-features" class="mosp-tabs-section">
        <div class="mosp-section-heading mosp-feature-heading"><p class="micro-label">01 / FEATURES</p><h2>{{ isZh ? '四项核心特性' : 'FOUR CORE FEATURES' }}</h2><span>{{ isZh ? '选择一项，查看用法与示例。' : 'Choose a feature to explore its use.' }}</span></div>
        <div class="mosp-tabs" :style="tabStyle" role="tablist" :aria-label="isZh ? 'MOSP 核心特性' : 'MOSP core features'" @keydown="onTabsKey">
          <span class="mosp-tab-indicator" aria-hidden="true"></span>
          <button v-for="(feature, index) in t.features" :id="`mosp-tab-${feature.id}`" :key="feature.id" role="tab" :aria-selected="active === feature.id" :aria-controls="`mosp-panel-${feature.id}`" :tabindex="active === feature.id ? 0 : -1" @click="selectFeature(feature.id)"><span class="mosp-tab-name"><span class="mosp-step-number">0{{ index + 1 }}</span><strong>{{ feature.name }}</strong><ArrowDown :size="15" /></span><span class="mosp-tab-description">{{ feature.description }}</span></button>
        </div>
      </section>

      <div class="mosp-document-layout" :class="{ 'with-directory': active === 'dci' }">
        <nav v-if="active === 'dci'" class="mosp-directory" :aria-label="isZh ? 'DCI 页内目录' : 'DCI contents'">
          <p class="micro-label">DCI / {{ isZh ? '本页目录' : 'ON THIS PAGE' }}</p>
          <a v-for="link in readingLinks" :key="link.id" :href="`#${link.id}`" :aria-current="readingSection === link.id ? 'location' : undefined">{{ link.label }}<ArrowUpRight :size="12" /></a>
          <span class="mosp-directory-note">DECLARATIVE<br>CODE INTERFACE</span>
        </nav>
      <div class="mosp-panel-stack">
        <Transition v-for="feature in t.features" :key="feature.id" name="panel-switch" :css="motionState === 'running'">
          <div v-show="active === feature.id" :id="`mosp-panel-${feature.id}`" class="mosp-tabpanel" :class="{ 'is-active': active === feature.id }" role="tabpanel" :aria-labelledby="`mosp-tab-${feature.id}`" :aria-hidden="active !== feature.id" :inert="active !== feature.id" tabindex="0">
            <MospDciPanel v-if="feature.id === 'dci'" :content="t" :locale="locale" :copied="copied" @copy="copyCode" />
            <MospFeaturePanel v-else :feature="feature.id" :content="details[feature.id]" :locale="locale" :copied="copied" @copy="copyCode" />
          </div>
        </Transition>
      </div>
      </div>
    </main>
    <footer class="site-footer section-width mosp-footer"><a class="text-link" href="./"><ArrowLeft :size="15" />{{ isZh ? '返回 Vyx 首页' : 'Back to Vyx' }}</a><span>MOSP</span><a class="text-link" :href="repository" target="_blank" rel="noopener noreferrer">GitHub<ArrowUpRight :size="15" /></a></footer>
    <div class="toast" :class="{ visible: copied || copyFailed }" role="status" aria-live="polite"><Check v-if="copied" :size="16" />{{ isZh ? copyFailed ? '请选中代码手动复制。' : '已复制代码' : copyFailed ? 'Select the code to copy it manually.' : 'Code copied' }}</div>
  </div>
</template>
