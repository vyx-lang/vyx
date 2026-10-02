<script setup>
/*
 * 教程站。内容来自 docs/ 下的双语文档，构建时由 scripts/build-tutorial.mjs 抽成分节 JSON，
 * 每章一份、按语言分块加载（切换语言或章节时才取那一块）。
 *
 * 路由用哈希：#<chapter> 或 #<chapter>/<section>。静态托管下不需要任何重写规则，
 * 主站的教程入口直接指到 ./tutorial/ 即可。
 */
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { ArrowLeft, ArrowRight, ArrowUpRight, Check, Github, Menu, Moon, Pause, Play, Sun, X } from 'lucide-vue-next'
import { repository } from './content'
import { chapterFigures, chapterOrder, chapters } from './tutorial-chapters'
import TutorialBlocks from './TutorialBlocks.vue'
import TutorialFigure from './TutorialFigure.vue'
import TutorialSection from './TutorialSection.vue'
import { usePageMotion } from './usePageMotion'

const CHUNKS = 8
const loaders = {
  basics: { zh: () => import('./tutorial/content/basics.zh.json'), en: () => import('./tutorial/content/basics.en.json') },
  intermediate: { zh: () => import('./tutorial/content/intermediate.zh.json'), en: () => import('./tutorial/content/intermediate.en.json') },
  advanced: { zh: () => import('./tutorial/content/advanced.zh.json'), en: () => import('./tutorial/content/advanced.en.json') },
  migration: { zh: () => import('./tutorial/content/migration.zh.json'), en: () => import('./tutorial/content/migration.en.json') },
}

