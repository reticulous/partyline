<!-- A member: name, identity hash in fours, the room they are in, how far
     away they are and what state they are in, and a poke — a private line
     only they see. -->
<template>
  <div class="scrim" @click.self="emit('close')">
    <div class="card">
      <div class="top">
        <div class="who">
          <div class="nm" :style="{ color: nickColour(memberHue(member.identity || member.mid)) }">{{ member.name || `member ${member.mid}` }}</div>
          <div class="hh">{{ member.identity ? groupHex(member.identity) : 'identity not given' }}</div>
        </div>
        <span class="x" title="Close" @click="emit('close')">✕</span>
      </div>
      <div class="meta">
        <div v-if="roomName">In {{ roomName }}</div>
        <div v-if="member.hops >= 0 || member.rtt > 0">
          <template v-if="member.hops >= 0">{{ member.hops }} hop{{ member.hops === 1 ? '' : 's' }}</template>
          <template v-if="member.hops >= 0 && member.rtt > 0"> · </template>
          <template v-if="member.rtt > 0">round trip {{ member.rtt }} ms</template>
        </div>
        <div v-if="states">{{ states }}</div>
        <div v-if="member.self" class="dim">This is you.</div>
      </div>
      <div v-if="!member.self" class="poke">
        <input ref="inp" v-model="text" class="in" placeholder="Poke: a line only they see" spellcheck="true"
               @keydown.enter.exact.prevent="send" />
        <button class="p" :disabled="!canSend" @click="send">Poke</button>
      </div>
      <div v-if="over" class="err">{{ bytes }}/{{ TEXT_LIMIT }} bytes: too long</div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { TEXT_LIMIT, type PlMember } from '../../modules/partyline'
import { groupHex, memberHue, nickColour, utf8Bytes } from '../../lib/partylineText'

const props = defineProps<{
  member: PlMember
  roomName: string
}>()
const emit = defineEmits<{
  close: []
  poke: [mid: string, text: string]
}>()

const states = computed(() => {
  const m = props.member
  const s: string[] = []
  if (m.op) s.push('operator')
  if (m.textOnly) s.push('text only')
  else {
    if (m.serverMuted) s.push('muted by an operator')
    else if (m.muted) s.push('muted')
    if (m.deaf) s.push('deafened')
    if (!m.canSpeak) s.push('may not speak here')
  }
  return s.join(' · ')
})

const text = ref('')
const inp = ref<HTMLInputElement | null>(null)
const bytes = computed(() => utf8Bytes(text.value))
const over = computed(() => bytes.value > TEXT_LIMIT)
const canSend = computed(() => text.value.trim().length > 0 && !over.value)

function send() {
  if (!canSend.value) return
  emit('poke', props.member.mid, text.value)
  text.value = ''
  emit('close')
}

onMounted(() => inp.value?.focus())
</script>

<style scoped>
.scrim { position: absolute; inset: 0; z-index: 8; display: flex; align-items: flex-start; justify-content: center;
         padding: 60px 12px 0; background: rgba(0, 0, 0, .3); }
.card { width: 320px; max-width: 100%; background: #2f2f2f; border: 1px solid #454545; border-radius: 10px;
        padding: 14px 16px; box-shadow: 0 24px 60px rgba(0, 0, 0, .6); color: #e8e8e8; font-size: calc(13px * var(--rfs, 1)); }
.top { display: flex; gap: 12px; align-items: center; }
.who { flex: 1; min-width: 0; }
.nm { font-weight: 700; font-size: calc(16px * var(--rfs, 1)); overflow: hidden; text-overflow: ellipsis; }
.hh { font-family: "DejaVu Sans Mono", Menlo, monospace; color: #9a9a9a; font-size: calc(12px * var(--rfs, 1));
      margin-top: 2px; word-break: break-all; }
.x { align-self: flex-start; color: #9a9a9a; cursor: pointer; padding: 0 2px; }
.x:hover { color: #fff; }
.meta { color: #9a9a9a; font-size: calc(12px * var(--rfs, 1)); margin: 10px 0; display: flex; flex-direction: column; gap: 3px; }
.dim { color: #777; }
.poke { display: flex; gap: 8px; }
.in { flex: 1; min-width: 0; background: #1c1c1c; color: #e8e8e8; border: 1px solid #3a3a3a; border-radius: 6px;
      padding: 6px 8px; font-size: calc(12.5px * var(--rfs, 1)); outline: none; font-family: inherit; }
.in:focus { border-color: #2c6bed; }
.p { background: #2c6bed; border: none; color: #fff; font-weight: 700; border-radius: 6px; padding: 5px 12px;
     font-size: calc(12px * var(--rfs, 1)); cursor: pointer; }
.p:disabled { background: #3a3a3a; color: #777; cursor: default; }
.err { color: #e05555; font-size: calc(11px * var(--rfs, 1)); margin-top: 4px; }
</style>
