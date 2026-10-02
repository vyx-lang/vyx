/*
 * docs/ 下的中英文教程文档 → 教程站用的结构化 JSON。
 *
 * 教程站不重写内容：仓库文档是唯一真相，这里只做「分节 + 分块 + 链接重写」，
 * 生成物写到 src/tutorial/content/ 并被 Git 忽略。vite 构建与 dev 都会自动
 * 跑一次（见 vite.config.js 里的插件），所以改了 docs/ 里的文档就自动生效。
 *
 * 用法：
 *   node scripts/build-tutorial.mjs            # 生成
 *   node scripts/build-tutorial.mjs --check    # 只校验（中英小节数量是否对得上）
 */
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join, relative } from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const WEBSITE = dirname(HERE)
const REPO = dirname(WEBSITE)
const DOCS = join(REPO, 'docs')
const OUT_DIR = join(WEBSITE, 'src', 'tutorial', 'content')

const REPO_URL = 'https://github.com/vyx-lang/vyx'
const BLOB = `${REPO_URL}/blob/HEAD/`

/* ------------------------------------------------------------------ 配置 -- */
const { chapters, chapterOrder } = await import('../src/tutorial-chapters.js')

/* 文档 → 站点页面。教程章节之外的文档继续指到仓库，MOSP 指到已有的独立页。 */
const PAGE_FOR_DOC = { 'MOSP_ZH.md': '../mosp.html', 'MOSP.md': '../mosp.html' }
const ZH_DOCS = new Set(Object.values(chapters).map(c => c.zh).concat(['README.md', '设计文档_ZH.md']))

/* ------------------------------------------------------------ markdown -- */
const isBlockStart = line => /^(?:#{1,6}\s|```|\||>\s?|\s*[-*]\s+|\s*\d+\.\s+|-{3,}\s*$)/.test(line)
const CJK = /[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\uff00-\uffef]/

/* 中文文档按标点处折行，直接补空格会在句子中间留下空隙；英文折行相反。 */
function joinWrapped(lines) {
  let out = ''
  for (const line of lines) {
    const text = line.trim()
    if (!out) { out = text; continue }
    const touching = CJK.test(out.slice(-1)) || CJK.test(text.slice(0, 1))
    out += (touching ? '' : ' ') + text
  }
  return out
}

function parseTable(rows, ctx) {
  const cells = row => {
    const result = []
    let cell = '', code = false
    for (let index = 0; index < row.length; index++) {
      const character = row[index]
      if (character === '\\' && row[index + 1] === '|') { cell += '|'; index++; continue }
      if (character === '`') code = !code
      if (character === '|' && !code) { result.push(cell.trim()); cell = '' }
      else cell += character
    }
    result.push(cell.trim())
    if (!result[0]) result.shift()
    if (!result.at(-1)) result.pop()
    return result
  }
  const head = cells(rows[0])
  const body = rows.slice(rows[1] && /^[\s|:-]+$/.test(rows[1]) ? 2 : 1)
  return {
    type: 'table',
    head: head.map(cell => inline(cell, ctx)),
    rows: body.map(row => cells(row).map(cell => inline(cell, ctx))),
  }
}

function parseBlocks(lines, ctx) {
  const blocks = []
  let i = 0
  while (i < lines.length) {
    const line = lines[i]
    if (/^```/.test(line)) {
      const lang = line.slice(3).trim() || 'text'
      const body = []
      i += 1
      while (i < lines.length && !/^```/.test(lines[i])) { body.push(lines[i]); i += 1 }
      i += 1
      blocks.push({ type: 'code', lang, code: body.join('\n').replace(/\s+$/, '') })
      continue
    }
    if (!line.trim()) { i += 1; continue }
    const heading = /^(#{1,6})\s+(.*?)\s*$/.exec(line)
    if (heading) {
      blocks.push({ type: 'heading', level: heading[1].length, text: heading[2], tokens: inline(heading[2], ctx) })
      i += 1
      continue
    }
    if (/^-{3,}\s*$/.test(line)) { blocks.push({ type: 'rule' }); i += 1; continue }
    if (/^\s*\|/.test(line)) {
      const rows = []
      while (i < lines.length && /^\s*\|/.test(lines[i])) { rows.push(lines[i]); i += 1 }
      blocks.push(parseTable(rows, ctx))
      continue
    }
    if (/^>\s?/.test(line)) {
      const quoted = []
      while (i < lines.length && /^>\s?/.test(lines[i])) { quoted.push(lines[i].replace(/^>\s?/, '')); i += 1 }
      blocks.push({ type: 'quote', blocks: parseBlocks(quoted, ctx) })
      continue
    }
    const bullet = /^\s*[-*]\s+(.*)$/.exec(line)
    const numbered = /^\s*\d+\.\s+(.*)$/.exec(line)
    if (bullet || numbered) {
      const ordered = Boolean(numbered)
      const items = []
      while (i < lines.length) {
        const next = ordered ? /^\s*\d+\.\s+(.*)$/.exec(lines[i]) : /^\s*[-*]\s+(.*)$/.exec(lines[i])
        if (!next) break
        items.push(inline(next[1], ctx))
        i += 1
      }
      blocks.push({ type: 'list', ordered, items })
      continue
    }
    const paragraph = [line]
    i += 1
    while (i < lines.length && lines[i].trim() && !isBlockStart(lines[i]) && !/^```/.test(lines[i])) {
      paragraph.push(lines[i])
      i += 1
    }
    blocks.push({ type: 'paragraph', tokens: inline(joinWrapped(paragraph), ctx) })
  }
  return blocks
}

/* -------------------------------------------------------------- inline -- */
const INLINE = /(`[^`]+`)|(\*\*[^*]+\*\*)|(\[[^\]]+\]\([^)]+\))|(\*[^*\n]+\*)|(__[^_]+__)/g

function inline(text, ctx) {
  const tokens = []
  let end = 0
  for (const match of text.matchAll(INLINE)) {
    if (match.index > end) tokens.push({ t: 'text', v: text.slice(end, match.index) })
    const value = match[0]
    if (value.startsWith('`')) tokens.push({ t: 'code', v: value.slice(1, -1) })
    else if (value.startsWith('**') || value.startsWith('__')) tokens.push({ t: 'strong', v: value.slice(2, -2) })
    else if (value.startsWith('*')) tokens.push({ t: 'em', v: value.slice(1, -1) })
    else {
      const link = /^\[([^\]]+)\]\(([^)]+)\)$/.exec(value)
      tokens.push({ t: 'link', v: link[1], href: resolveLink(link[2], ctx), out: isRepoLink(link[2], ctx) })
    }
    end = match.index + value.length
  }
  if (end < text.length) tokens.push({ t: 'text', v: text.slice(end) })
  return tokens
}

