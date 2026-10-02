import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { fileURLToPath } from 'node:url'
import { buildTutorialContent } from './scripts/build-tutorial.mjs'

/*
 * 教程正文来自 docs/ 下的双语文档，构建前抽成分节 JSON。做成插件是为了绕开 npm 生命周期：
 * `npm run build`、`vite build`、`deploy.py` 直接调 vite.js，走的都是这一条。
 * 生成物在 src/tutorial/content/（被 Git 忽略），改了文档不需要手动重跑。
 */
function tutorialContent() {
  const docsDir = fileURLToPath(new URL('../docs', import.meta.url))
  return {
    name: 'vyx-tutorial-content',
    buildStart() {
      const { problems } = buildTutorialContent({ log: () => {} })
      for (const problem of problems) this.warn(problem)
    },
    configureServer(server) {
      buildTutorialContent({ log: () => {} })
      server.watcher.add(docsDir)
      server.watcher.on('change', file => {
        if (!file.endsWith('.md')) return
        buildTutorialContent({ log: () => {} })
        server.ws.send({ type: 'full-reload' })
      })
    },
  }
}

export default defineConfig({
  plugins: [vue(), tutorialContent()],
  base: './',
  build: {
    rolldownOptions: {
      input: {
        main: fileURLToPath(new URL('./index.html', import.meta.url)),
        mosp: fileURLToPath(new URL('./mosp.html', import.meta.url)),
        tutorial: fileURLToPath(new URL('./tutorial/index.html', import.meta.url)),
      },
    },
  },
  server: { port: 5173, strictPort: true },
  preview: { port: 4173, strictPort: true },
})
