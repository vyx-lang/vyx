/*
 * Cross-document transition shared by the two static entries.
 *
 * A real navigation cannot animate the outgoing document from the incoming one,
 * so the curtain is driven from both ends. The page being left pulls a panel up
 * over itself and then records a flag; the page being entered reads that flag
 * from an inline <head> script, paints the panel already covering before the
 * first frame, and lifts it away once the app has mounted.
 *
 * Every path fails open. Without storage, without animation support, under
 * reduced motion, or on any unexpected error the click navigates exactly as it
 * would have without this module.
 */
const STORAGE_KEY = 'vyx-page-transition'
const ENTER_ATTRIBUTE = 'data-page-enter'
const CURTAIN_ID = 'vyx-transition'
const ENTER_MS = 560
const LEAVE_MS = 420
const EASING_IN = 'cubic-bezier(.76, 0, .24, 1)'
const EASING_OUT = 'cubic-bezier(.62, 0, .32, 1)'

function motionDisabled() {
  try { if (matchMedia('(prefers-reduced-motion: reduce)').matches) return true } catch { return true }
  try { return localStorage.getItem('vyx-motion-paused') === 'true' } catch { return false }
}

/* The markup ships with every page; it is only rebuilt if a previous arrival
   already removed it from this document. */
function curtain() {
  let node = document.getElementById(CURTAIN_ID)
  if (node) return node
  node = document.createElement('div')
  node.id = CURTAIN_ID
  node.setAttribute('aria-hidden', 'true')
  node.innerHTML = '<span class="vyx-transition-mark">VYX</span><span class="vyx-transition-rule"></span>'
  document.body.appendChild(node)
  return node
}

/* Leaving: the panel rises from below and navigation waits for it to land. */
function leave(url) {
  const node = curtain()
  node.style.display = 'grid'
  let gone = false
  const go = () => {
    if (gone) return
    gone = true
    try { sessionStorage.setItem(STORAGE_KEY, 'enter') } catch { /* Navigation is unaffected. */ }
    location.assign(url)
  }
  node.animate(
    [{ transform: 'translateY(100%)' }, { transform: 'translateY(0)' }],
    { duration: LEAVE_MS, easing: EASING_OUT, fill: 'forwards' },
  ).finished.then(go).catch(go)
  // A stalled or interrupted animation must never strand the visitor here.
  setTimeout(go, LEAVE_MS + 360)
}

/* Arriving: the panel is already covering, so it only has to get out of the way. */
function arrive() {
  document.documentElement.removeAttribute(ENTER_ATTRIBUTE)
  const node = document.getElementById(CURTAIN_ID)
  if (!node) return
  let gone = false
  const drop = () => { if (gone) return; gone = true; node.remove() }
  node.querySelector('.vyx-transition-mark')?.animate(
    [{ opacity: 1 }, { opacity: 0 }],
    { duration: 260, easing: 'ease-out', fill: 'forwards' },
  )
  node.animate(
    [{ transform: 'translateY(0)' }, { transform: 'translateY(-100%)' }],
    { duration: ENTER_MS, easing: EASING_IN, fill: 'forwards' },
  ).finished.then(drop).catch(drop)
  setTimeout(drop, ENTER_MS + 460)
}

/* Only same-origin document navigations belong to the curtain; in-page anchors,
   downloads, new tabs, and asset paths are left entirely alone. */
function navigable(event, anchor) {
  if (event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return null
  if (anchor.target || anchor.hasAttribute('download') || anchor.hasAttribute('data-no-transition')) return null
  const raw = anchor.getAttribute('href')
  if (!raw || raw.startsWith('#')) return null
  let url
  try { url = new URL(raw, location.href) } catch { return null }
  if (url.protocol !== location.protocol || url.host !== location.host) return null
  if (url.pathname === location.pathname) return null
  return /\/(?:[\w.-]+\.html?)?$/.test(url.pathname) ? url : null
}

export function installPageTransition() {
  if (document.documentElement.hasAttribute(ENTER_ATTRIBUTE)) arrive()
  let leaving = false
  document.addEventListener('click', event => {
    if (motionDisabled() || leaving || event.defaultPrevented || !(event.target instanceof Element)) return
    const anchor = event.target.closest('a[href]')
    if (!anchor) return
    const url = navigable(event, anchor)
    if (!url) return
    event.preventDefault()
    leaving = true
    leave(url.href)
  }, true)
}