/* 站外还是站内：教程章节与 MOSP 页留在站内，其余文档/源码一律回仓库。 */
function isRepoLink(href, ctx) {
  if (/^https?:/i.test(href)) return true
  const file = href.split('#')[0]
  if (!file) return false
  return !ctx.chapterDocs.has(file) && !PAGE_FOR_DOC[file]
}

const anchorSlug = text => text.toLowerCase().replace(/[^\w\u4e00-\u9fff-]+/g, '')

/* 文档之间的小标题可能只写「第 21 课」，标题对不上时退回到课号匹配。 */
function anchorTarget(doc, hash, ctx) {
  const map = ctx.headings.get(doc)
  if (!map) return null
  const exact = map[anchorSlug(hash)]
  if (exact) return exact
  const lesson = /^(?:第\s*(\d+)\s*课|Lesson\s*(\d+))/i.exec(hash.trim())
  return lesson ? (ctx.lessons.get(doc)?.get(Number(lesson[1] ?? lesson[2])) ?? null) : null
}

/* 文档之间的相对链接：教程章节转成站内路由，其余指到仓库对应文件。 */
function resolveLink(href, ctx) {
  if (/^https?:/i.test(href)) return href
  if (href.startsWith('#')) {
    const target = anchorTarget(ctx.doc, href.slice(1), ctx)
    return target ? `#${ctx.chapter}/${target}` : `#${ctx.chapter}`
  }
  const [path, hash = ''] = href.split('#')
  const chapterId = ctx.chapterFor(path)
  if (chapterId) {
    const target = hash ? anchorTarget(path, hash, ctx) : null
    return target ? `#${chapterId}/${target}` : `#${chapterId}`
  }
  if (PAGE_FOR_DOC[path]) {
    const feature = /^(migrate|reflection|dci|dce)(?:-|[^a-z]|$)/i.exec(hash)?.[1]?.toLowerCase()
    return PAGE_FOR_DOC[path] + (feature ? `#${feature}` : '')
  }
  const file = relative(REPO, join(DOCS, path)).replace(/\\/g, '/')
  return `${BLOB}${file}${hash ? '#' + hash : ''}`
}

