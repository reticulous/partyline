<!-- The left rail: the saved servers (and the one held, saved or not), then
     the servers heard on the mesh and not saved, then "+ Add server". The
     server held carries a status dot and the firmware's status text; a heard
     server shows its hops and how long ago, and a lock or a list icon when it
     wants a server password or keeps an allow list. Every row only selects
     what the centre column shows; nothing here connects. -->
<template>
  <div class="rail">
    <div class="lbl">SERVERS</div>
    <div v-if="servers.length === 0" class="none">No servers saved</div>
    <div v-for="s in servers" :key="s.dest" class="srv"
         :class="{ on: sel.kind !== '' && sel.id === s.dest }" :title="s.dest"
         @click="emit('open', s.dest)">
      <span class="hdot" :class="s.dest === session.server ? dotClass(session.state) : 'off'" />
      <span class="sn" :class="{ anon: !s.name }">{{ serverLabel(s.name, s.dest) }}</span>
      <FlagIcons :flags="s.heard?.flags ?? 0" />
      <span v-if="s.auto" class="auto" title="Connects at boot">auto</span>
      <span class="sp" />
      <span class="rt">{{ s.dest === session.server ? rightOf(session) : '' }}</span>
    </div>

    <template v-if="heard.length">
      <div class="lbl mesh">ON THE MESH</div>
      <div v-for="h in heard" :key="h.dest" class="heard"
           :class="{ on: sel.kind !== '' && sel.id === h.dest }" :title="h.desc || h.dest"
           @click="emit('open', h.dest)">
        <span class="sn" :class="{ anon: !h.name }">{{ serverLabel(h.name, h.dest) }}</span>
        <FlagIcons :flags="h.flags" />
        <span class="sp" />
        <span class="meta">{{ hopsText(h.hops) }}<template v-if="h.lastSeen"> · {{ ago(h.lastSeen) }}</template></span>
      </div>
    </template>

    <div v-if="!adding" class="add" @click="startAdd">+ Add server</div>
    <div v-else class="addbox">
      <input ref="addEl" v-model="addText" class="addin" placeholder="server hash (32 hex)"
             spellcheck="false" autocomplete="off"
             @keydown.enter="doAdd" @keydown.esc="adding = false" />
      <div v-if="addErr" class="adderr">{{ addErr }}</div>
      <div class="addbtns">
        <button class="btn" @click="adding = false">Cancel</button>
        <button class="btn p" :disabled="!addText.trim()" @click="doAdd">Add</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { nextTick, ref } from 'vue'
import type { PlServer, PlHeard, PlSession, LinkState } from '../../modules/partyline'
import { serverLabel, ago, parseDest } from '../../lib/partylineText'
import FlagIcons from './FlagIcons.vue'

defineProps<{
  servers: PlServer[]
  heard: PlHeard[]
  session: PlSession
  sel: { kind: '' | 'server' | 'live'; id: string }
}>()
const emit = defineEmits<{
  open: [dest: string]
  add: [dest: string]
}>()

function dotClass(st: LinkState): string {
  switch (st) {
    case 'live': return 'up'
    case 'connecting': case 'joining': return 'busy'
    case 'denied': case 'lost': return 'down'
    default: return 'off'
  }
}

function rightOf(s: PlSession): string {
  return s.state === 'live' ? '' : s.status
}

function hopsText(h: number): string {
  return h < 0 ? '' : `${h} hop${h === 1 ? '' : 's'}`
}

const adding = ref(false)
const addText = ref('')
const addErr = ref('')
const addEl = ref<HTMLInputElement | null>(null)

function startAdd() {
  adding.value = true
  addText.value = ''
  addErr.value = ''
  nextTick(() => addEl.value?.focus())
}

function doAdd() {
  const d = parseDest(addText.value)
  if (!d) { addErr.value = 'A server is 32 hex characters.'; return }
  adding.value = false
  emit('add', d)
}
</script>

<style scoped>
.rail { height: 100%; overflow-y: auto; background: #1c1c1c; padding-bottom: 12px;
        font-size: calc(13px * var(--rfs, 1)); color: #e8e8e8; user-select: none; }
.lbl { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; letter-spacing: .8px; padding: 12px 14px 4px; }
.lbl.mesh { margin-top: 10px; }
.none { color: #777; font-style: italic; padding: 4px 14px; font-size: calc(12px * var(--rfs, 1)); }
.srv { display: flex; align-items: center; gap: 6px; padding: 6px 14px; font-weight: 700; cursor: pointer; }
.srv:hover, .heard:hover { background: #262626; }
.srv.on, .heard.on { background: #2c3443; box-shadow: inset 3px 0 0 #2c6bed; }
.sn { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-width: 0; }
.sn.anon { font-style: italic; font-weight: 400; color: #b0b0b0; }
.sp { flex: 1; }
.rt { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; font-weight: 400;
      white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 50%; }
.auto { font-size: calc(10px * var(--rfs, 1)); color: #8fa3b6; border: 1px solid #3c4a58; border-radius: 4px;
        padding: 0 4px; font-weight: 400; }
.hdot { width: 8px; height: 8px; border-radius: 50%; flex: none; background: #555; }
.hdot.up { background: #4abf6a; }
.hdot.busy { background: #d6a12a; }
.hdot.down { background: #c0504d; }
.heard { display: flex; align-items: center; gap: 6px; padding: 4px 14px 4px 28px; color: #b8b8b8;
         font-size: calc(12px * var(--rfs, 1)); cursor: pointer; }
.meta { white-space: nowrap; font-size: calc(11px * var(--rfs, 1)); color: #8a8a8a; }
.add { padding: 8px 14px; color: #7fa6ff; cursor: pointer; margin-top: 6px; }
.add:hover { color: #a9c2ff; }
.addbox { padding: 8px 12px; }
.addin { width: 100%; background: #232323; color: #e8e8e8; border: 1px solid #3a3a3a; border-radius: 6px;
         padding: 6px 8px; font-size: calc(12px * var(--rfs, 1)); outline: none; font-family: inherit; }
.addin:focus { border-color: #2c6bed; }
.adderr { color: #d98a8a; font-size: calc(11px * var(--rfs, 1)); margin-top: 4px; }
.addbtns { display: flex; justify-content: flex-end; gap: 6px; margin-top: 6px; }
.btn { background: none; border: 1px solid #555; color: #e8e8e8; border-radius: 6px; padding: 3px 10px;
       font-size: calc(12px * var(--rfs, 1)); cursor: pointer; }
.btn.p { background: #2c6bed; border-color: #2c6bed; color: #fff; font-weight: 700; }
.btn:disabled { opacity: .45; cursor: default; }
</style>
