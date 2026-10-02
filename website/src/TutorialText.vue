<script setup>
/* 文档解析出来的行内片段：反引号、强调、站内外链接。不用 v-html。 */
import { ArrowUpRight } from 'lucide-vue-next'

defineProps({ tokens: { type: Array, default: () => [] } })
</script>

<template>
  <template v-for="(token, index) in tokens" :key="index">
    <code v-if="token.t === 'code'">{{ token.v }}</code>
    <strong v-else-if="token.t === 'strong'">{{ token.v }}</strong>
    <em v-else-if="token.t === 'em'">{{ token.v }}</em>
    <a v-else-if="token.t === 'link'" class="tutorial-link" :href="token.href" :target="token.out ? '_blank' : undefined" :rel="token.out ? 'noopener noreferrer' : undefined">{{ token.v }}<ArrowUpRight v-if="token.out" :size="11" /></a>
    <template v-else>{{ token.v }}</template>
  </template>
</template>
