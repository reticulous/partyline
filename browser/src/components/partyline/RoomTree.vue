<!-- The server's rooms as a tree: each room with its codec profile in words,
     a PTT badge when it is a push-to-talk room (ptt_jitter_ms > 0), a lock
     when it wants a password, and the members in it underneath, each with
     their muted / deafened / operator / text-only state and lit while they
     are heard speaking. Tapping another room asks to move there; the room we
     are in is not a target. Tapping a member opens their card. -->
<template>
  <div class="tree">
    <div class="lbl">ROOMS</div>
    <div v-if="rooms.length === 0" class="none">No rooms listed yet</div>
    <template v-for="r in rooms" :key="r.id">
      <div class="room" :class="{ here: r.id === current }"
           :title="r.id === current ? 'You are here' : `Move to ${r.name}`"
           @click="r.id !== current && emit('move', r)">
        <div class="rl">
          <q-icon v-if="r.access & ACCESS_PASSWORD" :name="matLock" size="13px" class="lock" title="Needs a password" />
          <span class="rn">{{ r.name || `room ${r.id}` }}</span>
          <span v-if="r.pttJitterMs > 0" class="badge" title="Push-to-talk room">PTT</span>
          <span v-if="r.flags & ROOM_BROADCAST" class="badge b" title="Broadcast room">BC</span>
          <span class="sp" />
          <span class="cnt">{{ byRoom.get(r.id)?.length ?? r.members }}</span>
        </div>
        <div v-if="r.profileText || r.desc" class="rp">
          {{ r.profileText }}<template v-if="r.profileText && r.desc"> · </template>{{ r.desc }}
        </div>
      </div>
      <div v-for="m in byRoom.get(r.id) ?? []" :key="m.mid" class="mem"
           :class="{ spk: m.speaking || (m.self && selfTalking), self: m.self }"
           @click="emit('member', m)">
        <span class="sd" />
        <span class="mn" :style="{ color: nickColour(memberHue(m.identity || m.mid)) }">{{ m.name || `member ${m.mid}` }}</span>
        <span class="ic">
          <q-icon v-if="m.op" :name="matShield" size="13px" title="Operator" class="op" />
          <q-icon v-if="m.textOnly" :name="matChat" size="13px" title="Text only" />
          <q-icon v-else-if="m.serverMuted" :name="matMicOff" size="13px" title="Muted by an operator" class="sm" />
          <q-icon v-else-if="m.muted" :name="matMicOff" size="13px" title="Muted" />
          <q-icon v-if="m.deaf && !m.textOnly" :name="matHeadsetOff" size="13px" title="Deafened" />
        </span>
      </div>
    </template>
    <template v-if="nowhere.length">
      <div class="room nowhere"><div class="rl"><span class="rn">In no room</span></div></div>
      <div v-for="m in nowhere" :key="m.mid" class="mem" :class="{ self: m.self }" @click="emit('member', m)">
        <span class="sd" />
        <span class="mn">{{ m.name || `member ${m.mid}` }}</span>
      </div>
    </template>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { matLock, matShield, matChat, matMicOff, matHeadsetOff } from '@quasar/extras/material-icons'
import { ACCESS_PASSWORD, ROOM_BROADCAST, type PlRoom, type PlMember } from '../../modules/partyline'
import { memberHue, nickColour } from '../../lib/partylineText'

const props = defineProps<{
  rooms: PlRoom[]
  members: PlMember[]
  /* The room we are in, '' = none. */
  current: string
  /* Our own transmitter is keyed. */
  selfTalking: boolean
}>()
const emit = defineEmits<{
  move: [room: PlRoom]
  member: [m: PlMember]
}>()

const byRoom = computed(() => {
  const out = new Map<string, PlMember[]>()
  for (const m of props.members) {
    const a = out.get(m.room)
    if (a) a.push(m); else out.set(m.room, [m])
  }
  return out
})

const nowhere = computed(() => {
  const ids = new Set(props.rooms.map(r => r.id))
  return props.members.filter(m => !ids.has(m.room))
})
</script>

<style scoped>
.tree { height: 100%; overflow-y: auto; background: #1c1c1c; padding-bottom: 12px;
        font-size: calc(13px * var(--rfs, 1)); color: #e8e8e8; user-select: none; }
.lbl { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; letter-spacing: .8px; padding: 12px 14px 4px; }
.none { color: #777; font-style: italic; padding: 4px 14px; font-size: calc(12px * var(--rfs, 1)); }
.room { padding: 6px 12px 4px; cursor: pointer; margin-top: 2px; }
.room:hover { background: #262626; }
.room.here { background: #2c3443; box-shadow: inset 3px 0 0 #2c6bed; cursor: default; }
.room.nowhere { cursor: default; color: #9a9a9a; }
.room.nowhere:hover { background: none; }
.rl { display: flex; align-items: center; gap: 5px; }
.rn { font-weight: 700; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-width: 0; }
.lock { color: #c9a14a; flex: none; }
.badge { font-size: calc(9.5px * var(--rfs, 1)); font-weight: 700; letter-spacing: .4px; color: #d6a12a;
         border: 1px solid #6a5420; border-radius: 4px; padding: 0 3px; flex: none; }
.badge.b { color: #8fa3b6; border-color: #3c4a58; }
.sp { flex: 1; }
.cnt { color: #8a8a8a; font-size: calc(11px * var(--rfs, 1)); }
.rp { color: #8a8a8a; font-size: calc(11px * var(--rfs, 1)); overflow: hidden; text-overflow: ellipsis;
      white-space: nowrap; margin-top: 1px; }
.mem { display: flex; align-items: center; gap: 6px; padding: 2px 12px 2px 22px; cursor: pointer; border-radius: 4px; }
.mem:hover { background: #262626; }
.sd { width: 7px; height: 7px; border-radius: 50%; flex: none; background: #3a3a3a; transition: background .1s; }
.mem.spk .sd { background: #4abf6a; box-shadow: 0 0 6px #4abf6a; }
.mem.spk .mn { text-shadow: 0 0 8px rgba(74, 191, 106, .55); }
.mn { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-width: 0; flex: 1; }
.mem.self .mn { font-weight: 700; }
.ic { display: inline-flex; gap: 3px; color: #9a9a9a; flex: none; }
.op { color: #7fa6ff; }
.sm { color: #d96a6a; }
</style>