/* ---------------------------------------------------------- 文档解析 -- */
function parseDoc(file) {
  const raw = readFileSync(join(DOCS, file), 'utf8').replace(/^\uFEFF/, '')
  const lines = raw.split(/\r?\n/)
  const title = /^#\s+(.*)$/.exec(lines[0])?.[1] ?? file
  const intro = []
  const groups = []
  let current = null
  let buffer = []
  let fenced = false
  for (let i = 1; i < lines.length; i += 1) {
    const line = lines[i]
    if (/^```/.test(line)) fenced = !fenced
    // 代码块里的一级/二级标题只是注释（如 powershell 片段里的 # Windows），不能当分节。
    if (!fenced && /^##\s+/.test(line)) {
      if (current) groups.push({ ...current, lines: buffer })
      else intro.push(...buffer)
      current = { title: line.replace(/^##\s+/, '').trim() }
      buffer = []
      continue
    }
    buffer.push(line)
  }
  if (current) groups.push({ ...current, lines: buffer })
  else intro.push(...buffer)
  return { file, title, introLines: intro, groups }
}

function sectionSlug(en, zh, index) {
  const base = (en || zh).toLowerCase().replace(/`/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '')
  return base ? `s${String(index + 1).padStart(2, '0')}-${base}` : `s${String(index + 1).padStart(2, '0')}`
}

function lessonNumber(...titles) {
  for (const title of titles) {
    const focus = /^Lesson\s+(\d+)/i.exec(title ?? '')
    if (focus) return Number(focus[1])
    const chinese = /^第\s*(\d+)\s*课/.exec(title ?? '')
    if (chinese) return Number(chinese[1])
  }
  return null
}

function kindOf(title) {
  if (/^(?:What is next|接下来)/i.test(title)) return 'next'
  if (/^(?:Install|Compile|安装|编译)/i.test(title)) return 'prep'
  if (/^(?:About this|关于这份|A safety map|先建立)/i.test(title)) return 'note'
  return 'section'
}

/* --------------------------------------------------------------- 主流程 --
 * 三遍走：先定分节与 id，再把「文档小标题 → 站内路由」建成一张表，最后才渲染正文。
 * 顺序不能换：跨章节链接的锚点要落到最终的小节 id 上，而 id 由英文标题决定。 */
export function buildTutorialContent({ check = false, log = console.log } = {}) {
  const docs = new Map()
  for (const chapter of Object.values(chapters)) {
    docs.set(chapter.zh, parseDoc(chapter.zh))
    docs.set(chapter.en, parseDoc(chapter.en))
  }
  const chapterDocs = new Set(Object.values(chapters).flatMap(c => [c.zh, c.en]))
  const chapterFor = path => chapterOrder.find(id => path === chapters[id].zh || path === chapters[id].en)
  const problems = []

  /* 一：分节。id 取英文标题，两种语言共用，切语言时锚点不跳。 */
  const plan = new Map()
  for (const id of chapterOrder) {
    const chapter = chapters[id]
    const zh = docs.get(chapter.zh)
    const en = docs.get(chapter.en)
    const pairs = chapter.manifest
      ? chapter.manifest.map(entry => ({
          id: entry.id,
          zh: entry.zh.map(title => docGroup(zh, title, problems, chapter, title)),
          en: entry.en.map(title => docGroup(en, title, problems, chapter, title)),
        }))
      : zh.groups.map((group, index) => ({
          id: null,
          zh: [group],
          en: en.groups[index] ? [en.groups[index]] : [],
        }))

    if (!chapter.manifest && zh.groups.length !== en.groups.length) {
      problems.push(`${id}: 中文 ${zh.groups.length} 节 / 英文 ${en.groups.length} 节，请加 manifest`)
    }

    plan.set(id, pairs.map((pair, index) => {
      const enGroups = pair.en.filter(Boolean)
      const zhGroups = pair.zh.filter(Boolean)
      const titleEn = plain(enGroups[0]?.title ?? zhGroups[0]?.title)
      const titleZh = plain(zhGroups[0]?.title ?? titleEn)
      return {
        id: pair.id ?? sectionSlug(titleEn, titleZh, index),
        n: lessonNumber(titleEn, titleZh),
        kind: kindOf(titleZh || titleEn),
        title: { zh: titleZh, en: titleEn },
        groups: { zh: zhGroups, en: enGroups },
      }
    }))
  }

  /* 二：锚点表。合并成一节的中文小标题也指向同一个 id。 */
  const headings = new Map()
  const lessons = new Map()
  for (const id of chapterOrder) {
    const chapter = chapters[id]
    for (const lang of ['zh', 'en']) {
      const map = {}
      const byLesson = new Map()
      for (const section of plan.get(id)) {
        for (const group of section.groups[lang]) map[anchorSlug(group.title)] = section.id
        if (section.n) byLesson.set(section.n, section.id)
      }
      headings.set(chapter[lang], map)
      lessons.set(chapter[lang], byLesson)
    }
  }

  /* 三：渲染正文。 */
  const output = []
  for (const id of chapterOrder) {
    const chapter = chapters[id]
    const sections = plan.get(id)
    for (const lang of ['zh', 'en']) {
      const doc = docs.get(chapter[lang])
      const ctx = { chapter: id, doc: doc.file, headings, lessons, chapterFor, chapterDocs }
      const blocks = sections.map(section => ({ section, blocks: renderGroups(section.groups[lang], ctx) }))
      const source = relative(REPO, join(DOCS, doc.file)).replace(/\\/g, '/')
      output.push({
        path: join(OUT_DIR, `${id}.${lang}.json`),
        payload: {
          id,
          lang,
          source,
          sourceUrl: `${BLOB}${source}`,
          title: doc.title,
          intro: stripLanguageLinks(parseBlocks(doc.introLines, ctx)),
          sections: blocks.map(({ section, blocks: body }) => ({
            id: section.id,
            n: section.n,
            kind: section.kind,
            title: section.title[lang],
            blocks: body,
          })),
          stats: {
            sections: sections.length,
            lessons: sections.filter(section => section.n).length,
            code: blocks.reduce((sum, entry) => sum + entry.blocks.filter(block => block.type === 'code').length, 0),
          },
        },
      })
    }
    log(`  ${id.padEnd(13)} ${String(sections.length).padStart(2)} 节 · ${docs.get(chapter.zh).title}`)
  }

  for (const problem of problems) console.warn(`  ! ${problem}`)

  if (!check) {
    mkdirSync(OUT_DIR, { recursive: true })
    for (const { path, payload } of output) writeFileSync(path, `${JSON.stringify(payload)}\n`, 'utf8')
  }
  return { problems, files: output.length }
}

/* 侧栏与标题栏要的是纯文本：去掉反引号、强调与链接包装。 */
const plain = text => (text ?? '')
  .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1')
  .replace(/\*\*([^*]+)\*\*/g, '$1')
  .replace(/`([^`]+)`/g, '$1')
  .replace(/\*([^*]+)\*/g, '$1')
  .trim()

function docGroup(doc, title, problems, chapter, wanted) {
  const group = doc.groups.find(item => item.title === title)
  if (!group) problems.push(`${chapter.zh}/${chapter.en}: 找不到小节「${wanted}」`)
  return group ?? null
}

/* 合并成一节时，被并进来的小节标题降为节内小标题，内容不丢。 */
function renderGroups(groups, ctx) {
  const blocks = []
  groups.forEach((group, index) => {
    if (index > 0) blocks.push({ type: 'heading', level: 3, text: group.title })
    blocks.push(...parseBlocks(group.lines, ctx))
  })
  return blocks
}

/* 文档开头那行「[English](TUTORIAL.md) · [文档目录](README.md)」在站内是多余的。 */
function stripLanguageLinks(blocks) {
  const separator = value => !/[\p{L}\p{N}]/u.test(value)
  const keep = block => !(block.type === 'paragraph' && block.tokens.length > 0
    && block.tokens.every(token => token.t === 'link' || (token.t === 'text' && separator(token.v))))
  return blocks.filter(keep)
}

if (process.argv[1] && fileURLToPath(import.meta.url) === join(process.argv[1])) {
  const check = process.argv.includes('--check')
  console.log(check ? '校验教程结构…' : '生成教程内容…')
  const result = buildTutorialContent({ check })
  console.log(check
    ? `校验完成：${result.problems.length} 个问题`
    : `已写出 ${result.files} 个文件到 website/src/tutorial/content/`)
  process.exit(result.problems.length && check ? 1 : 0)
}
