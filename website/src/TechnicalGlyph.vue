<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { createAsciiGlyph, staticAsciiGlyph } from './asciiGlyph'

defineProps({ compact: Boolean })
const root = ref(null), canvas = ref(null), fallback = ref(false)
const staticGlyph = staticAsciiGlyph()
let renderer, shell, observer, mutations, resize
let frame = 0, previous = 0, paintedAt = 0, elapsed = 0
let visible = true, alive = true
const pointer = [0, 0], target = [0, 0]
let ink = [.13, .15, .16], accent = [.16, .22, 1]

function colors() {
  const style = getComputedStyle(root.value)
  ink = (style.color.match(/[\d.]+/g) ?? [33, 38, 41]).slice(0, 3).map(value => Number(value) / 255)
  accent = style.getPropertyValue('--accent-ink-rgb').trim().split(/[ ,]+/).slice(0, 3).map(value => Number(value) / 255)
}
function draw() {
  if (!renderer || fallback.value) return
  const reduced = shell?.dataset.motion === 'reduced'
  renderer.render(reduced ? 0 : elapsed, reduced ? [0, 0] : pointer, ink, accent)
  root.value.dataset.pose = (reduced ? 0 : elapsed).toFixed(4)
}
function tick(now) {
  elapsed += previous ? Math.min(.064, (now - previous) / 1000) : 0
  previous = now
  for (let i = 0; i < 2; i++) pointer[i] += (target[i] - pointer[i]) * .055
  if (now - paintedAt >= 32) { draw(); paintedAt = now }
  frame = requestAnimationFrame(tick)
}
function sync() {
  cancelAnimationFrame(frame)
  previous = 0
  colors()
  draw()
  if (!fallback.value && visible && !document.hidden && shell?.dataset.motion === 'running') frame = requestAnimationFrame(tick)
}
function move(event) {
  const rect = root.value.getBoundingClientRect()
  target[0] = ((event.clientX - rect.left) / rect.width - .5) * .5
  target[1] = ((event.clientY - rect.top) / rect.height - .5) * .28
}
function leave() { target.fill(0) }
function lost(event) {
  event.preventDefault()
  fallback.value = true
  root.value.dataset.renderer = 'fallback'
  cancelAnimationFrame(frame)
}
function restore() {
  renderer?.dispose()
  renderer = createAsciiGlyph(canvas.value)
  fallback.value = !renderer
  root.value.dataset.renderer = renderer ? 'webgl' : 'fallback'
  sync()
}

onMounted(() => {
  shell = root.value.closest('.site-shell')
  restore()
  document.fonts.ready.then(() => { if (alive && renderer && !fallback.value) { renderer.refreshAtlas(); sync() } })
  observer = new IntersectionObserver(entries => { visible = entries.some(entry => entry.isIntersecting); sync() })
  observer.observe(root.value)
  mutations = new MutationObserver(sync)
  mutations.observe(shell, { attributes: true, attributeFilter: ['data-motion'] })
  mutations.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] })
  resize = new ResizeObserver(sync)
  resize.observe(root.value)
  document.addEventListener('visibilitychange', sync)
  canvas.value.addEventListener('webglcontextlost', lost)
  canvas.value.addEventListener('webglcontextrestored', restore)
})
onBeforeUnmount(() => {
  alive = false
  cancelAnimationFrame(frame)
  observer?.disconnect()
  mutations?.disconnect()
  resize?.disconnect()
  document.removeEventListener('visibilitychange', sync)
  canvas.value?.removeEventListener('webglcontextlost', lost)
  canvas.value?.removeEventListener('webglcontextrestored', restore)
  renderer?.dispose()
})
</script>

<template>
  <div ref="root" class="technical-glyph ascii-glyph" :class="{ compact }" aria-hidden="true" @pointermove="move" @pointerleave="leave">
    <canvas ref="canvas" class="glyph-canvas" :class="{ 'is-unavailable': fallback }"></canvas>
    <pre v-if="fallback" class="glyph-fallback">{{ staticGlyph }}</pre>
    <svg class="glyph-guides" viewBox="0 0 760 500" fill="none">
      <g class="glyph-axis"><path d="M20 20h18M20 20v18M740 20h-18M740 20v18M20 480h18M20 480v-18M740 480h-18M740 480v-18" /></g>
    </svg>
    <span class="glyph-marker marker-source">SOURCE</span>
    <span class="glyph-marker marker-native">NATIVE</span>
  </div>
</template>
