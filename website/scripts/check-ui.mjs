import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readFileSync, statSync } from 'node:fs'
import { createServer } from 'node:http'
import { extname, resolve, sep } from 'node:path'
import { fileURLToPath } from 'node:url'
import { chromium } from 'playwright'
import { features } from '../src/features.js'
import { chapterOrder } from '../src/tutorial-chapters.js'

const root = fileURLToPath(new URL('../', import.meta.url))
const artifacts = fileURLToPath(new URL('../artifacts/', import.meta.url))
mkdirSync(artifacts, { recursive: true })
assert(existsSync(new URL('../dist/index.html', import.meta.url)), 'Run npm run build first.')
assert(existsSync(new URL('../dist/mosp.html', import.meta.url)), 'Build the standalone MOSP page first.')
assert(existsSync(new URL('../dist/tutorial/index.html', import.meta.url)), 'Build the tutorial entry first.')
const url = 'http://127.0.0.1:4173'
const server = spawn(process.execPath, ['node_modules/vite/bin/vite.js', 'preview', '--host', '127.0.0.1'], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true })
let serverOutput = ''
// Vite colourises the port separately ("http://127.0.0.1:<esc>4173<esc>/"), so the
// raw stream never contains the plain URL. Strip escapes before matching.
const stripAnsi = value => String(value).replace(/\u001b\[[0-9;]*m/g, '')
server.stdout.on('data', chunk => { serverOutput += stripAnsi(chunk) })
server.stderr.on('data', chunk => { serverOutput += stripAnsi(chunk) })
const delay = ms => new Promise(resolve => setTimeout(resolve, ms))
let browser
let nestedServer
const errors = []
const widths = [1920, 1440, 1024, 768, 680, 390, 360, 320]
const observeErrors = page => {
  page.on('pageerror', error => errors.push(error.message))
  page.on('console', message => { if (message.type() === 'error') errors.push(message.text()) })
  page.on('response', response => { if (response.status() >= 400) errors.push(`${response.status()} ${response.url()}`) })
}
const assertWidth = async (page, width, label) => {
  await page.setViewportSize({ width, height: width > 680 ? 1000 : 844 })
  assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `${label}: horizontal overflow at ${width}px`)
}
const assertMospPage = async (page, label) => {
  await page.locator('.mosp-page h1').waitFor()
  assert((await page.locator('.mosp-page h1').innerText()).trim(), `${label}: the page has a reading title`)
  assert.match(await page.title(), /MOSP/, label)
  assert.match(await page.locator('.mosp-hero .micro-label').innerText(), /MOSP/, label)
}
const assertHomePage = async (page, label = 'The homepage shows its systems programming identity') => {
  await page.locator('.hero h1').waitFor()
  assert.match((await page.locator('.hero h1').innerText()).replaceAll('\n', ' '), /SYSTEMS,\s*CONNECTED\./, label)
  assert.match(await page.locator('.navigation .brand').innerText(), /Vyx/i, 'Navigation retains the Vyx wordmark')
}
const mospFeatures = ['migrate', 'reflection', 'dci', 'dce']
const assertMospFeature = async (page, selected, label = `MOSP ${selected}`) => {
  await page.waitForFunction(id => document.getElementById(`mosp-tab-${id}`)?.getAttribute('aria-selected') === 'true', selected)
  assert.equal(await page.locator('.mosp-page [role="tab"]').count(), mospFeatures.length, `${label}: four feature tabs`)
  assert.equal(await page.locator('.mosp-page [role="tabpanel"]').count(), mospFeatures.length, `${label}: four feature panels`)
  for (const id of mospFeatures) {
    const tab = page.locator(`#mosp-tab-${id}`)
    const panel = page.locator(`#mosp-panel-${id}`)
    assert.equal(await tab.getAttribute('role'), 'tab', `${id}: tab semantics`)
    assert.equal(await tab.getAttribute('aria-controls'), `mosp-panel-${id}`, `${id}: tab points to its panel`)
    assert.equal(await tab.getAttribute('aria-selected'), String(id === selected), `${id}: selection state`)
    assert.equal(await tab.getAttribute('tabindex'), id === selected ? '0' : '-1', `${id}: roving focus`)
    assert.equal(await panel.getAttribute('role'), 'tabpanel', `${id}: panel semantics`)
    assert.equal(await panel.getAttribute('aria-labelledby'), `mosp-tab-${id}`, `${id}: panel names its tab`)
    assert.equal(await panel.getAttribute('aria-hidden'), String(id !== selected), `${id}: inactive content is excluded from accessibility`)
    assert.equal(await panel.evaluate(element => element.inert), id !== selected, `${id}: inactive content cannot receive focus`)
    assert.equal(await panel.isVisible(), id === selected, `${label}: only the active panel is visible`)
  }
}
const selectMospFeature = async (page, id) => {
  await page.locator(`#mosp-tab-${id}`).click()
  await assertMospFeature(page, id)
  assert.equal(new URL(page.url()).hash, '#' + id, `${id}: selection is addressable`)
}
const checkMospExamples = async (page, feature) => {
  const examples = page.locator(`#mosp-panel-${feature} .mosp-code[data-example]`)
  const count = await examples.count()
  assert(count > 0, `${feature}: detailed examples are available`)
  for (let index = 0; index < count; index++) {
    const example = examples.nth(index)
    const source = (await example.locator('pre code').innerText()).replaceAll('\r\n', '\n').trim()
    assert(source.length > 20, `${feature}: the example contains a complete reading snippet`)
    const button = example.locator('.mosp-copy[data-copy]')
    assert.equal(await button.count(), 1, `${feature}: each example has one copy action`)
    await button.click()
    assert.equal((await page.evaluate(() => navigator.clipboard.readText())).replaceAll('\r\n', '\n').trim(), source, `${feature}: example ${index + 1} copies without line numbers or omissions`)
    assert.match(await button.innerText(), /Copied|已复制/, `${feature}: example ${index + 1} reports copying`)
  }
}
try {
  let ready = false
  for (let attempt = 0; attempt < 100; attempt++) {
    if (server.exitCode !== null) throw new Error('Preview failed: ' + serverOutput)
    if (serverOutput.includes('http://127.0.0.1:4173')) { ready = true; break }
    await delay(100)
  }
  assert(ready, 'Preview server did not start: ' + serverOutput)
  const installedChrome = 'C:/Program Files/Google/Chrome/Application/chrome.exe'
  browser = await chromium.launch({ headless: true, ...(existsSync(installedChrome) ? { executablePath: installedChrome } : {}) })
  const context = await browser.newContext({ viewport: { width: 1440, height: 1000 }, deviceScaleFactor: 1, permissions: ['clipboard-read', 'clipboard-write'], reducedMotion: 'reduce' })
  const page = await context.newPage()
  observeErrors(page)
  await page.goto(url)
  await page.evaluate(() => document.fonts.ready)
  await page.locator('h1').waitFor()
  const checkWidth = async width => {
    await assertWidth(page, width, 'Home')
  }
  await assertHomePage(page)
  assert.match(await page.locator('.hero-definition').innerText(), /编译为原生程序/)
  assert.equal(await page.locator('.desktop-links a[href$="mosp.html"]').count(), 1, 'The MOSP / DCI page is in desktop navigation')
  assert.equal(await page.locator('html').getAttribute('data-theme'), 'light')
  assert(await page.locator('.brand-mark').first().evaluate(img => img.complete && img.naturalWidth > 0), 'Vyx logo loaded')
  await page.locator('.hello-file button').click()
  assert.equal((await page.evaluate(() => navigator.clipboard.readText())).replaceAll('\r\n', '\n'), 'fn main() -> i32 {\n    print("Hello, Vyx!");\n    return 0;\n}', 'First program copies as compilable source')
  assert.equal(await page.locator('.learning-links a').count(), 3, 'Tutorial, projects, and editor guides are linked')
  await page.screenshot({ path: artifacts + 'desktop.png', fullPage: true, animations: 'disabled' })
  await page.screenshot({ path: artifacts + 'hero.png', animations: 'disabled' })
  await page.locator('.hero-feature-index a').nth(1).click()
  assert.equal(await page.locator('#tab-reflection').getAttribute('aria-selected'), 'true', 'Hero shortcuts select the matching feature')
  await page.getByRole('tab', { name: /Migrate/ }).click()
  assert.equal(await page.locator('.language-facts > div').count(), 3)
  await page.locator('.locale-button').click()
  assert.match(await page.locator('.hero-definition').innerText(), /Compile to native code/)
  assert.equal(await page.getByRole('tab').count(), 4)
  await page.getByRole('tab', { name: /Reflection/ }).click()
  await page.getByRole('button', { name: 'main.vyx', exact: true }).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('getMethod'))
  assert.match(await page.locator('.editor-code').innerText(), /method\.as/)
  await page.locator('.copy-code').click()
  assert.equal((await page.evaluate(() => navigator.clipboard.readText())).replaceAll('\r\n', '\n'), features[1].files[1].code)
  await page.getByRole('tab', { name: /Reflection/ }).focus()
  await page.keyboard.press('ArrowRight')
  assert.equal(await page.locator('#tab-dci').getAttribute('aria-selected'), 'true')
  assert.equal(await page.locator('.dci-case-list button').count(), 3, 'Inheritance, override, and open generics are shown together')
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('AbstractSink'))
  assert.match(await page.locator('.editor-kind').innerText(), /C\+\+/)
  assert.match(await page.locator('.editor-code').innerText(), /virtual int consume/)
  await page.locator('.copy-code').click()
  assert.equal((await page.evaluate(() => navigator.clipboard.readText())).replaceAll('\r\n', '\n'), features[2].cases[0].providers[0].files[0].code)
  await page.locator('.source-files button').nth(1).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('dci_import'))
  assert.match(await page.locator('.editor-code').innerText(), /Complex\.dcib/)
  await page.locator('.source-files button').nth(2).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('VyxSink'))
  assert.match(await page.locator('.editor-code').innerText(), /class VyxSink : abi_complex\.AbstractSink/)
  await page.locator('.dci-case-list button').nth(1).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('pub trait Sink'))
  assert.match(await page.locator('.editor-code').innerText(), /sink\.consume\(x\) \+ 30/)
  await page.locator('.source-files button').nth(2).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('VyxHost'))
  assert.match(await page.locator('.editor-code').innerText(), /override fn consume/)
  assert.match(await page.locator('.editor-code').innerText(), /native_dispatch\(sink as rawptr, 2\)/)
  await page.locator('.dci-case-list button').nth(2).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('twice'))
  assert.match(await page.locator('.editor-kind').innerText(), /Rust/)
  assert.match(await page.locator('.editor-code').innerText(), /std::ops::Add/)
  assert.match(await page.locator('.editor-code').innerText(), /impl<A, B>/)
  await page.locator('.source-files button').nth(1).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('dci_import'))
  assert.match(await page.locator('.editor-code').innerText(), /open_generic\.dcib/)
  assert.match(await page.locator('.editor-code').innerText(), /struct Pair2<A, B>/)
  await page.locator('.provider-toggle').getByRole('button', { name: 'C++', exact: true }).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('#pragma'))
  assert.match(await page.locator('.editor-kind').innerText(), /C\+\+/)
  assert.match(await page.locator('.editor-code').innerText(), /template <typename A, typename B>/)
  await page.locator('.source-files button').nth(1).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('dci_import'))
  assert.match(await page.locator('.editor-code').innerText(), /open_generic\.cpp\.dcib/)
  assert.match(await page.locator('.editor-code').innerText(), /norm1/)
  await page.locator('.source-files button').nth(2).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('pair.swapped'))
  assert.doesNotMatch(await page.locator('.editor-code').innerText(), /return [1-9]/)
  await page.locator('.core-features').screenshot({ path: artifacts + 'dci-cpp-usage.png' })
  await page.locator('.provider-toggle').getByRole('button', { name: 'Rust', exact: true }).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('pub struct'))
  await page.locator('.core-features').screenshot({ path: artifacts + 'dci-rust-definition.png' })
  await page.locator('#tab-dci').focus()
  await page.keyboard.press('End')
  await page.locator('.dce-panel').waitFor()
  assert.equal(await page.locator('.editor-code').count(), 0, 'DCE has a compiler visualization, not source code')
  assert.equal(await page.locator('.copy-code').count(), 0, 'DCE has no fake copy-source action')
  assert.equal(await page.locator('.dce-symbol').count(), 7)
  assert.equal(await page.locator('.dce-panel').getAttribute('data-phase'), '3', 'Reduced motion shows the completed DCE illustration')
  await page.locator('.dce-steps button').nth(1).click()
  assert.equal(await page.locator('.dce-panel').getAttribute('data-phase'), '1')
  await page.locator('#tab-dce').focus()
  await page.keyboard.press('Home')
  assert.equal(await page.locator('#tab-migrate').getAttribute('aria-selected'), 'true')
  await page.getByRole('button', { name: 'shipping_v2.vyx', exact: true }).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('@[migrate'))
  assert.match(await page.locator('.editor-code').innerText(), /fromSig/)
  await page.getByRole('button', { name: 'main.vyx', exact: true }).click()
  await page.waitForFunction(() => document.querySelector('.editor-code')?.innerText.includes('quote@1.0.0'))
  await page.locator('.copy-code').click()
  assert.equal((await page.evaluate(() => navigator.clipboard.readText())).replaceAll('\r\n', '\n'), features[0].files[2].code)
  await page.getByRole('button', { name: 'shipping_v1.vyx', exact: true }).click()
  await page.locator('.command-box button').click()
  assert.equal(await page.evaluate(() => navigator.clipboard.readText()), 'vyxc --src=file hello.vyx --run=aot')
  await page.locator('.desktop-links button').click()
  assert(await page.locator('dialog').evaluate(dialog => dialog.open))
  await page.keyboard.press('Escape')
  assert.equal(await page.locator('dialog').evaluate(dialog => dialog.open), false)
  assert(await page.locator('.desktop-links button').evaluate(button => document.activeElement === button), 'Dialog restores focus')
  await page.locator('.interop-copy a[href$="mosp.html"]').click()
  await page.locator('h1').waitFor()
  await assertMospPage(page, 'The interoperability CTA opens the dedicated page')
  await page.goBack()
  await page.locator('.hero-definition').waitFor()
  await assertHomePage(page, 'Back returns to the homepage')
  await page.goForward()
  await page.locator('h1').waitFor()
  await assertMospPage(page, 'Forward restores the dedicated page')
  await page.goBack()
  await page.locator('.hero-definition').waitFor()
  await page.locator('.locale-button').click()
  assert.match(await page.locator('.hero-definition').innerText(), /编译为原生程序/)
  await page.locator('.theme-button').click()
  assert.equal(await page.locator('html').getAttribute('data-theme'), 'dark')
  await page.reload()
  assert.equal(await page.locator('html').getAttribute('lang'), 'zh-CN')
  assert.equal(await page.locator('html').getAttribute('data-theme'), 'dark')
  await page.evaluate(() => scrollTo(0, 0))
  await page.screenshot({ path: artifacts + 'dark-zh.png', fullPage: true, animations: 'disabled' })
  await page.locator('.theme-button').click()
  await page.locator('.locale-button').click()
  for (const width of widths) await checkWidth(width)
  await checkWidth(390)
  await page.evaluate(() => scrollTo(0, 0))
  await page.screenshot({ path: artifacts + 'mobile.png', fullPage: true, animations: 'disabled' })
  await page.locator('.mobile-menu-button').click()
  assert.equal(await page.locator('.mobile-menu-button').getAttribute('aria-expanded'), 'true')
  assert.equal(await page.locator('#mobile-menu a[href$="mosp.html"]').count(), 1, 'The MOSP / DCI page is in mobile navigation')
  await page.locator('#mobile-menu a[href="#language"]').click()
  assert.equal(await page.locator('#mobile-menu').count(), 0)
  assert(new URL(page.url()).hash === '#language')
  await page.locator('.mobile-menu-button').click()
  await page.keyboard.press('Escape')
  assert.equal(await page.locator('#mobile-menu').count(), 0)
  await page.locator('.quickstart-button').click()
  assert(await page.locator('dialog').evaluate(dialog => dialog.scrollWidth <= dialog.clientWidth), 'Mobile dialog fits')
  const installation = await page.locator('dialog').innerText()
  assert.match(installation, /Windows and Linux SDKs include the LLVM backend/, 'Installation states that both SDKs include LLVM')
  assert.doesNotMatch(installation, /Windows also needs|MSVC \/ Windows SDK linking environment/, 'Installation does not repeat the retired Windows prerequisites')
  await page.screenshot({ path: artifacts + 'mobile-dialog.png', animations: 'disabled' })
  await page.keyboard.press('Escape')
  await page.locator('.locale-button').click()
  for (const width of [1440, 768, 390, 320]) await checkWidth(width)
  assert.equal(await page.locator('a[target="_blank"]').evaluateAll(links => links.every(link => (link.href === 'https://github.com/vyx-lang/vyx' || link.href.startsWith('https://github.com/vyx-lang/vyx/')) && link.rel.includes('noopener'))), true)
  // Reading surfaces remain still; only the explanatory flows animate.
  const live = await context.newPage()
  await live.emulateMedia({ reducedMotion: 'no-preference' })
  observeErrors(live)
  await live.goto(url)
  assert.equal(await live.locator('.hello-example').evaluate(element => getComputedStyle(element).animationName), 'none', 'The source example stays still')
  await live.locator('.interop-section').scrollIntoViewIfNeeded()
  assert.equal(await live.locator('.interop-journey .flow-segment').count(), 2, 'Interop shows both producer-to-contract and contract-to-call segments')
  const flowState = () => live.locator('.interop-journey .flow-packet').first().evaluate(element => {
    const style = getComputedStyle(element)
    const shell = document.querySelector('.site-shell')
    return { left: style.left, opacity: style.opacity, transform: style.transform, state: shell.dataset.motion, progress: shell.style.getPropertyValue('--motion-progress') }
  })
  const initialFlow = await flowState()
  await live.waitForFunction(progress => document.querySelector('.site-shell')?.style.getPropertyValue('--motion-progress') !== progress, initialFlow.progress)
  await live.locator('.motion-control').click()
  const pausedFlow = await flowState()
  assert.equal(pausedFlow.state, 'paused')
  await live.waitForTimeout(500)
  assert.deepEqual(await flowState(), pausedFlow, 'Pause freezes the flow animation')
  await live.locator('.motion-control').click()
  await live.locator('#tab-dce').click()
  await live.locator('.dce-panel').waitFor()
  await live.waitForFunction(() => document.querySelector('.dce-panel')?.dataset.phase === '1', null, { timeout: 6000 })
  await live.locator('.replay-dce').click()
  assert.equal(await live.locator('.dce-panel').getAttribute('data-phase'), '0', 'DCE replay resets the timeline')
  await live.emulateMedia({ reducedMotion: 'reduce' })
  await live.waitForFunction(() => document.querySelector('.dce-panel')?.dataset.phase === '3')
  await live.locator('.core-features').screenshot({ path: artifacts + 'features-dce.png' })
  await live.setViewportSize({ width: 390, height: 844 })
  assert(await live.evaluate(() => document.documentElement.scrollWidth <= innerWidth))
  await live.locator('#feature-panel').screenshot({ path: artifacts + 'mobile-dce.png' })
  await live.locator('#tab-dci').click()
  await live.locator('.dci-case-list button').nth(2).click()
  await live.locator('.provider-toggle').waitFor()
  for (const width of [390, 320]) {
    await live.setViewportSize({ width, height: 844 })
    assert(await live.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'DCI provider controls fit on mobile')
    assert(await live.locator('.feature-editor').evaluate(element => { const rect = element.getBoundingClientRect(); return rect.left >= 0 && rect.right <= innerWidth }), 'DCI panel fits inside the viewport without clipping')
    assert(await live.locator('.provider-toggle').evaluate(element => { const rect = element.getBoundingClientRect(); return rect.left >= 0 && rect.right <= innerWidth }), 'Both producer controls remain reachable')
  }
  await live.setViewportSize({ width: 390, height: 844 })
  await live.locator('#feature-panel').screenshot({ path: artifacts + 'mobile-dci.png' })
  await live.close()

  // Both HTML entries are independently addressable and share browser preferences.
  await page.setViewportSize({ width: 1440, height: 1000 })
  await page.goto(url + '/mosp.html')
  await page.locator('h1').waitFor()
  await page.evaluate(() => document.fonts.ready)
  await assertMospPage(page, 'The standalone MOSP page opens directly')
  assert.equal(await page.locator('html').getAttribute('lang'), 'zh-CN')
  assert.equal(await page.locator('html').getAttribute('data-theme'), 'light')
  assert(await page.locator('.brand-mark').first().evaluate(img => img.complete && img.naturalWidth > 0), 'The standalone page loads its logo')
  await assertMospFeature(page, 'dci', 'DCI is the default feature')
  for (const feature of mospFeatures) assert.equal(await page.locator(`.desktop-links a[href="#${feature}"]`).count(), 1, `${feature}: the header links to the feature`)

  // Feature selections remain usable with the keyboard and browser history.
  await selectMospFeature(page, 'migrate')
  await page.locator('#mosp-tab-migrate').focus()
  for (const [key, feature] of [['ArrowRight', 'reflection'], ['ArrowRight', 'dci'], ['ArrowRight', 'dce'], ['ArrowRight', 'migrate'], ['ArrowLeft', 'dce'], ['Home', 'migrate'], ['End', 'dce']]) {
    await page.keyboard.press(key)
    await assertMospFeature(page, feature, `${key} selects ${feature}`)
    assert.equal(new URL(page.url()).hash, '#' + feature)
    assert(await page.locator(`#mosp-tab-${feature}`).evaluate(element => document.activeElement === element), `${key} moves focus with selection`)
  }
  await selectMospFeature(page, 'migrate')
  await selectMospFeature(page, 'reflection')
  await selectMospFeature(page, 'dci')
  await page.goBack()
  await assertMospFeature(page, 'reflection', 'Back restores the previous feature')
  assert.equal(new URL(page.url()).hash, '#reflection')
  await page.goBack()
  await assertMospFeature(page, 'migrate', 'A second Back restores Migrate')
  await page.goForward()
  await assertMospFeature(page, 'reflection', 'Forward restores Reflection')
  await page.reload()
  await assertMospFeature(page, 'reflection', 'Refresh preserves the selected feature')
  await page.locator('.skip-link').focus()
  await page.keyboard.press('Enter')
  await page.waitForFunction(() => location.hash === '#main')
  await assertMospFeature(page, 'reflection', 'Skipping to content preserves the selected feature')
  for (const feature of mospFeatures) {
    await page.goto('about:blank')
    await page.goto(url + '/mosp.html#' + feature)
    await assertMospFeature(page, feature, `A direct ${feature} address restores its panel`)
    assert.equal(new URL(page.url()).hash, '#' + feature)
  }
  for (const locale of ['zh-CN', 'en']) {
    if (await page.locator('html').getAttribute('lang') !== locale) await page.locator('.locale-button').click()
    for (const feature of ['migrate', 'reflection']) {
      await selectMospFeature(page, feature)
      await checkMospExamples(page, feature)
    }
    await selectMospFeature(page, 'dce')
    assert.equal(await page.locator('#mosp-panel-dce .mosp-copy').count(), 0, 'The short DCE overview has no copy-source action')
    assert((await page.locator('#mosp-panel-dce').innerText()).trim().length > 30, 'DCE has a readable overview')
  }
  await page.locator('.locale-button').click()
  await selectMospFeature(page, 'dci')

  const sections = ['dci', 'pipeline', 'vector', 'ecosystem', 'failure', 'scope']
  for (const id of sections) {
    await page.goto('about:blank')
    await page.goto(url + '/mosp.html#' + id)
    await assertMospFeature(page, 'dci', `${id}: the existing deep link selects DCI`)
    assert.equal(await page.locator(`#${id}`).count(), 1, `The dedicated page covers ${id}`)
    assert((await page.locator(`#${id}`).innerText()).trim().length > 30, `${id} has reading content`)
    assert.equal(new URL(page.url()).hash, '#' + id)
    await page.waitForFunction(id => {
      const rect = document.getElementById(id)?.getBoundingClientRect()
      return rect && rect.top >= -16 && rect.top < innerHeight * 0.75
    }, id)
  }
  await page.reload()
  await assertMospFeature(page, 'dci', 'A DCI section refresh restores its panel')
  assert.equal(new URL(page.url()).hash, '#scope')
  await page.waitForFunction(() => {
    const rect = document.getElementById('scope')?.getBoundingClientRect()
    return rect && rect.top >= -16 && rect.top < innerHeight * 0.75
  })
  await page.locator('a[href="#dci"]').first().click()
  await assertMospFeature(page, 'dci')
  assert.equal(new URL(page.url()).hash, '#dci', 'DCI navigation reaches the section')
  const vectorCode = page.locator('#vector pre code').first()
  const vectorSource = (await vectorCode.innerText()).replaceAll('\r\n', '\n')
  assert.match(vectorSource, /std\.vector<i32>\s*\{\s*1\s*,\s*2\s*,\s*3\s*,\s*4\s*,\s*5\s*\}/, 'The example consumes an original generic vector')
  assert.match(vectorSource, /push_back\(&value\)/, 'The example includes a native container operation')
  assert.match(await page.locator('#vector').innerText(), /std\.vector<T>/, 'The section explains the open generic type')
  const vectorCopy = page.locator('.mosp-copy[data-copy="vector"]')
  assert.equal(await vectorCopy.count(), 1)
  const copyLabel = await vectorCopy.innerText()
  await vectorCopy.click()
  assert.equal((await page.evaluate(() => navigator.clipboard.readText())).replaceAll('\r\n', '\n').trim(), vectorSource.trim(), 'The vector example copies its complete source')
  await page.waitForFunction(label => document.querySelector('.mosp-copy[data-copy="vector"]')?.innerText !== label, copyLabel)
  assert.match(await vectorCopy.innerText(), /Copied|已复制/, 'Copy gives visible feedback')
  await page.locator('.mosp-directory a[href="#failure"]').click()
  await page.waitForFunction(() => document.querySelector('.mosp-directory a[href="#failure"]')?.getAttribute('aria-current') === 'location')
  assert.equal(new URL(page.url()).hash, '#failure', 'The reading directory navigates to a shareable section')
  await page.waitForFunction(() => {
    const rect = document.getElementById('failure').getBoundingClientRect()
    return rect.top >= 76 && rect.top < innerHeight / 2
  })
  const ecosystemLinks = await page.locator('#ecosystem a[href]').evaluateAll(links => links.map(link => ({ href: link.href, label: link.innerText.trim() })))
  assert(ecosystemLinks.length >= 2, 'The real ecosystem examples link to their projects or fixtures')
  assert(ecosystemLinks.every(link => /^https:\/\//.test(link.href) && link.label), 'Ecosystem references have concrete destinations and labels')
  assert(await page.locator('a[target="_blank"]').evaluateAll(links => links.every(link => link.rel.includes('noopener') && /^https:\/\//.test(link.href))), 'External references use safe new tabs')

  for (const locale of ['zh-CN', 'en']) {
    if (await page.locator('html').getAttribute('lang') !== locale) await page.locator('.locale-button').click()
    for (const theme of ['light', 'dark']) {
      if (await page.locator('html').getAttribute('data-theme') !== theme) await page.locator('.theme-button').click()
      for (const feature of mospFeatures) {
        await selectMospFeature(page, feature)
        for (const width of widths) await assertWidth(page, width, `MOSP ${feature} ${locale} ${theme}`)
      }
    }
  }
  await selectMospFeature(page, 'dci')
  await page.reload()
  await page.locator('h1').waitFor()
  await assertMospFeature(page, 'dci', 'The selected tab persists together with preferences')
  assert.equal(await page.locator('html').getAttribute('lang'), 'en', 'Language persists on direct page refresh')
  assert.equal(await page.locator('html').getAttribute('data-theme'), 'dark', 'Theme persists on direct page refresh')
  assert.equal(await page.evaluate(() => localStorage.getItem('vyx-v2-language')), 'en')
  assert.equal(await page.evaluate(() => localStorage.getItem('vyx-v2-theme')), 'dark')
  await assertWidth(page, 1440, 'MOSP dark screenshot')
  await page.evaluate(() => scrollTo(0, 0))
  await page.screenshot({ path: artifacts + 'mosp-dark.png', fullPage: true, animations: 'disabled' })
  await page.locator('.theme-button').click()
  await page.locator('.locale-button').click()
  await page.screenshot({ path: artifacts + 'mosp-desktop.png', fullPage: true, animations: 'disabled' })
  await assertWidth(page, 390, 'MOSP mobile screenshot')
  await page.evaluate(() => scrollTo(0, 0))
  await page.screenshot({ path: artifacts + 'mosp-mobile.png', fullPage: true, animations: 'disabled' })
  for (const feature of ['migrate', 'reflection', 'dce']) {
    await selectMospFeature(page, feature)
    for (const [width, suffix] of [[1440, ''], [390, '-mobile']]) {
      await assertWidth(page, width, `${feature} screenshot`)
      await page.evaluate(id => {
        const top = document.getElementById(`mosp-panel-${id}`).getBoundingClientRect().top + scrollY
        scrollTo({ top: top - 90, behavior: 'instant' })
      }, feature)
      await page.screenshot({ path: artifacts + `mosp-${feature}${suffix}.png`, animations: 'disabled' })
    }
  }
  await selectMospFeature(page, 'dci')
  await page.locator('.mobile-menu-button').click()
  assert.equal(await page.locator('.mobile-menu-button').getAttribute('aria-expanded'), 'true')
  for (const feature of mospFeatures) assert.equal(await page.locator(`#mobile-menu a[href="#${feature}"]`).count(), 1, `${feature}: the mobile menu links to the feature`)
  await page.locator('#mobile-menu a[href="#reflection"]').click()
  await assertMospFeature(page, 'reflection', 'Mobile navigation selects Reflection')
  assert.equal(await page.locator('#mobile-menu').count(), 0, 'Selecting a feature closes the mobile menu')
  await page.locator('.mobile-menu-button').click()
  await page.locator('#mobile-menu a[href="#dci"]').click()
  await assertMospFeature(page, 'dci', 'Mobile navigation returns to DCI')
  assert.equal(await page.locator('#mobile-menu').count(), 0, 'Selecting DCI closes the mobile menu')
  assert.equal(new URL(page.url()).hash, '#dci')
  await page.locator('.mobile-menu-button').click()
  await page.keyboard.press('Escape')
  assert.equal(await page.locator('#mobile-menu').count(), 0, 'Escape closes the standalone menu')
  const homeLink = page.locator('a[href="./"]').first()
  await homeLink.click()
  await page.locator('.hero-definition').waitFor()
  await assertHomePage(page, 'The independent page links home')
  assert.equal(await page.locator('html').getAttribute('lang'), 'zh-CN', 'Home reads the dedicated page language preference')
  assert.equal(await page.locator('html').getAttribute('data-theme'), 'light', 'Home reads the dedicated page theme preference')

  // Tutorial anchors wait for the requested chapter; chapter changes reset to the top.
  await page.setViewportSize({ width: 1440, height: 1000 })
  await page.emulateMedia({ reducedMotion: 'no-preference' })
  await page.goto(url + '/tutorial/#intermediate')
  await page.locator('.rail-sections a').first().waitFor()
  const projectGuide = page.locator('.tutorial-link').filter({ hasText: '创建与配置项目' })
  assert((await projectGuide.getAttribute('href')).includes('/blob/HEAD/docs/PROJECTS_ZH.md'), 'Document references retain their docs directory')
  const moduleGuide = page.locator('.tutorial-link').filter({ hasText: '项目指南' })
  assert((await moduleGuide.getAttribute('href')).includes('PROJECTS_ZH.md#'), 'Document references retain their anchor separator')
  const lesson = page.locator('.rail-sections a').nth(2)
  const lessonHash = await lesson.getAttribute('href')
  const lessonId = lessonHash.split('/')[1]
  const assertLessonPosition = async () => {
    await page.waitForFunction(id => {
      const top = document.getElementById(id)?.getBoundingClientRect().top
      return top >= 80 && top < 170
    }, lessonId)
    assert.equal(new URL(page.url()).hash, lessonHash)
    assert(await page.locator(`#${lessonId} h2`).evaluate(element => document.activeElement === element), 'Section navigation focuses its heading')
  }
  await lesson.click()
  await assertLessonPosition()
  await page.evaluate(() => scrollTo({ top: document.documentElement.scrollHeight, behavior: 'instant' }))
  await lesson.click()
  await assertLessonPosition()
  await page.reload()
  await assertLessonPosition()
  await page.locator('.chapter-pager .next').click()
  const nextChapter = chapterOrder[chapterOrder.indexOf('intermediate') + 1]
  await page.waitForFunction(id => location.hash === '#' + id && scrollY === 0 && document.activeElement === document.querySelector('#chapter-head h2'), nextChapter)
  await page.goBack()
  await assertLessonPosition()
  await page.goForward()
  await page.waitForFunction(id => location.hash === '#' + id && scrollY === 0, nextChapter)
  await page.locator('.skip-link').focus()
  await page.keyboard.press('Enter')
  assert.equal(new URL(page.url()).hash, '#' + nextChapter, 'The skip link preserves the chapter')
  assert(await page.locator('#main').evaluate(element => document.activeElement === element), 'The skip link focuses the reading surface')
  for (const locale of ['zh-CN', 'en']) {
    if (await page.locator('html').getAttribute('lang') !== locale) await page.locator('.locale-button').click()
    for (const theme of ['light', 'dark']) {
      if (await page.locator('html').getAttribute('data-theme') !== theme) await page.locator('.theme-button').click()
      for (const width of [1440, 768, 390, 320]) await assertWidth(page, width, `Tutorial ${locale} ${theme}`)
    }
  }
  await page.emulateMedia({ reducedMotion: 'reduce' })
  await page.setViewportSize({ width: 1440, height: 1000 })
  await page.goto(url + '/tutorial/' + lessonHash)
  await assertLessonPosition()
  await page.locator('.rail-sections a').nth(3).press('Enter')
  await page.waitForFunction(() => document.activeElement?.matches('.tutorial-section h2'))
  await page.setViewportSize({ width: 390, height: 844 })
  await page.locator('.chapter-pager .next').click()
  await page.waitForFunction(id => location.hash === '#' + id && scrollY === 0, nextChapter)
  await page.locator('.rail-mobile select').first().selectOption('basics')
  await page.waitForFunction(() => location.hash === '#basics' && scrollY === 0)
  const classesId = 's10-lesson-8-classes-and-methods'
  await page.locator('.rail-mobile select').nth(1).selectOption(classesId)
  await page.waitForFunction(id => {
    const section = document.getElementById(id)
    return section?.getBoundingClientRect().top >= 150 && section?.getBoundingClientRect().top < 210
  }, classesId)
  assert.match(await page.locator(`#${classesId}`).innerText(), /@\[vis\(package\)\]/, 'Visibility annotations are part of the actual lesson')
  assert.match(await page.locator(`#${classesId}`).innerText(), /@\[vis\(world\)\]/, 'The public shorthand is explained')
  assert.equal(await page.locator(`#${classesId} .prose-table tbody tr`).count(), 9, 'The full visibility scope table is present')
  await page.locator(`#${classesId} .prose-heading`).filter({ hasText: 'Visibility: @[vis]' }).scrollIntoViewIfNeeded()
  await page.screenshot({ path: artifacts + 'tutorial-mobile.png', animations: 'disabled' })
  await page.setViewportSize({ width: 1440, height: 1000 })
  await page.screenshot({ path: artifacts + 'tutorial-visibility.png', animations: 'disabled' })
  await page.goto(url + '/tutorial/')
  await page.locator('.rail-sections a').first().waitFor()
  await page.screenshot({ path: artifacts + 'tutorial-desktop.png', animations: 'disabled' })

  // Every published chapter renders from its paired documents, and the retired
  // standard-library chapter is gone from the rail, the stage list and the masthead.
  const publishedChapters = [
    { id: 'basics', first: { zh: '安装 SDK', en: 'Install the SDK' }, stats: [14, 10, 25], intro: true, marker: '@[vis(package)]' },
    { id: 'intermediate', first: { zh: '第11课：集合容器', en: 'Lesson 11: collection containers' }, stats: [11, 10, 25], intro: true, marker: 'fail ParseError.InvalidNumber' },
    { id: 'advanced', first: { zh: '关于这份教程', en: 'About this tutorial' }, stats: [27, 23, 33], intro: false, marker: 'Ref::<i32>.new(42)' },
    { id: 'migration', first: { zh: '基础语法', en: 'Basic syntax' }, stats: [9, 0, 32], intro: true, marker: 'fail MyErr.NotFound' },
  ]
  assert.match(await page.locator('.tutorial-masthead').innerText(), new RegExp(`${publishedChapters.length} CHAPTERS`), 'The masthead announces exactly the published chapters')
  assert.equal(await page.locator('.rail-chapters button').count(), publishedChapters.length, 'The rail lists every published chapter')
  assert.equal(await page.locator('.tutorial-stages li').count(), publishedChapters.length, 'The stage list matches the published chapters')
  assert.doesNotMatch(await page.locator('.rail-chapters').innerText(), /标准库|Standard library/, 'The retired standard-library chapter is gone from the rail')
  assert.doesNotMatch(await page.locator('.tutorial-stages').innerText(), /标准库|Standard library/, 'The retired standard-library chapter is gone from the stage list')
  await page.locator('.rail-chapters button').last().click()
  await page.waitForFunction(() => location.hash === '#migration' && scrollY === 0)
  assert.equal(await page.locator('.rail-chapters button').last().getAttribute('aria-current'), 'true', 'The rail marks the selected chapter')
  for (const locale of ['zh-CN', 'en']) {
    if (await page.locator('html').getAttribute('lang') !== locale) await page.locator('.locale-button').click()
    for (const chapter of publishedChapters) {
      await page.goto(url + '/tutorial/#' + chapter.id)
      // 换章节只是 hash 变化 ⇒ 同文档跳转，上一章的 DOM 会一直留到懒加载完成。
      // 必须先等到首节标题真的换成目标文档的标题，再去数小节，否则会读到上一章。
      const first = chapter.first[locale === 'zh-CN' ? 'zh' : 'en']
      await page.waitForFunction(title => document.querySelector('.tutorial-section h2')?.innerText.trim() === title, first)
      const label = `${chapter.id} (${locale})`
      assert.equal(new URL(page.url()).hash, '#' + chapter.id, `${label}: the chapter is addressable`)
      assert.equal((await page.locator('.tutorial-section h2').first().innerText()).trim(), first, `${label}: the first section keeps its document title`)
      assert.equal(await page.locator('.tutorial-section').count(), chapter.stats[0], `${label}: every section of the chapter renders`)
      assert.equal(await page.locator('.rail-sections a').count(), chapter.stats[0], `${label}: the rail lists every section`)
      assert.equal(await page.locator('.tutorial-section.is-lesson').count(), chapter.stats[1], `${label}: lessons keep their numbering`)
      assert.deepEqual((await page.locator('.chapter-stats dd').allInnerTexts()).map(Number), chapter.stats, `${label}: the chapter stats match the document`)
      assert.equal(await page.locator('.chapter-intro').count(), chapter.intro ? 1 : 0, `${label}: the chapter introduction renders where the document has one`)
      assert((await page.locator('.tutorial-body').innerText()).includes(chapter.marker), `${label}: the chapter carries its ${chapter.marker} explanation`)
      const empty = await page.locator('.tutorial-section').evaluateAll(nodes => nodes.filter(node => node.querySelector('.lesson-body')?.innerText.trim().length < 40).length)
      assert.equal(empty, 0, `${label}: no section renders without body content`)
      assert.equal(await page.locator('.tutorial-section .lesson-anchor').count(), chapter.stats[0], `${label}: every section exposes a shareable anchor`)
    }
  }
  await page.setViewportSize({ width: 1440, height: 1000 })
  await page.goto(url + '/tutorial/#migration')
  await page.waitForFunction(() => document.querySelector('.rail-sections a:last-child')?.innerText.includes('一句话总结') || document.querySelector('.rail-sections a:last-child')?.innerText.includes('In one line'))
  assert.equal(await page.locator('.tutorial-section').count(), 9, 'The migration chapter renders every section including its closing summary')
  assert.match(await page.locator('.rail-sections a').last().innerText(), /一句话总结|In one line/, 'The migration chapter keeps its closing summary section')
  await page.screenshot({ path: artifacts + 'tutorial-migration.png', animations: 'disabled' })
  await page.setViewportSize({ width: 390, height: 844 })
  await page.locator('.rail-mobile select').first().waitFor()
  await page.locator('.rail-mobile select').first().selectOption('migration')
  await page.locator('.rail-mobile select').nth(1).selectOption('wrap-up')
  await page.waitForFunction(() => location.hash === '#migration/wrap-up')
  assert.match(await page.locator('#wrap-up h2').innerText(), /^(一句话总结|In one line)$/, 'The mobile contents reaches the closing summary')
  assert(await page.locator('#wrap-up .lesson-body').innerText().then(text => text.trim().length > 200), 'The closing summary carries the migration takeaways')
  await page.screenshot({ path: artifacts + 'tutorial-migration-mobile.png', animations: 'disabled' })
  await page.setViewportSize({ width: 1440, height: 1000 })

  // Serve the built files under a subdirectory without a dev-server fallback.
  // This catches absolute asset URLs and entry links that only work at /.
  const distRoot = fileURLToPath(new URL('../dist/', import.meta.url))
  const mimeTypes = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml', '.woff': 'font/woff', '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.ico': 'image/x-icon' }
  nestedServer = createServer((request, response) => {
    try {
      const pathname = new URL(request.url, 'http://localhost').pathname
      if (!pathname.startsWith('/nested/')) { response.writeHead(404).end(); return }
      const requested = decodeURIComponent(pathname.slice('/nested/'.length))
      const relative = !requested || requested.endsWith('/') ? requested + 'index.html' : requested
      const file = resolve(distRoot, relative)
      if (!file.startsWith(resolve(distRoot) + sep) || !existsSync(file) || !statSync(file).isFile()) { response.writeHead(404).end(); return }
      response.writeHead(200, { 'Content-Type': mimeTypes[extname(file)] ?? 'application/octet-stream' }).end(readFileSync(file))
    } catch { response.writeHead(400).end() }
  })
  await new Promise(resolve => nestedServer.listen(0, '127.0.0.1', resolve))
  const nestedUrl = `http://127.0.0.1:${nestedServer.address().port}/nested/`
  const nested = await context.newPage()
  observeErrors(nested)
  await nested.goto(nestedUrl + 'mosp.html#reflection')
  await nested.locator('h1').waitFor()
  await assertMospPage(nested, 'Standalone entry loads beneath a deployment subdirectory')
  await assertMospFeature(nested, 'reflection', 'Nested feature deep link loads directly')
  assert(await nested.locator('.brand-mark').first().evaluate(img => img.complete && img.naturalWidth > 0), 'Nested entry logo loads')
  assert(await nested.evaluate(() => [...document.styleSheets].some(sheet => sheet.href?.includes('/nested/assets/'))), 'Nested entry stylesheet loads')
  await nested.reload()
  await nested.locator('h1').waitFor()
  await assertMospFeature(nested, 'reflection', 'Nested feature selection survives refresh')
  await nested.locator('a[href="./"]').first().click()
  await nested.locator('.hero-definition').waitFor()
  assert.equal(new URL(nested.url()).pathname, '/nested/', 'Home navigation retains the deployment prefix')
  await nested.locator('.desktop-links a[href$="mosp.html"]').click()
  await nested.locator('h1').waitFor()
  assert.equal(new URL(nested.url()).pathname, '/nested/mosp.html', 'MOSP navigation retains the deployment prefix')
  await assertMospFeature(nested, 'dci', 'Home opens the default DCI feature under the prefix')
  await nested.goto(nestedUrl + 'tutorial/' + lessonHash)
  await nested.locator(`#${lessonId}`).waitFor()
  assert(await nested.locator('.brand-mark').first().evaluate(img => img.complete && img.naturalWidth > 0), 'Nested tutorial loads its logo')
  assert((await nested.locator('.chapter-head .micro-label a').getAttribute('href')).startsWith('https://github.com/vyx-lang/vyx/'), 'Tutorial source links use the current repository')
  await nested.close()
  assert.deepEqual(errors, [], 'No browser errors or failed resources')
  console.log('UI checks passed: homepage and four MOSP tabs, all four tutorial chapters in both languages, tutorial section/repeated-anchor navigation, chapter reset, history and keyboard focus, 8 viewport widths, EN/ZH, dark/light, shared preferences, code copy, repository/document links, nested static deployment, DCE timeline, flow pause, dialogs, mobile navigation, and browser errors.')
  console.log('Screenshots saved in website/artifacts/.')
} finally {
  await browser?.close()
  nestedServer?.closeAllConnections()
  nestedServer?.close()
  server.kill()
}
