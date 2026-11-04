<!-- One server in the centre column: what is known of it (its status as the
     firmware words it while held, else how it was heard), and the connect
     form — the room to ask for, that room's password, the server password
     and the identity to present — with Connect / Disconnect, Save / Forget
     and the connect-at-boot switch for a saved server. Opening a server here
     never connects. -->
<template>
  <div class="srvv">
    <div class="hd">
      <button v-if="showBack" class="back" title="Back" @click="emit('back')">‹</button>
      <div class="ttl">
        <div class="nm" :class="{ anon: !server.name }">{{ serverLabel(server.name, server.dest) }}</div>
        <div class="hh">{{ groupHex(server.dest) }}</div>
      </div>
    </div>

    <div class="body">
      <div class="status">
        <span class="hdot" :class="dot" />
        <span v-if="held && session.status">{{ session.status }}</span>
        <span v-else-if="server.heard">Heard{{ server.heard.hops >= 0 ? ` ${server.heard.hops} hop${server.heard.hops === 1 ? '' : 's'} away` : '' }}<template v-if="server.heard.lastSeen">, {{ ago(server.heard.lastSeen) }} ago</template></span>
        <span v-else class="dim">Not heard on the mesh yet</span>
      </div>
      <div v-if="server.heard?.desc" class="desc">{{ server.heard.desc }}</div>
      <div v-if="heardMeta" class="dim small">{{ heardMeta }}</div>
      <div v-if="flags & ANNOUNCE_PASSWORD" class="flag"><q-icon :name="matLock" size="14px" /> Needs a server password.</div>
      <div v-if="flags & ANNOUNCE_ALLOWLIST" class="flag"><q-icon :name="matPlaylistAddCheck" size="15px" /> Only identities on its allow list may connect.</div>
      <div v-if="error" class="err">{{ error }}</div>

      <div class="btns">
        <button v-if="busy" class="btn" @click="emit('disconnect')">Disconnect</button>
        <button v-if="held && session.state === 'live'" class="btn p" @click="emit('open-live')">Open the room</button>
        <button v-if="!server.saved" class="btn" title="Keep this server in the list without connecting"
                @click="emit('add')">Save</button>
        <button v-if="server.saved" class="btn danger" @click="forget">Forget</button>
      </div>
      <label v-if="server.saved" class="sw">
        <input type="checkbox" :checked="server.auto" @change="e => emit('auto', (e.target as HTMLInputElement).checked)" />
        Connect at boot
      </label>

      <div v-if="!busy" class="sec">
        <div class="k">Connect</div>
        <div class="grid">
          <label>Room</label>
          <input v-model="room" class="in" placeholder="the server's default room" spellcheck="false"
                 @keydown.enter="doConnect" />
          <label>Room password</label>
          <input v-model="roomPass" class="in" type="password" placeholder="if the room has one"
                 autocomplete="off" @keydown.enter="doConnect" />
          <label>Server password</label>
          <input v-model="serverPass" class="in" type="password"
                 :placeholder="flags & ANNOUNCE_PASSWORD ? 'needed' : 'if the server has one'"
                 autocomplete="off" @keydown.enter="doConnect" />
        </div>

        <div class="k sub">Join as</div>
        <div v-for="i in identities" :key="i.sel" class="idrow" :class="{ on: pick === i.sel }"
             @click="pick = i.sel">
          <span class="radio" />
          <span class="idn">{{ i.label }}</span>
          <span class="idh">{{ i.hash ? i.hash.slice(0, 8) : '—' }}</span>
        </div>
        <div class="hint">The server knows you by this identity: operators, bans and allow lists key on it.</div>
        <div class="btns">
          <button class="btn p" :disabled="!pick" @click="doConnect">Connect</button>
        </div>
      </div>

      <div class="sec">
        <div class="k">Nick</div>
        <div class="row">
          <input v-model="nickText" class="in" placeholder="the LXMF display name" spellcheck="false"
                 @keydown.enter="emit('nick', nickText.trim())" />
          <button class="btn" :disabled="nickText.trim() === nick" @click="emit('nick', nickText.trim())">Set</button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { matLock, matPlaylistAddCheck } from '@quasar/extras/material-icons'
import { ANNOUNCE_PASSWORD, ANNOUNCE_ALLOWLIST,
         type PlServer, type PlSession, type PlIdentity, type ConnectArgs } from '../../modules/partyline'
import { groupHex, serverLabel, ago } from '../../lib/partylineText'

const props = defineProps<{
  server: PlServer
  session: PlSession
  identities: PlIdentity[]
  nick: string
  /* The task's answer to the last connect, '' = none. */
  error: string
  showBack: boolean
}>()
const emit = defineEmits<{
  back: []
  connect: [a: ConnectArgs]
  disconnect: []
  'open-live': []
  add: []
  forget: []
  auto: [on: boolean]
  nick: [name: string]
}>()

const held = computed(() => props.session.server === props.server.dest)
/* Holding the link, or on the way to it. */
const busy = computed(() => held.value && ['connecting', 'joining', 'live'].includes(props.session.state))
const flags = computed(() => props.server.heard?.flags ?? 0)

const dot = computed(() => {
  if (!held.value) return 'off'
  switch (props.session.state) {
    case 'live': return 'up'
    case 'connecting': case 'joining': return 'busy'
    case 'denied': case 'lost': return 'down'
    default: return 'off'
  }
})

