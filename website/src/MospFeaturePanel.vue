<script setup>
import { computed } from 'vue'
import { ArrowUpRight, Check, Copy } from 'lucide-vue-next'
import { repository } from './content'
import SourceCode from './SourceCode.vue'

const props = defineProps({ feature: String, content: Object, locale: String, copied: String })
const emit = defineEmits(['copy'])
const isZh = computed(() => props.locale === 'zh')
const sourceUrl = path => `${repository}/blob/HEAD/${path}`
const copyId = index => `${props.feature}-${index}`
</script>

<template>
  <div class="mosp-feature-content" :class="`mosp-${feature}-content`">
    <section :id="feature" class="mosp-section mosp-opening">
      <div class="mosp-section-heading"><p class="micro-label">{{ feature.toUpperCase() }}</p><h2>{{ content.title }}</h2><p>{{ content.intro }}</p></div>
      <dl class="mosp-feature-points"><div v-for="(point, index) in content.points" :key="point.name"><dt><span class="mosp-step-number" aria-hidden="true">0{{ index + 1 }}</span>{{ point.name }}</dt><dd>{{ point.detail }}</dd></div></dl>
    </section>

    <section v-if="content.examples?.length" class="mosp-section mosp-examples-section">
      <div class="mosp-section-heading"><h2>{{ isZh ? '代码示例' : 'Code examples' }}</h2></div>
      <div class="mosp-examples">
        <article v-for="(example, index) in content.examples" :key="example.filename" class="mosp-example-row">
          <div class="mosp-example-caption"><span class="mosp-step-number">0{{ index + 1 }}</span><h3>{{ example.filename }}</h3><p>{{ example.description }}</p></div>
          <div class="mosp-code" :data-example="copyId(index)">
            <div class="mosp-code-head"><span><i class="source-file-dot" aria-hidden="true"></i>{{ example.filename }}</span><button class="mosp-copy" :data-copy="copyId(index)" :aria-label="isZh ? copied === copyId(index) ? '已复制代码' : `复制 ${example.filename}` : copied === copyId(index) ? 'Code copied' : `Copy ${example.filename}`" @click="emit('copy', example.code, copyId(index))"><Check v-if="copied === copyId(index)" :size="14" /><Copy v-else :size="14" /><span>{{ isZh ? copied === copyId(index) ? '已复制' : '复制代码' : copied === copyId(index) ? 'Copied' : 'Copy code' }}</span></button></div>
            <SourceCode :source="example.code" :label="example.filename" />
          </div>
        </article>
      </div>
    </section>

    <section class="mosp-section mosp-feature-scope"><div class="mosp-section-heading"><h2>{{ content.scopeTitle }}</h2><p>{{ content.scope }}</p></div></section>
    <section class="mosp-section mosp-reading"><div class="mosp-section-heading"><h2>{{ isZh ? '文档与完整示例' : 'Documentation and complete examples' }}</h2></div><div class="mosp-reading-links"><a v-for="link in content.links" :key="link.path" :href="sourceUrl(link.path)" target="_blank" rel="noopener noreferrer"><div><strong>{{ link.title }}</strong><p>{{ link.description }}</p></div><ArrowUpRight :size="17" /></a></div></section>
  </div>
</template>
