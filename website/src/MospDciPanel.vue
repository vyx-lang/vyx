<script setup>
import { computed } from 'vue'
import { ArrowUpRight, Check, Copy } from 'lucide-vue-next'
import { repository } from './content'
import SourceCode from './SourceCode.vue'

const props = defineProps({ content: Object, locale: String, copied: String })
const emit = defineEmits(['copy'])
const t = computed(() => props.content)
const isZh = computed(() => props.locale === 'zh')
const copied = computed(() => props.copied === 'vector')
const sourceUrl = path => `${repository}/blob/HEAD/${path}`
const copyVector = () => emit('copy', t.value.vectorCode, 'vector')
</script>

<template>
  <div class="mosp-dci-content">
      <section id="dci" class="mosp-section mosp-opening">
        <div class="mosp-section-heading"><p class="micro-label">02 / DCI</p><h2>{{ t.dciTitle }}</h2><p>{{ t.dciIntro }}</p></div>
        <div id="pipeline" class="mosp-pipeline-block"><div class="mosp-subheading"><h3>{{ t.pipelineTitle }}</h3><p>{{ t.pipelineIntro }}</p></div><ol class="mosp-pipeline"><li v-for="(step, index) in t.pipeline" :key="step.id"><span class="mosp-step-number">0{{ index + 1 }}</span><h4>{{ step.name }}</h4><p>{{ step.detail }}</p></li></ol></div>
      </section>

      <section id="vector" class="mosp-section mosp-vector-section">
        <div class="mosp-vector-intro"><p class="micro-label">03 / std::vector&lt;T&gt;</p><h2>{{ t.vectorTitle }}</h2><p>{{ t.vectorIntro }}</p><p class="mosp-vector-note">{{ t.vectorNote }}</p><a class="text-link" :href="sourceUrl('probes/gates/dci-vector/README.md')" target="_blank" rel="noopener noreferrer">{{ isZh ? '声明与构建步骤' : 'Declarations and build steps' }}<ArrowUpRight :size="15" /></a></div>
        <div class="mosp-code"><div class="mosp-code-head"><span><i class="source-file-dot" aria-hidden="true"></i>main.vyx</span><button class="mosp-copy" data-copy="vector" :aria-label="isZh ? copied ? '已复制代码' : '复制 vector 代码' : copied ? 'Code copied' : 'Copy vector code'" @click="copyVector"><Check v-if="copied" :size="14" /><Copy v-else :size="14" /><span>{{ isZh ? copied ? '已复制' : '复制代码' : copied ? 'Copied' : 'Copy code' }}</span></button></div><SourceCode :source="t.vectorCode" :label="isZh ? 'Vyx vector 示例' : 'Vyx vector example'" /><div class="source-footer"><span>VYX / C++</span><span>{{ isZh ? '原始模板 · 两种实例' : 'ORIGINAL TEMPLATE / TWO INSTANCES' }}</span></div></div>
      </section>

      <section id="facts" class="mosp-section mosp-facts-section"><div class="mosp-section-heading"><p class="micro-label">04 / ABI</p><h2>{{ t.factsTitle }}</h2></div><dl class="mosp-facts"><div v-for="fact in t.facts" :key="fact.name"><dt>{{ fact.name }}</dt><dd>{{ fact.detail }}</dd></div></dl></section>

      <section id="ecosystem" class="mosp-section"><div class="mosp-section-heading"><p class="micro-label">05 / C++ &amp; Rust</p><h2>{{ t.ecosystemTitle }}</h2><p>{{ t.ecosystemIntro }}</p></div><div class="mosp-ecosystem"><article v-for="(example, index) in t.ecosystem" :key="example.id"><div class="mosp-example-meta"><p class="mosp-example-language">{{ example.language }}</p><span>0{{ index + 1 }}</span></div><h3>{{ example.name }}</h3><p>{{ example.description }}</p><p class="mosp-example-detail">{{ example.detail }}</p><a class="text-link" :href="sourceUrl(example.path)" target="_blank" rel="noopener noreferrer">{{ example.id === 'multilang' ? isZh ? '接口说明' : 'Interface reference' : isZh ? '查看项目' : 'View project' }}<ArrowUpRight :size="15" /></a></article></div></section>

      <section id="failure" class="mosp-section mosp-failure"><div class="mosp-section-heading"><p class="micro-label">06 / Lifetime</p><h2>{{ t.failureTitle }}</h2><p>{{ t.failureIntro }}</p></div><ul><li v-for="point in t.failurePoints" :key="point"><Check :size="16" /><p>{{ point }}</p></li></ul></section>

      <section id="scope" class="mosp-section"><div class="mosp-section-heading"><p class="micro-label">07 / AOT</p><h2>{{ t.scopeTitle }}</h2><p>{{ t.scopeIntro }}</p></div><dl class="mosp-scope"><div v-for="row in t.scope" :key="row.name"><dt>{{ row.name }}</dt><dd>{{ row.value }}</dd></div></dl></section>

      <section id="reading" class="mosp-section mosp-reading"><div class="mosp-section-heading"><h2>{{ t.readingTitle }}</h2></div><div class="mosp-reading-links"><a v-for="link in t.links" :key="link.path" :href="sourceUrl(link.path)" target="_blank" rel="noopener noreferrer"><div><strong>{{ link.title }}</strong><p>{{ link.description }}</p></div><ArrowUpRight :size="17" /></a></div></section>
  </div>
</template>
