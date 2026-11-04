<!-- The voice controls: mute and deafen toggles, the push-to-talk button,
     where the audio is, and the transmit mode. Audio is this browser's: the
     button is the browser's talk key, held from pointer down until pointer
     up, leave or cancel (the window also lets go when it loses focus), and
     it keys only this browser's microphone. It is lit while the node says it
     is transmitting, shows the microphone level while sending, and is
     disabled, with the reason as its title, when we may not speak or there
     is no voice session; when the first hop is what stops us (too slow for
     the room, or its airtime budget spent) that reason is shown beside the
     button. The audio line says whether the audio is here or on
     the device, or why nothing is sent from here (including browser audio
     output not yet allowed to run), and offers "Listen here" when the room is
     joined and the audio is not in this browser or is paused. -->
<template>
  <div class="bar">
    <button class="tg" :class="{ on: muted }" :disabled="textOnly" :title="muted ? 'Unmute' : 'Mute'"
            @click="emit('mute', !muted)">
      <q-icon :name="muted ? matMicOff : matMic" size="18px" />
    </button>
    <button class="tg" :class="{ on: deaf }" :disabled="textOnly" :title="deaf ? 'Undeafen' : 'Deafen'"
            @click="emit('deaf', !deaf)">
      <q-icon :name="deaf ? matHeadsetOff : matHeadset" size="18px" />
    </button>

    <button class="ptt" :class="{ held, talking, dead: held && audio.warn }" :disabled="!!blocked"
            :title="blocked || 'Hold to talk (or hold Space)'"
            @pointerdown="down" @pointerup="release" @pointerleave="up" @pointercancel="up"
            @lostpointercapture="up" @contextmenu.prevent>
      <q-icon :name="matMic" size="18px" />
      <span>{{ label }}</span>
      <span v-if="audio.level > 0" class="lvl" :style="{ width: Math.round(audio.level * 100) + '%' }" />
    </button>

    <div class="info">
      <div v-if="route" class="aud warn"><span class="at" :title="route">{{ route }}</span></div>
      <div class="aud" :class="{ warn: audio.warn }">
        <span v-if="audio.text" class="at">{{ audio.text }}</span>
        <button v-if="audio.offerListen" class="lh" @click="emit('listen')">Listen here</button>
      </div>
      <div class="mode">{{ modeText }}</div>
      <div v-if="profile" class="prof" :title="detail || profile">{{ profile }}</div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, watch } from 'vue'
import { matMic, matMicOff, matHeadset, matHeadsetOff } from '@quasar/extras/material-icons'
import { txLabel } from '../../lib/partylineText'
import type { PlAudio } from '../../modules/partyline'

const props = defineProps<{
  muted: boolean
  deaf: boolean
  textOnly: boolean
  /* Why the talk key can't be held, '' when it can. */
  blocked: string
  /* partyline.tx_blocked: why the first hop carries none of our audio, '' when it does. */
  route: string
  /* The talk key as this page holds it, and the node transmitting. */
  held: boolean
  talking: boolean
  /* s.lxst.tx */
  tx: string
  /* The voice session's profile, in words. */
  profile: string
  /* The session's counters, one per line, for the profile's tooltip. */
  detail: string
  /* Audio in this browser. */
  audio: PlAudio
}>()
const emit = defineEmits<{
  mute: [v: boolean]
  deaf: [v: boolean]
  ptt: [on: boolean]
  listen: []
  resume: []
}>()

const modeText = computed(() => txLabel(props.tx))
const blocked = computed(() => props.blocked)

const label = computed(() =>
  props.talking ? 'Talking' : props.held ? (props.audio.warn ? 'Not transmitting' : 'Keying…') : 'Hold to talk')

function down(e: PointerEvent) {
  e.preventDefault()
  if (blocked.value || e.button !== 0) return
  emit('ptt', true)
}
function up() { if (props.held) emit('ptt', false) }
/* On touch devices only the release may start audio output. */
function release() {
  emit('resume')
  up()
}

watch(blocked, (b) => { if (b) up() })
onBeforeUnmount(up)
</script>

<style scoped>
.bar { display: flex; align-items: center; gap: 8px; padding: 8px 12px; border-top: 1px solid #333; background: #1f1f1f; }
.tg { display: inline-flex; align-items: center; justify-content: center; width: 36px; height: 36px; border-radius: 8px;
      background: #2a2a2a; border: 1px solid #3a3a3a; color: #cfcfcf; cursor: pointer; flex: none; }
.tg:hover:not(:disabled) { border-color: #666; }
.tg.on { background: #4a2626; border-color: #8a4545; color: #f0a0a0; }
.tg:disabled { opacity: .4; cursor: default; }
.ptt { position: relative; overflow: hidden; display: inline-flex; align-items: center; justify-content: center; gap: 6px;
       flex: 1; max-width: 240px; min-width: 120px; height: 40px; border-radius: 20px; border: 2px solid #2c6bed;
       background: #1d2a44; color: #dfe8ff; font-weight: 700; font-size: calc(13px * var(--rfs, 1)); cursor: pointer;
       user-select: none; -webkit-user-select: none; touch-action: none; }
.ptt.held { background: #2c6bed; color: #fff; }
.ptt.talking { background: #2f8a4a; border-color: #4abf6a; color: #fff; box-shadow: 0 0 12px rgba(74, 191, 106, .5); }
.ptt.dead { background: #4a2626; border-color: #8a4545; color: #f0a0a0; }
.ptt:disabled { border-color: #444; background: #262626; color: #777; cursor: default; }
.lvl { position: absolute; left: 0; bottom: 0; height: 3px; background: #9fe0b0; transition: width 60ms linear; }
.info { min-width: 0; flex: 1; font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; line-height: 1.3; }
.aud { display: flex; align-items: center; gap: 6px; min-width: 0; color: #b8c8e8; }
.aud.warn { color: #e09b6a; }
.at { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; min-width: 0; }
.lh { flex: none; background: #2c6bed; border: none; color: #fff; border-radius: 5px; padding: 1px 8px; cursor: pointer;
      font-size: calc(11px * var(--rfs, 1)); font-weight: 700; }
.mode { white-space: nowrap; }
.prof { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: #777; }
</style>
