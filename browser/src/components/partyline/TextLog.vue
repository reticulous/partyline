<!-- The node's text log as Internet Relay Chat (IRC) style lines: a time gutter on the first line of
     each minute, the speaker in the colour of their identity, the text with
     a hanging indent. A poke is amber and says who it is from, a server
     notice is blue-grey, a system line is grey italic; a line said in
     another room than the one we are in carries that room's name. -->
<template>
  <div ref="scroller" class="log">
    <div v-if="rows.length === 0" class="empty">Nothing said yet while this node was listening.</div>
    <div v-for="r in rows" :key="r.line.order" class="wl" :class="r.line.kind">
      <span class="g">{{ r.gutter }}</span>
      <span class="x"><span v-if="r.roomName" class="rm">{{ r.roomName }}</span><b
          v-if="r.line.kind === 'poke'">poke from </b><b v-if="r.line.kind === 'poke' || r.line.kind === 'text'"
          class="nk" :style="{ color: r.colour }" @click="emit('member', r.line.mid)">{{ r.line.name || r.line.mid }}</b><span
          v-if="r.line.kind === 'poke' || r.line.kind === 'text'" class="sep">{{ r.line.kind === 'poke' ? ': ' : ' ' }}</span>{{ r.line.text }}</span>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, ref, watch, onMounted } from 'vue'
import type { PlLine, PlRoom } from '../../modules/partyline'
import { hhmm, memberHue, nickColour } from '../../lib/partylineText'

const props = defineProps<{
  lines: PlLine[]
  rooms: PlRoom[]
  /* The room we are in, '' = none. */
  current: string
  /* mid → identity, for a speaker's colour. */
  identityOf: (mid: string) => string
}>()
const emit = defineEmits<{ member: [mid: string] }>()

const rows = computed(() => {
  let lastMin = -1
  const roomName = new Map(props.rooms.map(r => [r.id, r.name]))
  return props.lines.map((l) => {
    const m = Math.floor(l.ts / 60)
    const gutter = l.ts && m !== lastMin ? hhmm(l.ts) : ''
    if (l.ts) lastMin = m
    const other = l.room !== '' && props.current !== '' && l.room !== props.current
    return {
      line: l,
      gutter,
      roomName: other ? (roomName.get(l.room) || `room ${l.room}`) : '',
      colour: nickColour(memberHue(props.identityOf(l.mid) || l.mid)),
    }
  })
})

const scroller = ref<HTMLElement | null>(null)

function atBottom(): boolean {
  const el = scroller.value
  return !el || el.scrollHeight - el.scrollTop - el.clientHeight <= 48
}
function toBottom() {
  nextTick(() => { const el = scroller.value; if (el) el.scrollTop = el.scrollHeight })
}
defineExpose({ toBottom })

/* New lines follow to the bottom only when the reader was already there
 * (this runs before the DOM update, so atBottom() is the old position). */
watch(rows, () => { if (atBottom()) toBottom() })
onMounted(toBottom)
</script>

<style scoped>
.log { flex: 1; min-height: 0; overflow-y: auto; padding: 8px 0;
       font-size: calc(13.5px * var(--rfs, 1)); line-height: 1.45; color: #e8e8e8; }
.empty { color: #777; font-style: italic; text-align: center; padding: 24px; font-size: calc(13px * var(--rfs, 1)); }
.wl { display: flex; padding: 1px 14px 1px 0; }
.wl:hover { background: #282828; }
.g { flex: none; width: 3.6em; color: #6a6a6a; font-size: calc(11px * var(--rfs, 1)); text-align: right;
     padding: 2px 10px 0 6px; font-variant-numeric: tabular-nums; }
.x { flex: 1; min-width: 0; white-space: pre-wrap; word-break: break-word; }
.nk { cursor: pointer; }
.rm { font-size: calc(10.5px * var(--rfs, 1)); color: #8a8a8a; border: 1px solid #3a3a3a; border-radius: 4px;
      padding: 0 4px; margin-right: 6px; }
.wl.poke { background: rgba(232, 163, 58, .07); }
.wl.poke .x { color: #e8c58a; box-shadow: inset 2px 0 #e8a33a; padding-left: 6px; }
.wl.notice .x { color: #8fa3b6; }
.wl.system .x { color: #8a8a8a; font-style: italic; }
</style>