const readPreference = (key, fallback, allowed) => {
  try {
    const value = localStorage.getItem(key)
    return allowed.includes(value) ? value : fallback
  } catch { return fallback }
}
function parseHash(hash) {
  const [id, section = ''] = hash.replace(/^#/, '').split('/')
  return { chapter: chapterOrder.includes(id) ? id : chapterOrder[0], section }
}

const locale = ref(readPreference('vyx-v2-language', 'zh', ['en', 'zh']))
const theme = ref(readPreference('vyx-v2-theme', 'light', ['dark', 'light']))
const route = ref(parseHash(location.hash))
const content = ref(null)
const cache = new Map()
const pageRoot = ref(null)
const { paused: motionPaused, state: motionState, phase } = usePageMotion(pageRoot, { steps: CHUNKS })
const isZh = computed(() => locale.value === 'zh')
const logo = '../vyx.png'
const menuOpen = ref(false)
const copied = ref('')
const activeSection = ref('')
const item = computed(() => content.value ?? null)
const meta = computed(() => chapters[route.value.chapter])
const figurePlan = computed(() => chapterFigures[route.value.chapter] ?? { hero: '', sections: [] })
const chapterIndex = computed(() => chapterOrder.indexOf(route.value.chapter))
const pager = computed(() => ({
  prev: chapterIndex.value > 0 ? chapterOrder[chapterIndex.value - 1] : '',
  next: chapterIndex.value < chapterOrder.length - 1 ? chapterOrder[chapterIndex.value + 1] : '',
}))
let copyTimer
let loadToken = 0
let targetFrame
let activeFrame
let navigationToken = 0

const label = id => chapters[id].label[locale.value]
const stage = id => chapters[id].stage[locale.value]
const blurb = id => chapters[id].blurb[locale.value]

function figureFor(sectionId) {
  return (figurePlan.value.sections.find(([key]) => sectionId.includes(key)) ?? [])[1] ?? ''
}

async function ensureContent(chapter = route.value.chapter, language = locale.value) {
  const key = `${chapter}.${language}`
  loadToken += 1
  const token = loadToken
  if (!cache.has(key)) {
    const module = await loaders[chapter][language]()
    cache.set(key, module.default ?? module)
  }
  if (token === loadToken) content.value = cache.get(key)
}

function scrollToTarget(section) {
  cancelAnimationFrame(targetFrame)
  targetFrame = requestAnimationFrame(() => {
    const target = section ? document.getElementById(section) : null
    if (target) target.scrollIntoView({ block: 'start', behavior: motionState.value === 'running' ? 'smooth' : 'instant' })
    else window.scrollTo({ top: 0, behavior: 'instant' })
    const heading = (target ?? document.getElementById('chapter-head'))?.querySelector('h2')
    heading?.focus({ preventScroll: true })
    updateActiveSection()
  })
}

async function applyHash({ scroll = true } = {}) {
  const next = parseHash(location.hash)
  const token = ++navigationToken
  route.value = next
  menuOpen.value = false
  await ensureContent(next.chapter, locale.value)
  await nextTick()
  if (token !== navigationToken) return
  if (scroll) scrollToTarget(next.section)
  updateActiveSection()
}

function selectChapter(id, section = '') {
  const hash = `#${id}${section ? '/' + section : ''}`
  if (location.hash === hash) { scrollToTarget(section); return }
  history.pushState(null, '', hash)
  applyHash()
}

function updateActiveSection() {
  if (!item.value) return
  cancelAnimationFrame(activeFrame)
  activeFrame = requestAnimationFrame(() => {
    const scrollPadding = parseFloat(getComputedStyle(document.documentElement).scrollPaddingTop) || 0
    let current = ''
    for (const section of item.value.sections) {
      const element = document.getElementById(section.id)
      if (!element) continue
      if (element.getBoundingClientRect().top <= Math.max(innerHeight * .28, scrollPadding + 40)) current = section.id
    }
    activeSection.value = current
  })
}

async function copy(text, id) {
  try {
    await navigator.clipboard.writeText(text)
    copied.value = id
    clearTimeout(copyTimer)
    copyTimer = setTimeout(() => { copied.value = '' }, 2200)
  } catch { copied.value = '' }
}
async function copyAnchor(section) {
  await copy(`${location.origin}${location.pathname}#${route.value.chapter}/${section}`, section)
  if (copied.value === section) scrollToTarget(section)
}
function closeMenu(event) { if (event.key === 'Escape') menuOpen.value = false }
function skipToContent() {
  const main = document.getElementById('main')
  main?.scrollIntoView({ block: 'start', behavior: 'instant' })
  main?.focus({ preventScroll: true })
}

function syncPreferences() {
  document.documentElement.lang = isZh.value ? 'zh-CN' : 'en'
  document.documentElement.dataset.theme = theme.value
  document.title = `${label(route.value.chapter)} · Vyx ${isZh.value ? '教程' : 'Tutorial'}`
  document.querySelector('meta[name="theme-color"]').content = getComputedStyle(document.documentElement).getPropertyValue('--bg').trim()
  document.querySelector('meta[name="description"]').content = `${item.value?.title ?? label(route.value.chapter)}。${blurb(route.value.chapter)}`
  try {
    localStorage.setItem('vyx-v2-language', locale.value)
    localStorage.setItem('vyx-v2-theme', theme.value)
  } catch { /* 偏好可省，页面照常读。 */ }
}
watch([locale, theme, () => route.value.chapter, () => item.value?.title], syncPreferences, { immediate: true })

watch(locale, async () => { await applyHash() })

function onRouteChange() { applyHash() }

onMounted(async () => {
  window.addEventListener('keydown', closeMenu)
  window.addEventListener('hashchange', onRouteChange)
  window.addEventListener('popstate', onRouteChange)
  window.addEventListener('scroll', updateActiveSection, { passive: true })
  window.addEventListener('resize', updateActiveSection)
  await applyHash({ scroll: !!route.value.section })
})
onBeforeUnmount(() => {
  clearTimeout(copyTimer)
  navigationToken++
  loadToken++
  cancelAnimationFrame(targetFrame)
  cancelAnimationFrame(activeFrame)
  window.removeEventListener('hashchange', onRouteChange)
  window.removeEventListener('popstate', onRouteChange)
  window.removeEventListener('keydown', closeMenu)
  window.removeEventListener('scroll', updateActiveSection)
  window.removeEventListener('resize', updateActiveSection)
})
</script>

<template>
  <div ref="pageRoot" class="site-shell tutorial-page" :data-motion="motionState" :class="{ 'locale-zh': isZh, 'motion-paused': motionState !== 'running' }">
    <div class="page-atmosphere" aria-hidden="true"></div>
    <a class="skip-link" href="#main" @click.prevent="skipToContent">{{ isZh ? '跳至正文' : 'Skip to content' }}</a>

    <header class="site-header">
      <nav class="navigation" :aria-label="isZh ? '主导航' : 'Main navigation'">
        <a class="brand brand-icon" href="../" :aria-label="isZh ? '返回 Vyx 首页' : 'Vyx home'"><img :src="logo" class="brand-mark" width="58" height="58" alt="" /><span class="brand-label">VYX</span><span class="brand-suffix">{{ isZh ? '教程' : 'TUTORIAL' }}</span></a>
        <div class="desktop-links"><a href="../">{{ isZh ? '首页' : 'Home' }}</a><a href="../mosp.html">MOSP / DCI<ArrowUpRight :size="13" /></a><a href="#" @click.prevent="selectChapter('basics')">{{ isZh ? '从第一课开始' : 'Start at lesson one' }}</a></div>
        <div class="nav-actions">
          <button class="locale-button" :aria-label="isZh ? 'Switch to English' : '切换为中文'" @click="locale = isZh ? 'en' : 'zh'">{{ isZh ? 'EN' : '中' }}</button>
          <button class="icon-button theme-button" :aria-label="isZh ? '切换明暗主题' : 'Switch color theme'" @click="theme = theme === 'dark' ? 'light' : 'dark'"><Sun v-if="theme === 'dark'" :size="17" /><Moon v-else :size="17" /></button>
          <button class="icon-button motion-control" :aria-pressed="motionPaused" :aria-label="isZh ? motionPaused ? '恢复动效' : '暂停动效' : motionPaused ? 'Resume motion' : 'Pause motion'" @click="motionPaused = !motionPaused"><Play v-if="motionPaused" :size="15" /><Pause v-else :size="15" /></button>
          <a class="nav-github" :href="repository" target="_blank" rel="noopener noreferrer"><Github :size="16" /><span>GitHub</span><ArrowUpRight :size="14" /></a>
          <button class="icon-button mobile-menu-button" :aria-label="isZh ? menuOpen ? '关闭菜单' : '打开菜单' : menuOpen ? 'Close menu' : 'Open menu'" :aria-expanded="menuOpen" aria-controls="mobile-menu" @click="menuOpen = !menuOpen"><X v-if="menuOpen" :size="21" /><Menu v-else :size="21" /></button>
        </div>
      </nav>
      <div v-if="menuOpen" id="mobile-menu" class="mobile-menu glass"><a href="../">{{ isZh ? 'Vyx 首页' : 'Vyx home' }}<ArrowLeft :size="16" /></a><a href="../mosp.html">MOSP / DCI<ArrowUpRight :size="16" /></a><button v-for="id in chapterOrder" :key="id" @click="selectChapter(id)">{{ label(id) }}<ArrowRight :size="16" /></button></div>
      <div class="reading-progress" aria-hidden="true"></div>
    </header>

    <main id="main" tabindex="-1">
      <section class="tutorial-hero section-width">
        <a class="tutorial-back text-link" href="../"><ArrowLeft :size="14" />{{ isZh ? 'Vyx 首页' : 'Vyx home' }}</a>
        <div class="tutorial-masthead"><p class="micro-label">TUTORIAL / {{ isZh ? '语言教程' : 'THE LANGUAGE' }}</p><span>VYX / {{ chapterOrder.length }} CHAPTERS</span></div>
        <div class="tutorial-intro">
          <h1>{{ isZh ? '从第一个程序，\n到完整的项目。' : 'From your first program\nto a complete project.' }}</h1>
          <p>{{ isZh
            ? '从变量、函数与类型开始，逐步学习错误处理、模块和资源管理。按章节阅读，或从目录找到需要的写法。'
            : 'Start with bindings, functions and types, then work through errors, modules and resource management. Read in order, or use the contents to find a particular construct.' }}</p>
        </div>
        <ol class="tutorial-stages" :style="{ '--chapter-count': chapterOrder.length }">
          <li v-for="(id, index) in chapterOrder" :key="id" :class="{ 'is-active': id === route.chapter }">
            <button :aria-current="id === route.chapter ? 'true' : undefined" @click="selectChapter(id)">
              <span class="index-number">0{{ index + 1 }}</span>
              <span class="stage-label">{{ stage(id) }}</span>
              <strong>{{ label(id) }}</strong>
              <span class="stage-blurb">{{ blurb(id) }}</span>
              <ArrowRight :size="15" />
            </button>
          </li>
        </ol>
      </section>

      <div class="tutorial-layout section-width">
        <nav class="tutorial-rail" :aria-label="isZh ? '章节目录' : 'Contents'">
          <div class="rail-mobile">
            <label><span>{{ isZh ? '章节' : 'CHAPTER' }}</span><select :aria-label="isZh ? '选择章节' : 'Select chapter'" :value="route.chapter" @change="selectChapter($event.target.value)"><option v-for="id in chapterOrder" :key="id" :value="id">{{ label(id) }}</option></select></label>
            <label v-if="item"><span>{{ isZh ? '小节' : 'SECTION' }}</span><select :aria-label="isZh ? '选择小节' : 'Select section'" :value="activeSection" @change="selectChapter(route.chapter, $event.target.value)"><option value="">{{ isZh ? '章节导读' : 'Chapter introduction' }}</option><option v-for="section in item.sections" :key="section.id" :value="section.id">{{ section.title }}</option></select></label>
          </div>
          <div class="rail-chapters">
            <p class="micro-label">{{ isZh ? '章节' : 'CHAPTERS' }}</p>
            <button v-for="id in chapterOrder" :key="id" :class="{ 'is-active': id === route.chapter }" :aria-current="id === route.chapter ? 'true' : undefined" @click="selectChapter(id)"><span>0{{ chapterOrder.indexOf(id) + 1 }}</span>{{ label(id) }}</button>
          </div>
          <div v-if="item" class="rail-sections">
            <p class="micro-label">{{ isZh ? '本章小节' : 'IN THIS CHAPTER' }}</p>
            <a v-for="section in item.sections" :key="section.id" :href="`#${route.chapter}/${section.id}`" :aria-current="activeSection === section.id ? 'location' : undefined" @click.prevent="selectChapter(route.chapter, section.id)"><span>{{ section.n ? String(section.n).padStart(2, '0') : '—' }}</span>{{ section.title }}</a>
          </div>
          <span class="rail-note">DOCS<br>→ WEB</span>
        </nav>

        <div class="tutorial-body">
          <header id="chapter-head" class="chapter-head">
            <p class="micro-label">{{ stage(route.chapter) }} · {{ isZh ? '来源' : 'SOURCE' }} <a :href="item?.sourceUrl ?? repository" target="_blank" rel="noopener noreferrer">{{ item?.source ?? '' }}<ArrowUpRight :size="11" /></a></p>
            <h2 tabindex="-1">{{ label(route.chapter) }}</h2>
            <p class="chapter-title">{{ item?.title ?? '' }}</p>
            <p class="chapter-blurb">{{ blurb(route.chapter) }}</p>
            <div v-if="item?.intro?.length" class="chapter-intro">
              <TutorialBlocks :blocks="item.intro" :locale="locale" :copied="copied" id-prefix="intro" @copy="(text, id) => copy(text, id)" />
            </div>
            <dl v-if="item" class="chapter-stats">
              <div><dt>{{ isZh ? '小节' : 'Sections' }}</dt><dd>{{ item.stats.sections }}</dd></div>
              <div><dt>{{ isZh ? '课程' : 'Lessons' }}</dt><dd>{{ item.stats.lessons }}</dd></div>
              <div><dt>{{ isZh ? '代码段' : 'Code blocks' }}</dt><dd>{{ item.stats.code }}</dd></div>
            </dl>
            <TutorialFigure v-if="figurePlan.hero" :name="figurePlan.hero" :locale="locale" :phase="phase" />
          </header>

          <TutorialSection v-for="section in item?.sections ?? []" :key="section.id" :section="section" :locale="locale" :copied="copied" :figure="figureFor(section.id)" :phase="phase" @copy="copy" @anchor="copyAnchor" />

          <nav class="chapter-pager" :aria-label="isZh ? '章节翻页' : 'Chapter pagination'">
            <button v-if="pager.prev" @click="selectChapter(pager.prev)"><ArrowLeft :size="15" /><span><small>{{ isZh ? '上一章' : 'Previous' }}</small>{{ label(pager.prev) }}</span></button>
            <span v-else></span>
            <button v-if="pager.next" class="next" @click="selectChapter(pager.next)"><span><small>{{ isZh ? '下一章' : 'Next' }}</small>{{ label(pager.next) }}</span><ArrowRight :size="15" /></button>
          </nav>
        </div>
      </div>
    </main>

    <footer class="site-footer section-width tutorial-footer">
      <a class="text-link" href="../"><ArrowLeft :size="15" />{{ isZh ? '返回 Vyx 首页' : 'Back to Vyx' }}</a>
      <span>TUTORIAL</span>
      <a class="text-link" :href="repository" target="_blank" rel="noopener noreferrer">GitHub<ArrowUpRight :size="15" /></a>
    </footer>

    <div class="toast" :class="{ visible: !!copied }" role="status" aria-live="polite"><Check :size="16" />{{ isZh ? '已复制' : 'Copied' }}</div>
  </div>
</template>
