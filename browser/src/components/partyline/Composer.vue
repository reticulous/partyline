<!-- The line being typed into the room. Enter sends. A byte counter appears
     past 80 % of partyline's 300-byte text limit, and a line over it can't be
     sent. -->
<template>
  <div class="comp">
    <div v-if="error" class="err">{{ error }}</div>
    <div class="row">
      <input ref="inp" v-model="text" class="f" :placeholder="placeholder" :disabled="disabled"
             spellcheck="true" autocomplete="off" @keydown.enter.exact.prevent="doSend" />
      <span v-if="showCount" class="cnt" :class="{ over }">{{ bytes }}/{{ TEXT_LIMIT }}</span>
      <button class="send" :disabled="!canSend" @click="doSend">Send</button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { TEXT_LIMIT } from '../../modules/partyline'
import { utf8Bytes } from '../../lib/partylineText'

const props = defineProps<{
  placeholder: string
  disabled: boolean
  error: string
}>()
const emit = defineEmits<{ send: [text: string] }>()

const text = ref('')
const inp = ref<HTMLInputElement | null>(null)

const bytes = computed(() => utf8Bytes(text.value))
const showCount = computed(() => bytes.value > TEXT_LIMIT * 0.8)
const over = computed(() => bytes.value > TEXT_LIMIT)
const canSend = computed(() => !props.disabled && text.value.trim().length > 0 && !over.value)

function doSend() {
  if (!canSend.value) return
  emit('send', text.value)
  text.value = ''
}

function focus() { inp.value?.focus() }
function insert(s: string) {
  text.value = text.value && !text.value.endsWith(' ') ? `${text.value} ${s}` : text.value + s
  focus()
}
defineExpose({ focus, insert })
</script>

<style scoped>
.comp { border-top: 1px solid #333; padding: 8px 12px; }
.err { color: #e09b6a; font-size: calc(12px * var(--rfs, 1)); margin-bottom: 6px; }
.row { display: flex; gap: 8px; align-items: center; }
.f { flex: 1; min-width: 0; background: #1c1c1c; color: #e8e8e8; border: 1px solid #3a3a3a; border-radius: 6px;
     padding: 7px 10px; font-size: calc(13.5px * var(--rfs, 1)); outline: none; font-family: inherit; }
.f:focus { border-color: #2c6bed; }
.f:disabled { opacity: .5; }
.cnt { color: #e8a33a; font-size: calc(11px * var(--rfs, 1)); white-space: nowrap; font-variant-numeric: tabular-nums; }
.cnt.over { color: #e05555; font-weight: 700; }
.send { background: #2c6bed; color: #fff; border: none; border-radius: 6px; padding: 7px 14px; font-weight: 700;
        font-size: calc(13px * var(--rfs, 1)); cursor: pointer; }
.send:disabled { background: #3a3a3a; color: #777; cursor: default; }
</style>
