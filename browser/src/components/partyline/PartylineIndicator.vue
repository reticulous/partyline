<!-- The header's room indicator: while in a room, a headset (a struck
     microphone while muted, a struck headset while deafened) and the room
     name, green while anyone in the room speaks. A click opens the Partyline
     window; the caret's menu also offers Leave. For a while after a retry
     lands back in a room, a "Rejoined" bubble hangs under it. Nothing when not
     in a room. -->
<template>
  <span v-if="inRoom" class="pli" :class="{ speak: speaking }" :title="tooltip" @click="showPartyline">
    <q-icon :name="icon" size="15px" />
    <span class="t">{{ shortRoom }}</span>
    <span class="more" title="Partyline" @click.stop>
      <q-icon :name="matArrowDropDown" size="16px" />
      <q-menu anchor="bottom right" self="top right">
        <q-list dark dense style="min-width: 140px">
          <q-item v-close-popup clickable @click="showPartyline">
            <q-item-section>Open Partyline</q-item-section>
          </q-item>
          <q-item v-close-popup clickable @click="pl.disconnect()">
            <q-item-section>Leave {{ shortRoom }}</q-item-section>
          </q-item>
        </q-list>
      </q-menu>
    </span>
    <span v-if="rejoinNotice" class="bubble" @click.stop="dismiss">{{ rejoinNotice }}</span>
  </span>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { matHeadset, matHeadsetOff, matMicOff, matArrowDropDown } from '@quasar/extras/material-icons'
import { usePartyline, showPartyline, rejoinNotice } from '../../modules/partyline'
import { serverLabel } from '../../lib/partylineText'

const ROOM_MAX = 16
const dismiss = () => { rejoinNotice.value = '' }

const pl = usePartyline()
const s = computed(() => pl.session.value)

const inRoom = computed(() => s.value.state === 'live' && s.value.room !== '')
const roomName = computed(() => {
  const r = pl.rooms.value.find(x => x.id === s.value.room)
  return r?.name || `room ${s.value.room}`
})
const shortRoom = computed(() => {
  const chars = [...roomName.value]
  return chars.length > ROOM_MAX ? chars.slice(0, ROOM_MAX - 1).join('') + '…' : roomName.value
})
const serverName = computed(() =>
  serverLabel(s.value.name || pl.serverOf(s.value.server)?.name || '', s.value.server))
const tooltip = computed(() => `${roomName.value} on ${serverName.value}`)

const icon = computed(() => pl.deaf.value ? matHeadsetOff : pl.muted.value ? matMicOff : matHeadset)
const speaking = computed(() =>
  pl.voice.value.talking || pl.members.value.some(m => m.room === s.value.room && m.speaking))
</script>

<style scoped>
.pli { position: relative; display: inline-flex; align-items: center; gap: 4px; padding: 1px 2px 1px 8px;
       border-radius: 10px; cursor: pointer; background: rgba(255, 255, 255, .1); color: #e8e8e8;
       font-size: 12px; white-space: nowrap; transition: background-color .15s, box-shadow .15s; }
.pli.speak { background: #2f8a4a; color: #fff; box-shadow: 0 0 8px rgba(74, 191, 106, .8); }
.t { overflow: hidden; text-overflow: ellipsis; }
.more { display: inline-flex; align-items: center; border-radius: 8px; }
.more:hover { background: rgba(255, 255, 255, .15); }
.bubble { position: absolute; top: calc(100% + 6px); right: 0; z-index: 3000; padding: 5px 10px;
          border-radius: 6px; background: #2f2f2f; border: 1px solid #454545; border-left: 3px solid #4abf6a;
          color: #e8e8e8; font-size: 12px; box-shadow: 0 8px 24px rgba(0, 0, 0, .5); cursor: default; }
</style>