const heardMeta = computed(() => {
  const h = props.server.heard
  if (!h) return ''
  return [h.lang, h.country].filter(Boolean).join(' · ')
})

const room = ref('')
const roomPass = ref('')
const serverPass = ref('')
const pick = ref('')
const nickText = ref('')

/* The saved choice, else the only LXMF identity, else the first listed. */
function defaultPick(): string {
  const ids = props.identities
  if (props.server.ident && ids.some(i => i.sel === props.server.ident)) return props.server.ident
  return ids[0]?.sel ?? ''
}

watch(() => props.server.dest, () => {
  room.value = props.server.room
  roomPass.value = ''
  serverPass.value = ''
  pick.value = defaultPick()
}, { immediate: true })
watch(() => props.identities.length, () => { if (!pick.value) pick.value = defaultPick() })
watch(() => props.nick, (n) => { nickText.value = n }, { immediate: true })

function doConnect() {
  if (!pick.value) return
  emit('connect', {
    server: props.server.dest,
    room: room.value.trim(),
    password: roomPass.value,
    server_password: serverPass.value,
    ident: pick.value,
  })
  roomPass.value = ''
  serverPass.value = ''
}

function forget() {
  const name = serverLabel(props.server.name, props.server.dest)
  if (!window.confirm(`Forget ${name}?`)) return
  emit('forget')
}
</script>

<style scoped>
.srvv { display: flex; flex-direction: column; height: 100%; min-height: 0; color: #e8e8e8;
        font-size: calc(13px * var(--rfs, 1)); }
.hd { display: flex; align-items: center; gap: 10px; padding: 10px 16px; border-bottom: 1px solid #333; }
.back { background: none; border: none; color: #9a9a9a; font-size: 22px; cursor: pointer; padding: 0 4px; line-height: 1; }
.ttl { min-width: 0; }
.nm { font-weight: 700; font-size: calc(15px * var(--rfs, 1)); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.nm.anon { font-style: italic; font-weight: 400; }
.hh { font-family: "DejaVu Sans Mono", Menlo, monospace; color: #9a9a9a; font-size: calc(11px * var(--rfs, 1)); }
.body { flex: 1; overflow-y: auto; padding: 12px 16px 20px; }
.status { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.hdot { width: 8px; height: 8px; border-radius: 50%; flex: none; background: #555; }
.hdot.up { background: #4abf6a; }
.hdot.busy { background: #d6a12a; }
.hdot.down { background: #c0504d; }
.desc { margin-top: 8px; color: #cfcfcf; }
.dim { color: #9a9a9a; }
.small { font-size: calc(12px * var(--rfs, 1)); margin-top: 4px; }
.flag { display: flex; align-items: center; gap: 6px; color: #c9a14a; margin-top: 6px; font-size: calc(12px * var(--rfs, 1)); }
.err { color: #e09b6a; margin-top: 8px; font-size: calc(12px * var(--rfs, 1)); }
.btns { display: flex; gap: 8px; margin: 12px 0 4px; flex-wrap: wrap; }
.btn { background: none; border: 1px solid #555; color: #e8e8e8; border-radius: 6px; padding: 5px 12px;
       font-size: calc(12px * var(--rfs, 1)); cursor: pointer; white-space: nowrap; }
.btn:hover:not(:disabled) { border-color: #777; }
.btn.p { background: #2c6bed; border-color: #2c6bed; color: #fff; font-weight: 700; }
.btn.danger { border-color: #8a4545; color: #e09090; }
.btn:disabled { opacity: .45; cursor: default; }
.sw { display: inline-flex; align-items: center; gap: 8px; margin-top: 6px; cursor: pointer; color: #cfcfcf; }
.sec { margin-top: 14px; padding-top: 10px; border-top: 1px solid #333; }
.k { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; letter-spacing: .6px; text-transform: uppercase; margin-bottom: 6px; }
.k.sub { margin-top: 14px; }
.grid { display: grid; grid-template-columns: auto 1fr; gap: 6px 10px; align-items: center; }
.grid label { color: #9a9a9a; font-size: calc(12px * var(--rfs, 1)); white-space: nowrap; }
.row { display: flex; gap: 8px; align-items: center; }
.in { flex: 1; min-width: 0; background: #1c1c1c; color: #e8e8e8; border: 1px solid #3a3a3a; border-radius: 6px;
      padding: 6px 8px; font-size: calc(12.5px * var(--rfs, 1)); outline: none; font-family: inherit; }
.in:focus { border-color: #2c6bed; }
.hint { color: #777; font-size: calc(11px * var(--rfs, 1)); margin-top: 6px; }
.idrow { display: flex; align-items: center; gap: 10px; padding: 5px 4px; cursor: pointer; border-radius: 5px; }
.idrow:hover { background: #2a2a2a; }
.radio { width: 12px; height: 12px; border-radius: 50%; border: 2px solid #777; flex: none; }
.idrow.on .radio { border-color: #2c6bed; background: #2c6bed; box-shadow: inset 0 0 0 2px #232323; }
.idn { flex: 1; }
.idh { font-family: "DejaVu Sans Mono", Menlo, monospace; color: #777; font-size: calc(11px * var(--rfs, 1)); }
</style>
