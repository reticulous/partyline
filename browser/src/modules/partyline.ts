/**
 * partyline — the Partyline voice-room window (SPA side).
 *
 * The node is the Partyline client; this window is a view over its storage
 * and a writer of its command keys (the contract is partyline.h):
 *   reads  = computed() over the reactive device mirror
 *   writes = self-clearing partyline.cmd.* keys
 *
 * Audio is always this browser's, never the device's microphone or speaker:
 * a connect, a move, a talk press or "Listen here" makes the browser the
 * voice session's endpoint over the DataChannel "lxst:2" (lxst's
 * lib/lxstAudio.ts, one for the page, shared with the Phone window), and the
 * talk key travels on that channel.
 *
 *   s.partyline.servers.<dest>.{name,ident,auto,room}   saved servers
 *   partyline.heard.<dest>.{name,flags,desc,lang,country,hops,seen}
 *   partyline.{server,state,status,name,motd,sid,room,can_speak,audio,tx_blocked}
 *   partyline.rooms.<id>.*  partyline.members.<mid>.*  partyline.log.<n>.*
 *   lxst.{state,talking,profile,stats.*}  s.lxst.tx
 *
 * This is the only module that touches useDeviceStore; the components take
 * what it computes and call what it returns.
 */
import { ref, computed, watch, effectScope, type ComputedRef } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { registerApp } from 'spangap-browser/lib/apps'
import { registerWindowMount } from 'spangap-browser/lib/windowMounts'
import PartylineWindow from '../panels/PartylineWindow.vue'
import {
  audioActive, audioOwner, audioOnDevice, channelState, ctxState, micState, micLevel, talkKeyHeldBy,
  startAudio as startSharedAudio, resumeAudio, stopAudio as stopSharedAudio, setTalkKey as setSharedTalkKey,
} from 'lxst/lib/lxstAudio'

/* This window's name on the page's one audio surface. */
const AUDIO_USER = 'partyline'
const startAudio = () => startSharedAudio(AUDIO_USER)
const stopAudio = () => stopSharedAudio(AUDIO_USER)
const setTalkKey = (down: boolean) => setSharedTalkKey(down, AUDIO_USER)
const pttHeld = talkKeyHeldBy(AUDIO_USER)

/* Visibility ref for the Partyline floating window. */
export const partylineVisible = ref(false)

/** Focus nonce — bumped to raise the window even when already open. */
export const partylineFocus = ref(0)

/* Dock action: only ever show + raise, never hide. */
export function showPartyline() { partylineVisible.value = true; partylineFocus.value++ }

export type LinkState = 'idle' | 'connecting' | 'joining' | 'live' | 'denied' | 'lost' | ''

/* Announce flags (partyline.heard.<dest>.flags). */
export const ANNOUNCE_HIDDEN = 1
export const ANNOUNCE_PASSWORD = 2
export const ANNOUNCE_ALLOWLIST = 4

/* Room access flags (partyline.rooms.<id>.access) and room flags. */
export const ACCESS_IDENTITY = 1
export const ACCESS_ALLOWLIST = 2
export const ACCESS_PASSWORD = 4
export const ROOM_BROADCAST = 1

/* Partyline's own text limit, in UTF-8 bytes. */
export const TEXT_LIMIT = 300

export interface PlHeard {
  dest: string
  /** '' for a hidden server. */
  name: string
  flags: number
  desc: string
  lang: string
  country: string
  hops: number
  /** Device monotonic seconds, and the unix time that is (0 = clock not set). */
  mono: number
  lastSeen: number
}

export interface PlServer {
  dest: string
  /** In s.partyline.servers — a server the node keeps. */
  saved: boolean
  /** Saved name, else the WELCOME name when it is the server we hold, else
   *  the announced one; '' = none. */
  name: string
  /** Storage path of the identity presented there, '' = ask. */
  ident: string
  auto: boolean
  /** Room asked for in HELLO, '' = the server's default. */
  room: string
  heard: PlHeard | null
}

export interface PlRoom {
  id: string
  name: string
  profile: string
  profileText: string
  frameMs: number
  access: number
  desc: string
  pttJitterMs: number
  flags: number
  members: number
}

export interface PlMember {
  mid: string
  name: string
  identity: string
  room: string
  muted: boolean
  deaf: boolean
  hops: number
  rtt: number
  op: boolean
  serverMuted: boolean
  textOnly: boolean
  canSpeak: boolean
  speaking: boolean
  self: boolean
}

