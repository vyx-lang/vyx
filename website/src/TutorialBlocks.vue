<script setup>
/*
 * 教程正文的块渲染：段落、节内小标题、代码、列表、表格、引用、分隔线。
 * 分节正文与章节导读共用这一份，保证两处的排版与复制行为完全一致。
 */
import { computed } from 'vue'
import { Check, Copy } from 'lucide-vue-next'
import SourceCode from './SourceCode.vue'
import TutorialText from './TutorialText.vue'

const props = defineProps({
  blocks: { type: Array, default: () => [] },
  locale: { type: String, default: 'zh' },
  copied: { type: String, default: '' },
  idPrefix: { type: String, default: 'block' },
})
const emit = defineEmits(['copy'])

const isZh = computed(() => props.locale === 'zh')
/* 代码块按语言着色的只给 Vyx 与迁移对照里的原语言；shell / toml / 纯文本照原样排。 */
const isHighlighted = lang => ['vyx', 'rust', 'cpp', 'c', 'h', 'toml'].includes(lang)
const codeId = index => `${props.idPrefix}-${index}`
</script>

<template>
  <template v-for="(block, index) in blocks" :key="index">
    <p v-if="block.type === 'paragraph'" class="prose"><TutorialText :tokens="block.tokens" /></p>

    <h3 v-else-if="block.type === 'heading' && block.level <= 3" class="prose-heading"><TutorialText :tokens="block.tokens" /></h3>
    <h4 v-else-if="block.type === 'heading'" class="prose-subheading"><TutorialText :tokens="block.tokens" /></h4>

    <div v-else-if="block.type === 'code'" class="tutorial-code">
      <div class="code-head">
        <span><i class="source-file-dot" aria-hidden="true"></i>{{ block.lang }}</span>
        <button class="code-copy" :aria-label="isZh ? '复制代码' : 'Copy the code'" @click="emit('copy', block.code, codeId(index))"><Check v-if="copied === codeId(index)" :size="14" /><Copy v-else :size="14" /><span>{{ copied === codeId(index) ? (isZh ? '已复制' : 'Copied') : (isZh ? '复制代码' : 'Copy code') }}</span></button>
      </div>
      <SourceCode v-if="isHighlighted(block.lang)" :source="block.code" :label="block.lang" />
      <div v-else class="source-plain" tabindex="0"><pre><code>{{ block.code }}</code></pre></div>
    </div>

    <ul v-else-if="block.type === 'list' && !block.ordered" class="prose-list">
      <li v-for="(item, itemIndex) in block.items" :key="itemIndex"><TutorialText :tokens="item" /></li>
    </ul>
    <ol v-else-if="block.type === 'list'" class="prose-list ordered">
      <li v-for="(item, itemIndex) in block.items" :key="itemIndex"><TutorialText :tokens="item" /></li>
    </ol>

    <div v-else-if="block.type === 'table'" class="table-scroll" tabindex="0">
      <table class="prose-table">
        <thead><tr><th v-for="(cell, cellIndex) in block.head" :key="cellIndex"><TutorialText :tokens="cell" /></th></tr></thead>
        <tbody><tr v-for="(row, rowIndex) in block.rows" :key="rowIndex"><td v-for="(cell, cellIndex) in row" :key="cellIndex"><TutorialText :tokens="cell" /></td></tr></tbody>
      </table>
    </div>

    <blockquote v-else-if="block.type === 'quote'" class="prose-quote">
      <p v-for="(inner, innerIndex) in block.blocks" :key="innerIndex"><TutorialText :tokens="inner.tokens ?? []" /></p>
    </blockquote>

    <hr v-else-if="block.type === 'rule'" class="prose-rule" />
  </template>
</template>
