import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { existsSync, mkdirSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { chromium } from 'playwright'

const root = fileURLToPath(new URL('../', import.meta.url))
const artifacts = fileURLToPath(new URL('../artifacts/', import.meta.url))
const url = 'http://127.0.0.1:4173'
mkdirSync(artifacts, { recursive: true })
assert(existsSync(new URL('../dist/mosp.html', import.meta.url)), 'Build both website entries first.')
const server = spawn(process.execPath, ['node_modules/vite/bin/vite.js', 'preview', '--host', '127.0.0.1'], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true })
let output = ''
// Vite colourises the port separately ("http://127.0.0.1:<esc>4173<esc>/"), so the
// raw stream never contains the plain URL. Strip escapes before matching.
const stripAnsi = value => String(value).replace(/\u001b\[[0-9;]*m/g, '')
server.stdout.on('data', chunk => { output += stripAnsi(chunk) })
server.stderr.on('data', chunk => { output += stripAnsi(chunk) })
const delay = ms => new Promise(resolve => setTimeout(resolve, ms))
const errors = []
let browser
const observeErrors = page => {
  page.on('pageerror', error => errors.push(error.message))
  page.on('console', message => { if (message.type() === 'error') errors.push(message.text()) })
  page.on('response', response => { if (response.status() >= 400) errors.push(`${response.status()} ${response.url()}`) })
}
const assertReadingStill = async page => {
  const movingSource = await page.evaluate(() => [...document.querySelectorAll('pre, code, .editor-code')].flatMap(element => {
    const moving = []
    for (let ancestor = element; ancestor; ancestor = ancestor.parentElement) {
      for (const animation of ancestor.getAnimations()) {
        if (animation.effect?.getTiming().iterations === Infinity) moving.push(`${element.tagName}.${element.className}: ${animation.animationName || 'infinite animation'}`)
      }
    }
    return moving
  }))
  assert.deepEqual(movingSource, [], 'Source and reading surfaces have no perpetual animation')
}
const animationState = (page, selector) => page.locator(selector).evaluate(element => {
  const style = getComputedStyle(element)
  return {
    transform: style.transform, translate: style.translate, left: style.left, opacity: style.opacity,
    animations: element.getAnimations().map(animation => ({
      name: animation.animationName || animation.constructor.name,
      state: animation.playState, time: animation.currentTime,
      duration: animation.effect?.getTiming().duration,
      iterations: animation.effect?.getTiming().iterations === Infinity ? 'infinite' : animation.effect?.getTiming().iterations,
    })),
  }
})
const assertFrozen = (before, after, label) => {
  assert.equal(after.transform, before.transform, `${label}: transform freezes`)
  assert.equal(after.translate, before.translate, `${label}: translation freezes`)
  assert.equal(after.left, before.left, `${label}: position freezes`)
  assert.equal(after.opacity, before.opacity, `${label}: opacity freezes`)
  assert.equal(after.animations.length, before.animations.length, `${label}: animation objects remain stable`)
  for (let index = 0; index < before.animations.length; index++) {
    const first = before.animations[index], last = after.animations[index]
    assert.equal(last.state, 'paused', `${label}: animation is paused`)
    assert(Math.abs(Number(last.time) - Number(first.time)) < 2, `${label}: animation time freezes`)
  }
}
const assertFeatureSettled = async (page, feature) => {
  await page.waitForFunction(id => {
    const selected = document.getElementById(`mosp-tab-${id}`)
    const panel = document.getElementById(`mosp-panel-${id}`)
    if (selected?.getAttribute('aria-selected') !== 'true' || !panel) return false
    const style = getComputedStyle(panel)
    const outgoingHidden = [...document.querySelectorAll('.mosp-tabpanel')].every(element => element === panel || getComputedStyle(element).display === 'none')
    return outgoingHidden && style.display !== 'none' && Number(style.opacity) > .99 && !panel.getAnimations().some(animation => animation.playState === 'running')
  }, feature)
  for (const id of ['migrate', 'reflection', 'dci', 'dce']) {
    assert.equal(await page.locator(`#mosp-panel-${id}`).isVisible(), id === feature, 'Only the settled selected feature remains visible')
  }
}
const assertNoRunningMotion = async page => {
  const active = await page.evaluate(() => document.getAnimations().filter(animation => animation.playState === 'running').map(animation => ({ name: animation.animationName || animation.constructor.name, duration: animation.effect?.getTiming().duration })))
  assert.deepEqual(active, [], 'Reduced motion leaves no running animation or transition')
}
try {
  for (let attempt = 0; !output.includes(url); attempt++) {
    assert(attempt < 100 && server.exitCode === null, 'Preview failed: ' + output)
    await delay(100)
  }
  const installedChrome = 'C:/Program Files/Google/Chrome/Application/chrome.exe'
  browser = await chromium.launch({ headless: true, ...(existsSync(installedChrome) ? { executablePath: installedChrome } : {}) })
  const context = await browser.newContext({ viewport: { width: 1440, height: 1000 }, reducedMotion: 'no-preference' })
  const page = await context.newPage()
  observeErrors(page)
  await page.goto(url)
  await page.locator('.hero h1').waitFor()
  await page.evaluate(() => document.fonts.ready)
  assert.equal(await page.locator('.site-shell').getAttribute('data-motion'), 'running', 'Normal motion starts in the running state')
  const glyph = page.locator('.technical-glyph')
  const canvas = glyph.locator('canvas')
  assert.equal(await glyph.getAttribute('data-renderer'), 'webgl', 'The ASCII illustration uses WebGL')
  await page.waitForFunction(() => Number(getComputedStyle(document.querySelector('.hero-art')).opacity) > .99)
  const firstPixels = await canvas.screenshot()
  await page.waitForTimeout(250)
  assert.notDeepEqual(await canvas.screenshot(), firstPixels, 'Rotation changes actual ASCII pixels')
  await page.locator('.motion-control').click()
  await page.waitForFunction(() => document.querySelector('.site-shell').dataset.motion === 'paused')
  const pausedPose = await glyph.getAttribute('data-pose')
  const pausedPixels = await canvas.screenshot()
  await page.waitForTimeout(250)
  assert.equal(await glyph.getAttribute('data-pose'), pausedPose, 'Pause stops the WebGL clock')
  assert.deepEqual(await canvas.screenshot(), pausedPixels, 'Pause preserves the displayed ASCII pose')
  await page.locator('.motion-control').click()
  await page.waitForFunction(pose => document.querySelector('.technical-glyph').dataset.pose !== pose, pausedPose)
  assert.notDeepEqual(await canvas.screenshot(), pausedPixels, 'Resume restarts the illustration')
  // Pausing makes all reading sections visible. Reload to verify ordinary first-entry reveals.
  await page.reload()
  await page.locator('.hero h1').waitFor()
  await page.evaluate(() => document.fonts.ready)
  await assertReadingStill(page)
  assert.equal(await page.locator('.language-section .hello-example').count(), 1, 'The static source example belongs to the writing section')

  // A scroll reveal must animate into a readable state instead of remaining hidden.
  const reveal = page.locator('.interop-heading.reveal')
  await reveal.scrollIntoViewIfNeeded()
  await page.waitForFunction(() => {
    const element = document.querySelector('.interop-heading.reveal')
    return element?.classList.contains('is-visible') && element.getAnimations().some(animation => animation.playState === 'running')
  }, null, { polling: 'raf' })
  const revealFirst = await animationState(page, '.interop-heading.reveal')
  await page.waitForTimeout(120)
  const revealNext = await animationState(page, '.interop-heading.reveal')
  assert(revealFirst.opacity !== revealNext.opacity || revealFirst.transform !== revealNext.transform || revealFirst.translate !== revealNext.translate, 'Scroll reveal actually changes the rendered element')
  await page.waitForFunction(() => Number(getComputedStyle(document.querySelector('.interop-heading.reveal')).opacity) > .99)

  // The compiler illustration follows the five stages in order.
  await page.locator('.compile-section').scrollIntoViewIfNeeded()
  const compile = page.locator('.compile-track')
  const compileSelector = '.compile-track'
  assert.equal(await page.locator('.compile-stage').count(), 5, 'The illustration has five compilation stages')
  const phases = await page.evaluate(async selector => {
    const element = document.querySelector(selector)
    const seen = [Number(element.dataset.phase)]
    const deadline = performance.now() + 16000
    while (seen.length < 6 && performance.now() < deadline) {
      await new Promise(resolve => setTimeout(resolve, 80))
      const phase = Number(element.dataset.phase)
      if (phase !== seen.at(-1)) seen.push(phase)
    }
    return seen
  }, compileSelector)
  assert(phases.length >= 6, 'The compiler phase advances through a full cycle')
  assert(phases.every((phase, index) => phase >= 0 && phase < 5 && (!index || phase === (phases[index - 1] + 1) % 5)), 'Compiler stages animate in source-to-native order')
  await page.locator('.motion-control').click()
  assert.equal(await page.locator('.motion-control').getAttribute('aria-pressed'), 'true')
  assert.equal(await page.locator('.site-shell').getAttribute('data-motion'), 'paused')
  const pausedPhase = await compile.getAttribute('data-phase')
  await page.waitForTimeout(1800)
  assert.equal(await compile.getAttribute('data-phase'), pausedPhase, 'Global pause freezes the compiler phase timer')
  await page.locator('.motion-control').click()
  await page.waitForFunction(([selector, phase]) => document.querySelector(selector)?.dataset.phase !== phase, [compileSelector, pausedPhase])
  await page.locator('.compile-section').screenshot({ path: artifacts + 'visual-compile.png', animations: 'disabled' })
  await page.setViewportSize({ width: 390, height: 844 })
  await page.locator('.compile-section').screenshot({ path: artifacts + 'visual-compile-mobile.png', animations: 'disabled' })
  await page.setViewportSize({ width: 1440, height: 1000 })

  // Both journey segments use one clock; pausing freezes their rendered state.
  await page.locator('.interop-journey').scrollIntoViewIfNeeded()
  assert.equal(await page.locator('.interop-journey .flow-segment').count(), 2)
  const packets = page.locator('.interop-journey .flow-packet')
  assert.equal(await packets.count(), 2, 'Each native interop boundary has a flow packet')
  await page.waitForTimeout(700)
  const flowSamples = await packets.evaluateAll(async elements => {
    const samples = []
    const deadline = performance.now() + 8800
    while (performance.now() < deadline) {
      samples.push(elements.map(element => ({ x: element.getBoundingClientRect().x, opacity: Number(getComputedStyle(element).opacity) })))
      await new Promise(requestAnimationFrame)
    }
    return samples
  })
  const visibleOrder = []
  for (const sample of flowSamples) {
    const dominant = sample[0].opacity >= sample[1].opacity ? 0 : 1
    if (sample[dominant].opacity > .1 && visibleOrder.at(-1) !== dominant) visibleOrder.push(dominant)
  }
  assert(visibleOrder.length >= 3, 'One shared cycle shows production and consumption in sequence, then repeats')
  assert(visibleOrder.every((segment, index) => !index || segment !== visibleOrder[index - 1]), 'The journey advances from one boundary to the next')
  for (let index = 0; index < 2; index++) {
    const visible = flowSamples.map(sample => sample[index]).filter(sample => sample.opacity > .1)
    assert(visible.length > 10, `Flow segment ${index + 1} visibly appears during the cycle`)
    assert(Math.max(...visible.map(sample => sample.x)) - Math.min(...visible.map(sample => sample.x)) > 20, `Flow segment ${index + 1} travels along its boundary`)
    const trackWidth = await packets.nth(index).evaluate(element => element.closest('.flow-track').getBoundingClientRect().width)
    for (let frame = 1; frame < flowSamples.length; frame++) {
      const first = flowSamples[frame - 1][index], next = flowSamples[frame][index]
      if (first.opacity > .1 && next.opacity > .1) assert(Math.abs(next.x - first.x) < trackWidth * .2, `Flow segment ${index + 1} moves continuously between visible frames`)
    }
  }
  await page.locator('.motion-control').click()
  const captureFlow = () => page.evaluate(() => ({
    progress: document.querySelector('.site-shell').style.getPropertyValue('--motion-progress'),
    pose: document.querySelector('.technical-glyph').dataset.pose,
    packets: [...document.querySelectorAll('.interop-journey .flow-packet')].map(element => {
      const style = getComputedStyle(element)
      return { left: style.left, transform: style.transform, opacity: style.opacity, animations: element.getAnimations().map(animation => ({ time: animation.currentTime, state: animation.playState })) }
    }),
  }))
  const frozen = await captureFlow()
  await page.waitForTimeout(600)
  const later = await captureFlow()
  assert.equal(later.progress, frozen.progress, 'Global pause freezes the shared progress clock')
  assert.equal(later.pose, frozen.pose, 'Global pause freezes the technical glyph pose')
  for (let index = 0; index < frozen.packets.length; index++) {
    const first = frozen.packets[index], next = later.packets[index]
    assert.equal(next.left, first.left, `Paused flow ${index + 1} keeps its position`)
    assert.equal(next.transform, first.transform, `Paused flow ${index + 1} keeps its transform`)
    assert.equal(next.opacity, first.opacity, `Paused flow ${index + 1} keeps its visibility`)
    assert.equal(next.animations.length, first.animations.length)
    for (let animation = 0; animation < first.animations.length; animation++) {
      assert.equal(next.animations[animation].state, 'paused', 'Any CSS packet animation pauses with the clock')
      assert(Math.abs(Number(next.animations[animation].time) - Number(first.animations[animation].time)) < 2, 'Any CSS packet animation keeps its time')
    }
  }
  const inherited = await context.newPage()
  observeErrors(inherited)
  await inherited.goto(url + '/mosp.html')
  await inherited.locator('.motion-control').waitFor()
  assert.equal(await inherited.locator('.site-shell').getAttribute('data-motion'), 'paused', 'MOSP reads the homepage pause preference')
  assert.equal(await inherited.locator('.motion-control').getAttribute('aria-pressed'), 'true', 'The shared pause preference is visible on the control')
  await inherited.close()
  await page.locator('.motion-control').click()
  assert.equal(await page.locator('.site-shell').getAttribute('data-motion'), 'running')
  await page.waitForFunction(progress => Math.abs(Number(document.querySelector('.site-shell')?.style.getPropertyValue('--motion-progress')) - Number(progress)) > .02, frozen.progress)
  await assertReadingStill(page)
  await page.locator('.interop-section').screenshot({ path: artifacts + 'visual-interop.png', animations: 'disabled' })
  await page.setViewportSize({ width: 390, height: 844 })
  await page.locator('.interop-section').screenshot({ path: artifacts + 'visual-interop-mobile.png', animations: 'disabled' })
  await page.setViewportSize({ width: 1440, height: 1000 })
  await page.evaluate(() => scrollTo(0, 0))
  await page.screenshot({ path: artifacts + 'visual-home-hero.png', animations: 'disabled' })

  // A document change uses a real curtain, and the pause preference disables it.
  await page.locator('.desktop-links a[href="./mosp.html"]').click({ noWaitAfter: true })
  await page.waitForFunction(() => document.getElementById('vyx-transition')?.getAnimations().some(animation => animation.playState === 'running'), null, { polling: 'raf', timeout: 2000 })
  await page.waitForURL(url + '/mosp.html')
  await page.locator('.mosp-hero h1').waitFor()
  await page.waitForFunction(() => !document.getElementById('vyx-transition'))

  // MOSP transitions preserve content during ordinary and rapid selections.
  await page.goto(url + '/mosp.html#dci')
  await assertFeatureSettled(page, 'dci')
  await page.locator('#mosp-tab-reflection').click()
  await page.waitForFunction(() => document.getElementById('mosp-panel-reflection')?.getAnimations().some(animation => animation.playState === 'running'), null, { polling: 'raf', timeout: 2000 })
  const transitionFirst = await animationState(page, '#mosp-panel-reflection')
  await page.waitForTimeout(70)
  const transitionNext = await animationState(page, '#mosp-panel-reflection')
  assert(transitionFirst.opacity !== transitionNext.opacity || transitionFirst.transform !== transitionNext.transform || transitionFirst.translate !== transitionNext.translate, 'The selected feature uses a real continuous transition')
  await assertFeatureSettled(page, 'reflection')
  const rapid = await page.evaluate(async () => {
    const ids = ['migrate', 'dci', 'reflection', 'dce', 'migrate', 'reflection', 'dci', 'reflection']
    let blankSince = null, longestBlank = 0
    for (const id of ids) {
      document.getElementById(`mosp-tab-${id}`).click()
      const until = performance.now() + 65
      while (performance.now() < until) {
        await new Promise(requestAnimationFrame)
        const painted = [...document.querySelectorAll('.mosp-tabpanel')].some(panel => {
          const style = getComputedStyle(panel)
          return style.display !== 'none' && style.visibility !== 'hidden' && Number(style.opacity) > .02 && panel.getBoundingClientRect().height > 0
        })
        if (painted) blankSince = null
        else { blankSince ??= performance.now(); longestBlank = Math.max(longestBlank, performance.now() - blankSince) }
      }
    }
    return { longestBlank }
  })
  assert(rapid.longestBlank < 100, 'Rapid tab selection does not leave an empty panel')
  await assertFeatureSettled(page, 'reflection')
  await page.locator('.motion-control').click()
  assert.equal(await page.locator('.motion-control').getAttribute('aria-pressed'), 'true', 'MOSP has the same global pause control')
  await page.locator('#mosp-tab-migrate').click()
  await assertFeatureSettled(page, 'migrate')
  const pausedPanel = await animationState(page, '#mosp-panel-migrate')
  await page.waitForTimeout(350)
  assertFrozen(pausedPanel, await animationState(page, '#mosp-panel-migrate'), 'Paused MOSP panel')
  await page.locator('.navigation .brand').click()
  await page.locator('.hero h1').waitFor()
  assert.equal(await page.locator('.site-shell').getAttribute('data-motion'), 'paused', 'The pause preference persists when returning home')
  assert.equal(await page.locator('#vyx-transition').evaluate(element => getComputedStyle(element).display), 'none', 'Paused navigation does not animate a curtain')
  await page.locator('.desktop-links a[href="./mosp.html"]').click()
  await assertFeatureSettled(page, 'dci')
  assert.equal(await page.locator('#vyx-transition').evaluate(element => getComputedStyle(element).display), 'none', 'Paused navigation stays still in both directions')
  await page.locator('.motion-control').click()
  await page.locator('#mosp-tab-reflection').click()
  await page.waitForFunction(() => document.getElementById('mosp-panel-reflection')?.getAnimations().some(animation => animation.playState === 'running'), null, { polling: 'raf', timeout: 2000 })
  await assertFeatureSettled(page, 'reflection')
  await assertReadingStill(page)
  await page.locator('#mosp-features').screenshot({ path: artifacts + 'visual-mosp-tabs.png', animations: 'disabled' })
  await page.setViewportSize({ width: 390, height: 844 })
  await page.locator('#mosp-features').screenshot({ path: artifacts + 'visual-mosp-tabs-mobile.png', animations: 'disabled' })
  await page.setViewportSize({ width: 1440, height: 1000 })
  await page.evaluate(() => scrollTo(0, 0))
  await page.screenshot({ path: artifacts + 'visual-mosp-hero.png', animations: 'disabled' })

  // The tutorial shares the same page clock: chapter figures step through their
  // sequence, and the global pause freezes them without touching the prose.
  await page.goto(url + '/tutorial/#basics')
  await page.locator('.tutorial-figure[data-figure="pipeline"]').waitFor()
  await page.evaluate(() => document.fonts.ready)
  assert.equal(await page.locator('.site-shell').getAttribute('data-motion'), 'running', 'The tutorial runs the shared motion clock')
  const figureSignature = () => page.locator('.tutorial-figure[data-figure="pipeline"] .pipe-track li').evaluateAll(nodes => nodes.map(node => [...node.classList].sort().join('.')).join('|'))
  const stepped = await page.evaluate(async () => {
    const signature = () => [...document.querySelectorAll('.tutorial-figure[data-figure="pipeline"] .pipe-track li')].map(node => [...node.classList].sort().join('.')).join('|')
    const start = signature()
    const deadline = performance.now() + 9500
    while (performance.now() < deadline) {
      if (signature() !== start) return true
      await new Promise(requestAnimationFrame)
    }
    return false
  })
  assert(stepped, 'The chapter figure steps through its illustration on the shared clock')
  const tutorialProgress = () => page.evaluate(() => document.querySelector('.site-shell').style.getPropertyValue('--motion-progress'))
  await page.locator('.motion-control').click()
  assert.equal(await page.locator('.site-shell').getAttribute('data-motion'), 'paused', 'The tutorial honours the global pause')
  const frozenTutorial = { progress: await tutorialProgress(), signature: await figureSignature() }
  await page.waitForTimeout(700)
  assert.equal(await tutorialProgress(), frozenTutorial.progress, 'Pausing the tutorial freezes the shared progress clock')
  assert.equal(await figureSignature(), frozenTutorial.signature, 'Paused tutorial figures keep their step')
  const pausedPoseText = await page.locator('.tutorial-section').first().innerText()
  await page.waitForTimeout(300)
  assert.equal(await page.locator('.tutorial-section').first().innerText(), pausedPoseText, 'Paused tutorial prose stays identical')
  await page.locator('.motion-control').click()
  await page.waitForFunction(progress => document.querySelector('.site-shell').style.getPropertyValue('--motion-progress') !== progress, frozenTutorial.progress)
  await assertReadingStill(page)
  await page.locator('.tutorial-layout').screenshot({ path: artifacts + 'visual-tutorial-figure.png', animations: 'disabled' })

  // Each chapter opens with its own figure, and matching sections carry an inline one.
  // A chapter change is a hash-only navigation (same document), so wait for the
  // target chapter's own figure before asserting it replaced the previous one.
  for (const [chapter, hero] of [['basics', 'pipeline'], ['intermediate', 'result'], ['advanced', 'ownership'], ['migration', 'languages']]) {
    await page.goto(url + '/tutorial/#' + chapter)
    await page.locator(`#chapter-head .tutorial-figure[data-figure="${hero}"]`).waitFor()
    assert.equal(await page.locator('#chapter-head .tutorial-figure').count(), 1, `${chapter}: the chapter opens with its own figure`)
  }
  for (const [chapter, section, figure] of [['basics', 's07-lesson-5-loops-break-and-continue', 'loop'], ['intermediate', 's01-lesson-11-collection-containers', 'containers'], ['advanced', 's07-lesson-25-async-task-promise-and-vio', 'result']]) {
    await page.goto(url + `/tutorial/#${chapter}/${section}`)
    await page.locator(`#${section} .tutorial-figure[data-figure="${figure}"]`).waitFor()
    assert.equal(await page.locator(`#${section} .tutorial-figure`).count(), 1, `${chapter}: the ${figure} figure explains its section`)
  }
  assert.equal(await page.locator('.tutorial-figure').count(), 2, 'A section figure joins its chapter hero illustration')
  await page.goto(url + '/tutorial/#migration')
  await page.locator('#chapter-head .tutorial-figure[data-figure="languages"]').waitFor()
  assert.equal(await page.locator('.tutorial-figure').count(), 1, 'A chapter without an inline figure renders only its hero illustration')
  await page.goto(url + '/tutorial/#basics')
  await page.locator('.tutorial-figure[data-figure="pipeline"]').waitFor()
  await page.setViewportSize({ width: 390, height: 844 })
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Tutorial figures fit the mobile column')
  await page.locator('.chapter-head').screenshot({ path: artifacts + 'visual-tutorial-mobile.png', animations: 'disabled' })
  await page.setViewportSize({ width: 1440, height: 1000 })
  await page.goto(url + '/mosp.html')
  await page.locator('.mosp-hero h1').waitFor()

  // Reducing motion while the page is active settles every explanatory flow.
  await page.emulateMedia({ reducedMotion: 'reduce' })
  await page.waitForFunction(() => document.querySelector('.site-shell')?.dataset.motion === 'reduced')
  await page.locator('#mosp-tab-dci').click()
  await assertFeatureSettled(page, 'dci')
  await assertNoRunningMotion(page)
  await page.goto(url)
  await page.locator('.hero h1').waitFor()
  assert.equal(await page.locator('.site-shell').getAttribute('data-motion'), 'reduced')
  assert.equal(await glyph.getAttribute('data-pose'), '0.0000', 'Reduced motion uses a fixed ASCII pose')
  const reducedPixels = await canvas.screenshot()
  await page.waitForTimeout(250)
  assert.deepEqual(await canvas.screenshot(), reducedPixels, 'Reduced motion leaves the canvas still')
  await assertNoRunningMotion(page)
  assert.equal(await page.locator(compileSelector).first().getAttribute('data-phase'), '4', 'Reduced motion shows the completed compiler illustration')
  const hiddenReveals = await page.locator('.reveal').evaluateAll(elements => elements.filter(element => Number(getComputedStyle(element).opacity) < .99).length)
  assert.equal(hiddenReveals, 0, 'Reduced motion makes every reading section visible immediately')
  await page.goto(url + '/tutorial/#basics')
  await page.locator('.tutorial-figure[data-figure="pipeline"]').waitFor()
  assert.equal(await page.locator('.site-shell').getAttribute('data-motion'), 'reduced', 'The tutorial follows the reduced-motion preference')
  assert.equal(await page.locator('.tutorial-figure[data-figure="pipeline"] .pipe-track li.lit').count(), 0, 'Reduced motion settles the tutorial figure on its final step')
  assert.equal(await page.locator('.tutorial-figure[data-figure="pipeline"] .pipe-track li.past').count(), 5, 'The settled figure shows the whole sequence as completed')
  assert.equal(await page.locator('.tutorial-section.reveal:not(.is-visible)').count(), 0, 'Reduced motion makes every tutorial section readable immediately')
  await assertReadingStill(page)
  await assertNoRunningMotion(page)
  for (const entry of ['/', '/mosp.html']) {
    await page.goto(url + entry)
    await page.locator('h1').waitFor()
    await page.setViewportSize({ width: 390, height: 844 })
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'The editorial layout fits mobile')
    await page.screenshot({ path: artifacts + `visual-${entry === '/' ? 'home' : 'mosp'}-mobile.png`, animations: 'disabled' })
    await page.locator('.theme-button').click()
    await page.setViewportSize({ width: 1440, height: 1000 })
    await page.screenshot({ path: artifacts + `visual-${entry === '/' ? 'home' : 'mosp'}-dark.png`, animations: 'disabled' })
    await page.locator('.theme-button').click()
  }
  // A device without WebGL retains a readable decorative V and normal controls.
  const fallbackPage = await context.newPage()
  observeErrors(fallbackPage)
  await fallbackPage.addInitScript(() => {
    const getContext = HTMLCanvasElement.prototype.getContext
    HTMLCanvasElement.prototype.getContext = function (type, ...args) {
      return type === 'webgl' ? null : getContext.call(this, type, ...args)
    }
  })
  await fallbackPage.goto(url)
  assert.equal(await fallbackPage.locator('.technical-glyph').getAttribute('data-renderer'), 'fallback')
  assert.match(await fallbackPage.locator('.glyph-fallback').innerText(), /[+#%@]/, 'No WebGL still displays an ASCII V')
  await fallbackPage.close()
  assert.deepEqual(errors, [], 'Motion tests have no browser errors or failed resources')
  console.log('Motion checks passed: WebGL ASCII pixel motion, pause/resume and static fallback, real scroll reveals, five sequential compiler stages, directional flows, MOSP transitions, tutorial chapter figures on the shared clock, static reading surfaces, and live reduced-motion settling.')
  console.log('Editorial desktop/mobile/dark screenshots saved in website/artifacts/.')
} finally {
  await browser?.close()
  server.kill()
}