export type LogKind = 'text' | 'poke' | 'notice' | 'system'

export interface PlLine {
  /** The line's n in partyline.log.<n>: oldest first; unique. */
  order: number
  ts: number
  kind: LogKind
  mid: string
  name: string
  room: string
  text: string
}

export interface PlSession {
  /** <dest> of the server held, '' = none. */
  server: string
  state: LinkState
  status: string
  name: string
  motd: string
  sid: string
  /** The room we are in, '' = none. */
  room: string
  canSpeak: boolean
  audio: boolean
  /** partyline.tx_blocked: why our audio may not go out on the first hop, '' = it may. */
  txBlocked: string
}

export interface PlVoice {
  state: string
  talking: boolean
  profile: string
  /** s.lxst.tx: ptt (push-to-talk) | vox (voice-operated) | open. */
  tx: string
  stats: [string, string][]
}

export interface PlAudio {
  /** This browser is the audio surface. */
  active: boolean
  /** One line on where the audio is, '' = nothing to say. */
  text: string
  /** The line is a fault: nothing is transmitted from here. */
  warn: boolean
  /** Offer "Listen here": joined with audio not here, or here but paused. */
  offerListen: boolean
  /** Microphone level 0…1 while sending. */
  level: number
}

/** One identity the node can present. `sel` is the storage path the
 *  firmware takes. */
export interface PlIdentity { sel: string; label: string; hash: string }

export interface ConnectArgs {
  server: string
  room?: string
  password?: string
  server_password?: string
  ident?: string
}

type Patch = Record<string, any>

function nest(path: string, val: any): Patch {
  const parts = path.split('.')
  const root: Patch = {}
  let cur = root
  for (let i = 0; i < parts.length - 1; i++) cur = (cur[parts[i]!] = {})
  cur[parts[parts.length - 1]!] = val
  return root
}

function num(v: unknown, d = 0): number { const n = Number(v); return Number.isFinite(n) ? n : d }
function str(v: unknown): string { return v == null ? '' : String(v) }
const on = (v: unknown) => num(v) === 1

const HASH_RE = /^[0-9a-f]{32}$/
const isHash = (k: string) => HASH_RE.test(k)

/* Storage subtrees whose keys are numbers may arrive as arrays; walk both. */
function entries(tree: unknown): [string, any][] {
  if (!tree || typeof tree !== 'object') return []
  return Object.entries(tree as Record<string, any>).filter(([, v]) => v && typeof v === 'object')
}

const KINDS: LogKind[] = ['text', 'poke', 'notice', 'system']
function kindOf(v: unknown): LogKind {
  const s = str(v)
  if ((KINDS as string[]).includes(s)) return s as LogKind
  return KINDS[num(v)] ?? 'system'
}

export interface UsePartyline {
  session: ComputedRef<PlSession>
  voice: ComputedRef<PlVoice>
  /** Saved servers, then the held one if it is not saved, by name. */
  servers: ComputedRef<PlServer[]>
  /** Heard announcing and not saved, newest first. */
  heard: ComputedRef<PlHeard[]>
  /** A server by hash — saved, held or heard; null for a non-hash. */
  serverOf: (dest: string) => PlServer | null
  rooms: ComputedRef<PlRoom[]>
  members: ComputedRef<PlMember[]>
  self: ComputedRef<PlMember | null>
  log: ComputedRef<PlLine[]>
  identities: ComputedRef<PlIdentity[]>
  /** What the node joins with: our member record when in a room, else the
   *  saved choice. */
  muted: ComputedRef<boolean>
  deaf: ComputedRef<boolean>
  textOnly: ComputedRef<boolean>
  nick: ComputedRef<string>
  /** The last rejection the task wrote for a command, '' = none. */
  cmdError: (name: string) => string
  /** The talk key as this window holds it. */
  pttHeld: ComputedRef<boolean>
  /** Audio in this browser, for the talk bar. */
  audio: ComputedRef<PlAudio>

  add: (dest: string) => void
  forget: (dest: string) => void
  connect: (a: ConnectArgs) => void
  disconnect: () => void
  move: (room: string, password?: string) => void
  text: (text: string) => void
  poke: (mid: string, text: string) => void
  setMuted: (v: boolean) => void
  setDeaf: (v: boolean) => void
  setNick: (name: string) => void
  setAuto: (dest: string, v: boolean) => void
  /** The talk key; a press starts audio here and takes it from the device.
   *  Call from the gesture. */
  setPtt: (v: boolean) => void
  /** Play and capture audio here, taking it from the device if it has it.
   *  Call from the gesture. */
  listen: () => void
  /** Resume audio output here, if audio is here. Call from a gesture. */
  resume: () => void
}

