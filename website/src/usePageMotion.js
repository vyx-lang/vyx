import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'

const PAGE_REVEAL = '.reveal, .hero-heading, .technical-glyph, .language-intro, .language-facts > div, .compile-heading, .interop-heading, .interop-canvas, .start-intro, .learning-links a, .mosp-section-heading, .mosp-code, .mosp-feature-points > div, .mosp-ecosystem article, .mosp-reading-links a, .tutorial-section, .tutorial-figure, .tutorial-stages > li, .tutorial-intro > *'

// One page clock keeps the compiler stages in order and resumes where it paused.
// `steps` splits the same period into as many discrete phases as a page needs
// (the home page walks five compiler stages, the tutorial steps through more).
export function usePageMotion(root, { steps = 5, reveal: extraReveal = '' } = {}) {
  let saved = false
  try { saved = localStorage.getItem('vyx-motion-paused') === 'true' } catch { /* Optional preference. */ }
  const paused = ref(saved)
  const reduced = ref(matchMedia('(prefers-reduced-motion: reduce)').matches)
  const hidden = ref(document.hidden)
  const phase = ref(reduced.value ? steps - 1 : 0)
  const state = computed(() => reduced.value ? 'reduced' : paused.value || hidden.value ? 'paused' : 'running')
  let frame, last = 0, elapsed = 0, observer, mutations, media
  const period = 8000
  const revealSelectors = extraReveal ? `${PAGE_REVEAL}, ${extraReveal}` : PAGE_REVEAL

  function reveal(node) {
    if (!(node instanceof Element) || node.dataset.motionObserved) return
    node.dataset.motionObserved = 'true'
    node.classList.add('reveal')
    if (state.value !== 'running') node.classList.add('is-visible')
    else observer.observe(node)
  }
  function scan(node) {
    if (!(node instanceof Element)) return
    if (node.matches(revealSelectors)) reveal(node)
    node.querySelectorAll(revealSelectors).forEach(reveal)
  }
  function showContent() {
    root.value?.querySelectorAll('.reveal').forEach(node => node.classList.add('is-visible'))
  }
  function updateScroll() {
    const length = document.documentElement.scrollHeight - innerHeight
    root.value?.style.setProperty('--scroll-progress', String(length > 0 ? Math.min(1, scrollY / length) : 0))
  }
  function tick(time) {
    elapsed += Math.min(64, Math.max(0, time - last))
    last = time
    const progress = (elapsed % period) / period
    root.value?.style.setProperty('--motion-progress', String(progress))
    phase.value = Math.min(steps - 1, Math.floor(progress * steps))
    frame = requestAnimationFrame(tick)
  }
  function sync() {
    cancelAnimationFrame(frame)
    if (state.value === 'running') {
      last = performance.now()
      frame = requestAnimationFrame(tick)
    } else {
      showContent()
      if (reduced.value) {
        phase.value = steps - 1
        root.value?.style.setProperty('--motion-progress', '1')
      }
    }
  }
  function onMedia() { reduced.value = media.matches }
  function onVisibility() { hidden.value = document.hidden }
  watch(state, sync)
  watch(paused, value => { try { localStorage.setItem('vyx-motion-paused', String(value)) } catch { /* Optional preference. */ } })
  onMounted(() => {
    observer = new IntersectionObserver(entries => {
      for (const entry of entries) if (entry.isIntersecting) {
        entry.target.classList.add('is-visible')
        observer.unobserve(entry.target)
      }
    }, { threshold: 0.08, rootMargin: '0px 0px -24px 0px' })
    scan(root.value)
    root.value.classList.add('motion-ready')
    mutations = new MutationObserver(records => {
      for (const record of records) for (const node of record.addedNodes) scan(node)
      updateScroll()
    })
    mutations.observe(root.value, { childList: true, subtree: true })
    media = matchMedia('(prefers-reduced-motion: reduce)')
    media.addEventListener('change', onMedia)
    window.addEventListener('scroll', updateScroll, { passive: true })
    window.addEventListener('resize', updateScroll)
    document.addEventListener('visibilitychange', onVisibility)
    updateScroll()
    sync()
  })
  onBeforeUnmount(() => {
    cancelAnimationFrame(frame)
    observer?.disconnect()
    mutations?.disconnect()
    media?.removeEventListener('change', onMedia)
    window.removeEventListener('scroll', updateScroll)
    window.removeEventListener('resize', updateScroll)
    document.removeEventListener('visibilitychange', onVisibility)
  })
  return { paused, reduced, state, phase }
}
