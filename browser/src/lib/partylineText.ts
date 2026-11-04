/**
 * Text helpers for the Partyline window: hashes, byte counts and times. Pure
 * functions, no store.
 */

/** A server destination typed or pasted: 32 hex characters, spaces and
 *  angle brackets ignored. '' when it isn't one. */
export function parseDest(raw: string): string {
  const s = raw.replace(/[\s<>:]/g, '').toLowerCase()
  return /^[0-9a-f]{32}$/.test(s) ? s : ''
}

/** "0c4d11fe93aa…" → "0c4d 11fe 93aa …" */
export function groupHex(hex: string): string {
  return (hex.match(/.{1,4}/g) ?? []).join(' ')
}

/** How a server is named in a list: its name, or "unnamed server" with the
 *  start of its hash (a hidden server announces an empty name). */
export function serverLabel(name: string, dest: string): string {
  return name || `unnamed server ${dest.slice(0, 6)}`
}

export function utf8Bytes(s: string): number {
  return new TextEncoder().encode(s).length
}

export function hhmm(ts: number): string {
  const d = new Date(ts * 1000)
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
}

/** Age of a unix-seconds instant, short ("40 s", "5 min", "3 h", "2 d"). */
export function ago(unixS: number): string {
  if (!unixS) return ''
  const s = Math.max(0, Math.round(Date.now() / 1000 - unixS))
  if (s < 60) return `${s} s`
  if (s < 3600) return `${Math.floor(s / 60)} min`
  if (s < 86400) return `${Math.floor(s / 3600)} h`
  return `${Math.floor(s / 86400)} d`
}

/** s.lxst.tx in words. */
export function txLabel(tx: string): string {
  switch (tx) {
    case 'vox': return 'Voice activated'
    case 'open': return 'Open mic'
    default: return 'Push to talk'
  }
}

/** A member's colour, from their identity (or member id when it is unknown). */
export function memberHue(key: string): number {
  let h = 0
  for (let i = 0; i < Math.min(key.length, 8); i++) h = (h * 31 + key.charCodeAt(i)) >>> 0
  return h % 360
}

export function nickColour(hue: number): string {
  return `hsl(${hue} 60% 68%)`
}
