<script setup>
import { computed } from 'vue'
import { highlightSource } from './sourceHighlight'

const props = defineProps({ source: String, label: String })
const highlighted = computed(() => highlightSource(props.source))
const lines = computed(() => props.source.split('\n').length)
</script>

<template>
  <div class="source-scroll" tabindex="0" :aria-label="label">
    <div class="source-gutter" aria-hidden="true"><span v-for="line in lines" :key="line" :data-line="line"></span></div>
    <pre><code v-html="highlighted"></code></pre>
  </div>
</template>
