<script setup>
/*
 * 一节教程：节号 + 标题 + 正文分块（段落、小标题、代码、列表、表格、引用）。
 * 代码块复用 SourceCode 的高亮与行号，外壳与 MOSP 页保持同一套编辑器配色。
 */
import { computed } from 'vue'
import { Link2 } from 'lucide-vue-next'
import TutorialBlocks from './TutorialBlocks.vue'
import TutorialFigure from './TutorialFigure.vue'

const props = defineProps({
  section: { type: Object, required: true },
  locale: { type: String, default: 'zh' },
  copied: { type: String, default: '' },
  figure: { type: String, default: '' },
  phase: { type: Number, default: 0 },
})
const emit = defineEmits(['copy', 'anchor'])

const isZh = computed(() => props.locale === 'zh')
const label = computed(() => {
  if (props.section.n) return `${isZh.value ? '第' : 'LESSON '}${isZh.value ? props.section.n + ' 课' : props.section.n}`
  if (props.section.kind === 'prep') return isZh.value ? '准备' : 'SETUP'
  if (props.section.kind === 'note') return isZh.value ? '导读' : 'OVERVIEW'
  if (props.section.kind === 'next') return isZh.value ? '下一步' : 'NEXT'
  return isZh.value ? '小节' : 'SECTION'
})
</script>

<template>
  <section :id="section.id" class="tutorial-section" :class="[`kind-${section.kind}`, { 'is-lesson': section.n }]">
    <header class="lesson-head">
      <div class="lesson-marker"><span v-if="section.n" class="lesson-number">{{ String(section.n).padStart(2, '0') }}</span><span v-else class="lesson-kind">{{ label }}</span></div>
      <div class="lesson-title">
        <p class="micro-label">{{ label }}</p>
        <h2 tabindex="-1">{{ section.title }}</h2>
        <button class="lesson-anchor" :aria-label="isZh ? '复制本节链接' : 'Copy the link to this section'" @click="emit('anchor', section.id)"><Link2 :size="13" /><span>{{ copied === section.id ? (isZh ? '已复制链接' : 'Link copied') : (isZh ? '本节链接' : 'Section link') }}</span></button>
      </div>
    </header>

    <div class="lesson-body">
      <TutorialBlocks :blocks="section.blocks" :locale="locale" :copied="copied" :id-prefix="section.id" @copy="(text, id) => emit('copy', text, id)" />
    </div>

    <TutorialFigure v-if="figure" class="figure-inline" :name="figure" :locale="locale" :phase="phase" />
  </section>
</template>
