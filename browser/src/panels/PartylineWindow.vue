<!-- The Partyline window. Three columns on a desktop: the server rail, the
     centre (the room we are in — connection line, message of the day, text
     and the voice controls — or a server's connect view) and the room tree
     with its members, which can be hidden. On a phone the rail and the
     centre slide, as rrc's do, and the room tree is a sheet over the room.
     This is the only place layout is decided; the components below take what
     modules/partyline.ts computes and say what was asked for. Holding Space
     while no text field has focus is the talk key, as the button is. -->
<template>
  <FloatingWindow
    id="partyline"
    :title="title"
    :visible="visible"
    :default-geom="defaultGeom"
    :min-size="{ w: 30, h: 24 }"
    @update:visible="v => emit('update:visible', v)"
  >
    <template #titlebar-right>
      <span class="rfs-btn" title="Smaller" @click="zoomOut">-</span>
      <span class="rfs-btn" title="Larger" @click="zoomIn">+</span>
    </template>

    <template #default>
      <div class="pl" :style="{ '--rfs': scale }">
        <div class="cols" :class="{ mobile: compact, 'show-detail': compact && sel.kind !== '' }">
          <div class="railcol" :style="compact ? undefined : { width: railW + 'px' }">
            <ServerRail :servers="pl.servers.value" :heard="pl.heard.value" :session="s" :sel="sel"
                        @open="openServer" @add="onAdd" />
          </div>

          <div class="center">
            <!-- the room we are in -->
            <template v-if="liveShown">
              <div class="topic">
                <button v-if="compact" class="back" title="Back" @click="clearSel">‹</button>
                <span class="hdot" :class="dot" />
                <span class="hd" :title="headTitle">
                  <template v-if="roomName">In <b>{{ roomName }}</b> on </template>
                  <template v-else>Not in a room · </template>
                  <span class="sn" title="Server" @click="showServer(s.server)">{{ serverName }}</span>
                  <template v-if="roomName"> · {{ hereCount }} here</template>
                </span>
                <span class="sp" />
                <button v-if="holding" class="btn lv" title="Leave the room and disconnect" @click="pl.disconnect()">Leave</button>
                <span class="mem" :class="{ on: treeShown }" title="Rooms and members" @click="treeShown = !treeShown">
                  <q-icon :name="matPeople" size="16px" /> {{ pl.members.value.length }}
                </span>
              </div>
              <div v-if="s.state !== 'live' && s.status" class="stline" @click="showServer(s.server)">{{ s.status }}</div>
              <div v-if="s.motd" class="motd" :class="{ open: motdOpen }">
                <div class="mh" @click="motdOpen = !motdOpen">
                  <q-icon :name="motdOpen ? matExpandMore : matChevronRight" size="16px" />
                  <span>Message of the day</span>
                </div>
                <pre v-if="motdOpen" class="mt">{{ s.motd }}</pre>
              </div>
              <div v-if="liveError" class="errline">{{ liveError }}</div>
              <TextLog :lines="pl.log.value" :rooms="pl.rooms.value" :current="s.room"
                       :identity-of="identityOf" @member="mid => openMember(mid)" />
              <Composer ref="composerRef"
                        :placeholder="roomName ? `Message ${roomName}` : 'Not in a room'"
                        :disabled="s.state !== 'live' || s.room === ''"
                        :error="pl.cmdError('text')"
                        @send="pl.text" />
              <TalkBar :muted="pl.muted.value" :deaf="pl.deaf.value" :text-only="pl.textOnly.value"
                       :blocked="blocked" :route="s.txBlocked" :held="pl.pttHeld.value" :talking="v.talking"
                       :tx="v.tx" :profile="v.profile" :detail="statsText" :audio="pl.audio.value"
                       @mute="pl.setMuted" @deaf="pl.setDeaf" @ptt="pl.setPtt" @listen="pl.listen" @resume="pl.resume" />
            </template>

            <!-- a server -->
            <ServerView
              v-else-if="sel.kind === 'server' && selServer"
              :server="selServer"
              :session="s"
              :identities="pl.identities.value"
              :nick="pl.nick.value"
              :error="pl.cmdError('connect')"
              :show-back="compact"
              @back="clearSel"
              @connect="onConnect"
              @disconnect="pl.disconnect()"
              @open-live="showLive"
              @add="pl.add(selServer!.dest)"
              @forget="onForget"
              @auto="on => pl.setAuto(selServer!.dest, on)"
              @nick="pl.setNick"
            />

            <!-- nothing chosen -->
            <div v-else class="emptyst">
              <div class="big">Partyline</div>
              <div class="dim">Voice rooms on Reticulum. Pick a server, connect, and talk with the button below the room, or hold Space.</div>
              <template v-if="pl.heard.value.length">
                <div class="k">Servers heard on the mesh</div>
                <div v-for="h in pl.heard.value" :key="h.dest" class="eh" @click="openServer(h.dest)">
                  <span class="ehn" :class="{ anon: !h.name }">{{ serverLabel(h.name, h.dest) }}</span>
                  <span class="dim">{{ h.hops >= 0 ? `${h.hops} hop${h.hops === 1 ? '' : 's'}` : '' }}<template v-if="h.lastSeen"> · {{ ago(h.lastSeen) }}</template></span>
                </div>
              </template>
              <div v-else class="dim small">No server heard announcing yet. A server can be added by its hash with “+ Add server”.</div>
            </div>
          </div>

          <div v-if="liveShown && treeShown && !compact" class="treecol">
            <RoomTree :rooms="pl.rooms.value" :members="pl.members.value" :current="s.room"
                      :self-talking="v.talking" @move="onMove" @member="m => card = m" />
          </div>
        </div>

        <!-- compact: the room tree as a sheet over the room -->
        <div v-if="compact && liveShown && treeShown" class="sheet-bg" @click.self="treeShown = false">
          <div class="sheet">
            <RoomTree :rooms="pl.rooms.value" :members="pl.members.value" :current="s.room"
                      :self-talking="v.talking" @move="onMove" @member="m => card = m" />
          </div>
        </div>

        <MemberCard v-if="card" :member="card" :room-name="roomNameOf(card.room)"
                    @close="card = null" @poke="(mid, t) => pl.poke(mid, t)" />

        <!-- a room that wants a password -->
        <div v-if="pwRoom" class="scrim" @click.self="pwRoom = null">
          <div class="pwcard">
            <div class="pwt"><q-icon :name="matLock" size="15px" /> {{ pwRoom.name || `room ${pwRoom.id}` }}</div>
            <div class="dim">This room needs a password.</div>
            <input ref="pwEl" v-model="pwText" class="in" type="password" autocomplete="off"
                   placeholder="room password" @keydown.enter="submitPw" @keydown.esc="pwRoom = null" />
            <div class="pwb">
              <button class="btn" @click="pwRoom = null">Cancel</button>
              <button class="btn p" @click="submitPw">Move</button>
            </div>
          </div>
        </div>
      </div>
    </template>
  </FloatingWindow>
</template>

<script lang="ts">
/* What the window shows, kept across remounts for the session. */
import { reactive as _reactive } from 'vue'
const sel = _reactive<{ kind: '' | 'server' | 'live'; id: string }>({ kind: '', id: '' })
</script>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { matPeople, matLock, matExpandMore, matChevronRight } from '@quasar/extras/material-icons'
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue'
import { useCompact } from 'spangap-browser/lib/viewport'
import { focusedWindowId } from 'spangap-browser/lib/windows'
import { useWinZoom } from 'rns/lib/winZoom'
import ServerRail from '../components/partyline/ServerRail.vue'
import ServerView from '../components/partyline/ServerView.vue'
import RoomTree from '../components/partyline/RoomTree.vue'
import TextLog from '../components/partyline/TextLog.vue'
import Composer from '../components/partyline/Composer.vue'
import TalkBar from '../components/partyline/TalkBar.vue'
import MemberCard from '../components/partyline/MemberCard.vue'
import { usePartyline, ACCESS_PASSWORD, type ConnectArgs, type PlMember, type PlRoom } from '../modules/partyline'
import { serverLabel, ago, txLabel } from '../lib/partylineText'

const props = defineProps<{ visible: boolean; title: string }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const defaultGeom = { x: 10, y: 6, w: 72, h: 80 }
const pl = usePartyline()
const { scale, zoomIn, zoomOut } = useWinZoom('partyline')
const compact = useCompact()
const railW = 220

const s = computed(() => pl.session.value)
const v = computed(() => pl.voice.value)

/* ── Selection ── */
const holding = computed(() => ['connecting', 'joining', 'live'].includes(s.value.state))

function showLive() { sel.kind = 'live'; sel.id = s.value.server; card.value = null }
function showServer(dest: string) { if (dest) { sel.kind = 'server'; sel.id = dest; card.value = null } }
function clearSel() { sel.kind = ''; sel.id = '' }

/* The server held opens on its room; any other on its connect view. */
function openServer(dest: string) {
  if (dest === s.value.server && holding.value) showLive()
  else showServer(dest)
}

/* A connection coming up (from here, the LCD or at boot) brings its room to
 * the front of a window showing nothing, or showing that server. */
watch(() => [s.value.server, holding.value] as const, ([server, h]) => {
  if (!server || !h) return
  if (sel.kind === '' || (sel.kind === 'server' && sel.id === server)) showLive()
}, { immediate: true })

/* A link ended for good (disconnected, or refused) shows that server's view,
 * with its status and the connect form; a lost link reconnects by itself and
 * keeps the room on screen. */
watch(() => s.value.state, (st) => {
  if (sel.kind === 'live' && (st === 'idle' || st === 'denied' || st === '')) showServer(sel.id)
})

const liveShown = computed(() => sel.kind === 'live' && sel.id !== '' && sel.id === s.value.server)
const selServer = computed(() => sel.kind === 'server' ? pl.serverOf(sel.id) : null)

function onAdd(dest: string) {
  pl.add(dest)
  showServer(dest)
}
function onConnect(a: ConnectArgs) { pl.connect(a) }
function onForget() {
  if (sel.id === s.value.server && holding.value) pl.disconnect()
  pl.forget(sel.id)
  clearSel()
}

/* ── The room we are in ── */
const serverName = computed(() => serverLabel(s.value.name || pl.serverOf(s.value.server)?.name || '', s.value.server))
const roomNameOf = (id: string) => {
  const r = pl.rooms.value.find(x => x.id === id)
  return r ? (r.name || `room ${r.id}`) : ''
}
const roomName = computed(() => roomNameOf(s.value.room))
const hereCount = computed(() => s.value.room === '' ? 0 : pl.members.value.filter(m => m.room === s.value.room).length)
const headTitle = computed(() => roomName.value
  ? `In ${roomName.value} on ${serverName.value} · ${hereCount.value} here` : `Not in a room · ${serverName.value}`)
const dot = computed(() => {
  switch (s.value.state) {
    case 'live': return 'up'
    case 'connecting': case 'joining': return 'busy'
    case 'denied': case 'lost': return 'down'
    default: return 'off'
  }
})
const identityOf = (mid: string) => pl.members.value.find(m => m.mid === mid)?.identity ?? ''

const liveError = computed(() => pl.cmdError('move') || pl.cmdError('state') || pl.cmdError('poke'))

const statsText = computed(() => v.value.stats.map(([k, x]) => `${k}: ${x}`).join('\n'))

/* The message of the day opens the first time this browser sees it, and
 * folds after that. */
const motdOpen = ref(false)
watch(() => [s.value.server, s.value.motd] as const, ([server, motd]) => {
  if (!server || !motd) return
  const key = `partyline.motd.${server}`
  let seen = ''
  try { seen = localStorage.getItem(key) ?? '' } catch { /* */ }
  motdOpen.value = seen !== motd
  try { localStorage.setItem(key, motd) } catch { /* */ }
}, { immediate: true })

/* ── The room tree ── */
const LS_TREE = 'partyline.tree'
const treePref = ref(localStorage.getItem(LS_TREE) !== '0')
const compactTree = ref(false)
const treeShown = computed({
  get: () => compact.value ? compactTree.value : treePref.value,
  set: (x: boolean) => {
    if (compact.value) compactTree.value = x
    else { treePref.value = x; localStorage.setItem(LS_TREE, x ? '1' : '0') }
  },
})

const pwRoom = ref<PlRoom | null>(null)
const pwText = ref('')
const pwEl = ref<HTMLInputElement | null>(null)

/* A move to the room we are in gets no answer from the server, so it is
 * never asked for. */
function onMove(r: PlRoom) {
  if (r.id === s.value.room || s.value.state !== 'live') return
  if (r.access & ACCESS_PASSWORD) {
    pwRoom.value = r
    pwText.value = ''
    nextTick(() => pwEl.value?.focus())
    return
  }
  pl.move(r.id)
  if (compact.value) compactTree.value = false
}
function submitPw() {
  const r = pwRoom.value
  if (!r) return
  pl.move(r.id, pwText.value)
  pwRoom.value = null
  pwText.value = ''
  if (compact.value) compactTree.value = false
}

/* ── Members ── */
const card = ref<PlMember | null>(null)
function openMember(mid: string) {
  const m = pl.members.value.find(x => x.mid === mid)
  if (m) card.value = m
}

const composerRef = ref<InstanceType<typeof Composer> | null>(null)

/* ── Talking ── */
const blocked = computed(() => {
  if (pl.textOnly.value) return 'Joined without audio'
  if (s.value.state !== 'live' || s.value.room === '') return 'Not in a room'
  if (!s.value.audio) return 'Audio is off'
  if (!s.value.canSpeak) return 'You may not speak in this room'
  if (s.value.txBlocked) return s.value.txBlocked
  if (pl.muted.value) return 'Muted'
  if (v.value.tx === 'vox' || v.value.tx === 'open') return `${txLabel(v.value.tx)}: no button needed`
  return ''
})

/* Space keys the transmitter while this window is in front, showing the
 * room, and no text field has the focus. */
function typing(t: EventTarget | null): boolean {
  const el = t as HTMLElement | null
  if (!el || !el.tagName) return false
  return el.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(el.tagName)
}
function onKeyDown(e: KeyboardEvent) {
  if (e.code !== 'Space' || typing(e.target)) return
  if (!props.visible || !liveShown.value || focusedWindowId.value !== 'partyline') return
  e.preventDefault()
  if (!e.repeat && !blocked.value) pl.setPtt(true)
}
function onKeyUp(e: KeyboardEvent) {
  if (e.code === 'Space' && pl.pttHeld.value) pl.setPtt(false)
}
onMounted(() => {
  document.addEventListener('keydown', onKeyDown)
  document.addEventListener('keyup', onKeyUp)
})
onBeforeUnmount(() => {
  document.removeEventListener('keydown', onKeyDown)
  document.removeEventListener('keyup', onKeyUp)
  pl.setPtt(false)
})

/* Never left keyed: leaving the room view, or the talk key becoming
 * unusable, lets go. */
watch(() => liveShown.value && props.visible && !blocked.value, (ok) => { if (!ok) pl.setPtt(false) })
watch(focusedWindowId, (id) => { if (id !== 'partyline') pl.setPtt(false) })
</script>

<style scoped>
.pl { position: relative; height: 100%; display: flex; flex-direction: column; background: #1d1d1d;
      overflow: hidden; color: #e8e8e8; }
.rfs-btn {
  display: inline-flex; align-items: center; justify-content: center;
  width: 18px; height: 18px; border-radius: 4px; font-size: 14px; font-weight: 700;
  color: rgba(255,255,255,0.5); cursor: pointer; font-family: system-ui; line-height: 1;
  user-select: none;
}
.rfs-btn:hover { color: rgba(255,255,255,0.9); background: rgba(255,255,255,0.1); }
.cols { flex: 1; display: flex; min-height: 0; }
.railcol { flex: none; border-right: 1px solid #2f2f2f; overflow: hidden; }
.center { flex: 1; min-width: 0; display: flex; flex-direction: column; background: #232323; }
.treecol { flex: none; width: calc(220px * var(--rfs, 1)); max-width: 34%; border-left: 1px solid #333; overflow: hidden; }

/* Compact: one column that slides — the rail, with the room or a server
 * sweeping in over it. */
.cols.mobile { position: relative; display: block; overflow: hidden; }
.cols.mobile .railcol, .cols.mobile .center {
  position: absolute; inset: 0; width: 100%; transition: transform 0.25s ease;
}
.cols.mobile .railcol { border-right: none; transform: translateX(0); }
.cols.mobile .center { transform: translateX(100%); z-index: 2; }
.cols.mobile.show-detail .railcol { transform: translateX(-25%); }
.cols.mobile.show-detail .center { transform: translateX(0); }

.topic { padding: 8px 14px; border-bottom: 1px solid #333; display: flex; gap: 8px; align-items: center;
         font-size: calc(13px * var(--rfs, 1)); min-width: 0; }
.back { background: none; border: none; color: #9a9a9a; font-size: 22px; cursor: pointer; padding: 0 4px; line-height: 1; }
.hdot { width: 8px; height: 8px; border-radius: 50%; flex: none; background: #555; }
.hdot.up { background: #4abf6a; }
.hdot.busy { background: #d6a12a; }
.hdot.down { background: #c0504d; }
.hd { color: #cfcfcf; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; min-width: 0; }
.hd b { color: #fff; font-size: calc(14px * var(--rfs, 1)); }
.sn { cursor: pointer; }
.sn:hover { text-decoration: underline; }
.btn.lv { flex: none; padding: 2px 10px; border-color: #7a2f2f; color: #f0a0a0; }
.btn.lv:hover { background: rgba(192, 80, 77, .15); }
.sp { flex: 1; }
.mem { display: inline-flex; align-items: center; gap: 3px; color: #9a9a9a; font-size: calc(12px * var(--rfs, 1));
       padding: 1px 6px; border-radius: 5px; white-space: nowrap; cursor: pointer; flex: none; }
.mem:hover { background: rgba(255,255,255,0.08); color: #e8e8e8; }
.mem.on { color: #e8e8e8; }
.stline { padding: 4px 14px; background: #2a2a2a; color: #d6a12a; font-size: calc(12px * var(--rfs, 1));
          border-bottom: 1px solid #333; cursor: pointer; }
.errline { padding: 4px 14px; color: #e09b6a; font-size: calc(12px * var(--rfs, 1)); border-bottom: 1px solid #333; }
.motd { border-bottom: 1px solid #333; background: #202020; }
.mh { display: flex; align-items: center; gap: 4px; padding: 3px 10px; color: #9a9a9a; cursor: pointer;
      font-size: calc(11.5px * var(--rfs, 1)); user-select: none; }
.mh:hover { color: #e8e8e8; }
.mt { white-space: pre-wrap; word-break: break-word; margin: 0 14px 8px; font-family: "DejaVu Sans Mono", Menlo, monospace;
      font-size: calc(12px * var(--rfs, 1)); color: #cfcfcf; max-height: 30vh; overflow-y: auto; }
.emptyst { flex: 1; overflow-y: auto; padding: 32px 28px; font-size: calc(13px * var(--rfs, 1)); }
.big { font-size: calc(17px * var(--rfs, 1)); font-weight: 700; margin-bottom: 6px; }
.dim { color: #9a9a9a; }
.small { margin-top: 18px; font-size: calc(12px * var(--rfs, 1)); }
.k { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; letter-spacing: .8px; text-transform: uppercase;
     margin: 22px 0 6px; }
.eh { display: flex; gap: 12px; align-items: baseline; padding: 6px 10px; border-radius: 6px; cursor: pointer;
      max-width: 420px; }
.eh:hover { background: #2a2a2a; }
.ehn { flex: 1; font-weight: 700; }
.ehn.anon { font-style: italic; font-weight: 400; }
.sheet-bg { position: absolute; inset: 0; z-index: 7; background: rgba(0,0,0,0.45);
            display: flex; justify-content: flex-end; }
.sheet { width: 78%; max-width: 300px; height: 100%; }
.scrim { position: absolute; inset: 0; z-index: 9; display: flex; align-items: flex-start; justify-content: center;
         padding: 70px 12px 0; background: rgba(0, 0, 0, .35); }
.pwcard { width: 300px; max-width: 100%; background: #2f2f2f; border: 1px solid #454545; border-radius: 10px;
          padding: 14px 16px; box-shadow: 0 24px 60px rgba(0, 0, 0, .6); font-size: calc(13px * var(--rfs, 1));
          display: flex; flex-direction: column; gap: 8px; }
.pwt { font-weight: 700; display: flex; align-items: center; gap: 6px; }
.in { background: #1c1c1c; color: #e8e8e8; border: 1px solid #3a3a3a; border-radius: 6px;
      padding: 6px 8px; font-size: calc(12.5px * var(--rfs, 1)); outline: none; font-family: inherit; }
.in:focus { border-color: #2c6bed; }
.pwb { display: flex; justify-content: flex-end; gap: 8px; }
.btn { background: none; border: 1px solid #555; color: #e8e8e8; border-radius: 6px; padding: 4px 12px;
       font-size: calc(12px * var(--rfs, 1)); cursor: pointer; }
.btn.p { background: #2c6bed; border-color: #2c6bed; color: #fff; font-weight: 700; }
</style>