let _inst: UsePartyline | null = null

/** The Partyline state layer. One instance for the page, in its own effect
 *  scope so no component's unmount stops it. */
export function usePartyline(): UsePartyline {
  if (_inst) return _inst
  _inst = effectScope(true).run(build)!
  return _inst
}

function build(): UsePartyline {
  const device = useDeviceStore()
  /* Records the node deletes: a reconnect's dump replaces these, so what was
   * deleted while away does not linger. */
  for (const t of ['partyline.rooms', 'partyline.members', 'partyline.heard', 'partyline.log'])
    device.snapshotTree(t)

  const session = computed<PlSession>(() => ({
    server: str(device.get('partyline.server')).toLowerCase(),
    state: str(device.get('partyline.state')) as LinkState,
    status: str(device.get('partyline.status')),
    name: str(device.get('partyline.name')),
    motd: str(device.get('partyline.motd')),
    sid: str(device.get('partyline.sid')),
    room: str(device.get('partyline.room')),
    canSpeak: on(device.get('partyline.can_speak')),
    audio: on(device.get('partyline.audio')),
    txBlocked: str(device.get('partyline.tx_blocked')),
  }))

  const voice = computed<PlVoice>(() => {
    const stats = device.get('lxst.stats')
    const list: [string, string][] = []
    if (stats && typeof stats === 'object')
      for (const [k, v] of Object.entries(stats))
        if (v != null && typeof v !== 'object') list.push([k, str(v)])
    return {
      state: str(device.get('lxst.state')),
      talking: on(device.get('lxst.talking')),
      profile: str(device.get('lxst.profile')),
      tx: str(device.get('s.lxst.tx')) || 'ptt',
      stats: list,
    }
  })

  const heardMap = computed(() => {
    const out = new Map<string, PlHeard>()
    /* seen is device monotonic seconds; sys.boot_time is the wall-clock
     * instant of boot, so boot + seen is the unix time heard. */
    const boot = num(device.get('sys.boot_time'))
    for (const [k, h] of entries(device.get('partyline.heard'))) {
      const dest = k.toLowerCase()
      if (!isHash(dest)) continue
      const mono = num(h.seen)
      out.set(dest, {
        dest, mono,
        name: str(h.name), flags: num(h.flags), desc: str(h.desc),
        lang: str(h.lang), country: str(h.country), hops: num(h.hops, -1),
        lastSeen: boot > 0 && mono > 0 ? boot + mono : 0,
      })
    }
    return out
  })

  function parseServer(dest: string, s: any): PlServer {
    const heard = heardMap.value.get(dest) ?? null
    const held = session.value.server === dest ? session.value.name : ''
    return {
      dest,
      saved: s != null,
      name: str(s?.name) || held || heard?.name || '',
      ident: str(s?.ident),
      auto: on(s?.auto),
      room: str(s?.room),
      heard,
    }
  }

  const servers = computed<PlServer[]>(() => {
    const saved = device.get('s.partyline.servers') ?? {}
    const keys = new Set(Object.keys(saved).filter(isHash))
    if (isHash(session.value.server)) keys.add(session.value.server)
    return [...keys].map(d => parseServer(d, saved[d]))
      .sort((a, b) => (a.name || '~' + a.dest).localeCompare(b.name || '~' + b.dest))
  })

  const heard = computed<PlHeard[]>(() => {
    const saved = device.get('s.partyline.servers') ?? {}
    return [...heardMap.value.values()]
      .filter(h => saved[h.dest] == null && h.dest !== session.value.server)
      .sort((a, b) => b.mono - a.mono)
  })

  const serverOf = (dest: string): PlServer | null => {
    if (!isHash(dest)) return null
    return parseServer(dest, (device.get('s.partyline.servers') ?? {})[dest])
  }

  const rooms = computed<PlRoom[]>(() =>
    entries(device.get('partyline.rooms')).map(([id, r]) => ({
      id,
      name: str(r.name),
      profile: str(r.profile),
      profileText: str(r.profile_text),
      frameMs: num(r.frame_ms),
      access: num(r.access),
      desc: str(r.desc),
      pttJitterMs: num(r.ptt_jitter_ms),
      flags: num(r.flags),
      members: num(r.members),
    })).sort((a, b) => num(a.id) - num(b.id)))

  const members = computed<PlMember[]>(() =>
    entries(device.get('partyline.members')).map(([mid, m]) => ({
      mid,
      name: str(m.name),
      identity: str(m.identity).toLowerCase(),
      room: str(m.room),
      muted: on(m.muted),
      deaf: on(m.deaf),
      hops: num(m.hops, -1),
      rtt: num(m.rtt),
      op: on(m.op),
      serverMuted: on(m.server_muted),
      textOnly: on(m.text_only),
      canSpeak: on(m.can_speak),
      speaking: on(m.speaking),
      self: on(m.self) || (mid === session.value.sid && mid !== ''),
    })).sort((a, b) =>
      (a.name || '~' + a.mid).localeCompare(b.name || '~' + b.mid, undefined, { sensitivity: 'base' })))

  const self = computed(() => members.value.find(m => m.self) ?? null)

  /* The text log, oldest first: partyline.log.<n> holds the last 100 lines,
   * n counting up from 0 each boot. */
  const log = computed<PlLine[]>(() => {
    const boot = num(device.get('sys.boot_time'))
    const out: PlLine[] = []
    for (const [k, r] of entries(device.get('partyline.log'))) {
      let ts = num(r.ts)
      /* A stamp before 2001 is device monotonic seconds. */
      if (ts > 0 && ts < 1e9) ts = boot > 0 ? boot + ts : 0
      out.push({
        order: num(k), ts, kind: kindOf(r.kind), mid: str(r.mid), name: str(r.name),
        room: str(r.room), text: str(r.text),
      })
    }
    return out.sort((a, b) => a.order - b.order)
  })

  /* The LXMF identities this device holds, then the node's own, as rrc lists
   * them; the selector is the identity's private-key storage path. */
  const identities = computed<PlIdentity[]>(() => {
    const out: PlIdentity[] = []
    const ids = device.get('lxmf.id') ?? {}
    for (const [slot, v] of Object.entries<any>(ids)) {
      const hash = str(v?.dest_hash)
      if (!HASH_RE.test(hash.toLowerCase())) continue
      if (str(device.get(`s.lxmf.id.${slot}.kind`)) === 'group') continue
      const name = str(device.get(`s.lxmf.id.${slot}.display_name`))
      out.push({ sel: `secrets.lxmf.id.${slot}.privkey`, label: name || `LXMF identity ${slot}`, hash })
    }
    out.sort((a, b) => a.sel.localeCompare(b.sel, undefined, { numeric: true }))
    out.push({ sel: 'secrets.rnsd.identity', label: 'This node',
               hash: str(device.get('rnsd.identity_hash')) })
    return out
  })

  const inRoom = computed(() => self.value != null && session.value.state === 'live')
  const muted = computed(() => inRoom.value ? self.value!.muted : on(device.get('s.partyline.muted')))
  const deaf = computed(() => inRoom.value ? self.value!.deaf : on(device.get('s.partyline.deaf')))
  const textOnly = computed(() => inRoom.value ? self.value!.textOnly : on(device.get('s.partyline.text_only')))
  const nick = computed(() => str(device.get('s.partyline.nick')))

  const cmdError = (name: string) => {
    const v = device.get(`partyline.cmd.${name}`)
    return v && typeof v === 'object' ? str(v.error) : ''
  }

  /* ── Commands. Each is one self-clearing key; the task unsets it and acts. ── */
  const cmd = (name: string, val: string) => device.sendJson(nest(`partyline.cmd.${name}`, val))
  const cmdObj = (name: string, val: Record<string, unknown>) => cmd(name, JSON.stringify(val))
  const destArg = (d: string) => { const x = d.trim().toLowerCase(); return isHash(x) ? x : '' }

  const add = (dest: string) => { const d = destArg(dest); if (d) cmd('add', d) }
  const forget = (dest: string) => { const d = destArg(dest); if (d) cmd('forget', d) }
  const connect = (a: ConnectArgs) => {
    const server = destArg(a.server)
    if (!server) return
    const o: Record<string, unknown> = { server }
    if (a.room) o.room = a.room
    if (a.password) o.password = a.password
    if (a.server_password) o.server_password = a.server_password
    if (a.ident) o.ident = a.ident
    cmdObj('connect', o)
    wantAudio()
  }
  const disconnect = () => cmd('disconnect', '1')
  /* A move to the room we are in gets no reply from the server: never sent. */
  const move = (room: string, password = '') => {
    if (room === '' || room === session.value.room) return
    const o: Record<string, unknown> = { room: num(room) }
    if (password) o.password = password
    cmdObj('move', o)
    wantAudio()
  }
  const text = (t: string) => { if (t.trim() && session.value.room !== '') cmd('text', t) }
  const poke = (mid: string, t: string) => { if (mid !== '') cmdObj('poke', { mid: num(mid), text: t }) }
  const setMuted = (v: boolean) => cmdObj('state', { muted: v ? 1 : 0 })
  const setDeaf = (v: boolean) => cmdObj('state', { deaf: v ? 1 : 0 })
  const setNick = (name: string) => cmd('nick', name.trim())
  /* Autoconnect at boot; the task keeps it on at most one server. */
  const setAuto = (dest: string, v: boolean) => {
    const d = destArg(dest)
    if (d) cmdObj('auto', { server: d, auto: v ? 1 : 0 })
  }

  /* The talk key, on the audio channel; a press also starts audio here. */
  const setPtt = (v: boolean) => {
    if (pttHeld.value === v) return
    if (v && textOnly.value) return
    setTalkKey(v)
  }
  /* Never left keyed: losing the room, the right to speak or the voice
   * session lets go. */
  watch(() => session.value.state === 'live' && session.value.canSpeak && session.value.audio,
        (ok) => { if (!ok) setPtt(false) })

  /* Audio here ends with the link: disconnected or refused. A lost link
   * reconnects by itself and keeps it. */
  watch(() => session.value.state, (st) => {
    if (st === 'idle' || st === 'denied' || st === '') stopAudio()
  })

  /* Gestures that join a room start audio here, unless joining as text only. */
  const wantAudio = () => { if (!textOnly.value) startAudio() }
  const listen = () => { if (!textOnly.value) startAudio() }
  const resume = () => resumeAudio()

  const audio = computed<PlAudio>(() => {
    const joined = session.value.state === 'live' && session.value.room !== '' && !textOnly.value
    const base = { active: audioActive.value, level: micLevel.value, offerListen: false, warn: false }
    if (!audioActive.value)
      return { ...base, text: joined ? 'Audio is not playing in this browser' : '', offerListen: joined }
    if (audioOnDevice.value || audioOwner.value === false)
      return { ...base, text: 'Audio is on the device', offerListen: true }
    if (channelState.value === 'connecting') return { ...base, text: 'Connecting audio…' }
    if (channelState.value !== 'open') return { ...base, text: 'Audio channel down', warn: true }
    if (ctxState.value !== 'running')
      return { ...base, text: 'Browser audio is paused — not playing or transmitting', warn: true, offerListen: true }
    switch (micState.value) {
      case 'denied': return { ...base, text: 'Microphone blocked — not transmitting', warn: true }
      case 'none': return { ...base, text: 'No microphone — not transmitting', warn: true }
      case 'error': return { ...base, text: 'Microphone unavailable — not transmitting', warn: true }
      case 'asking': return { ...base, text: 'Waiting for microphone permission…' }
      default: return { ...base, text: 'Audio here (browser)' }
    }
  })

  return {
    session, voice, servers, heard, serverOf, rooms, members, self, log, identities,
    muted, deaf, textOnly, nick, cmdError, pttHeld, audio,
    add, forget, connect, disconnect, move, text, poke, setMuted, setDeaf, setNick, setAuto, setPtt, listen, resume,
  }
}

export function registerPartyline() {
  /* The talk key is released whenever the page stops being in front of the
   * person holding it: the tab hidden, the browser window blurred, the page
   * going away. */
  const release = () => { if (_inst) _inst.setPtt(false) }
  window.addEventListener('blur', release)
  window.addEventListener('pagehide', release)
  document.addEventListener('visibilitychange', () => { if (document.visibilityState !== 'visible') release() })
  /* Closing the window hands the audio back to the device. */
  watch(partylineVisible, (v) => { if (!v) { release(); stopAudio() } })

  /* Dock app — foregrounds the Partyline window. */
  registerApp({ id: 'partyline', label: 'Partyline', icon: 'partyline', placement: 7,
                open: showPartyline, isOpen: () => partylineVisible.value })

  registerWindowMount({ id: 'partyline', title: 'Partyline', component: PartylineWindow,
                        visible: partylineVisible, focusToken: partylineFocus })
}
