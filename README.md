# partyline — Partyline voice rooms on spangap

```
node                                                  partyline server (lxst.room)
  ◀─ ANNOUNCE lxst.room  app_data [1, name, flags(, desc(, lang(, country)))]
  ── Link + LINKIDENTIFY ─────────────────────────────▶
  ── {0x05 HELLO: {name, room, password, server_password,
                   text_only, muted, deaf, hops, rtt, ver: 1, app}} ─▶   once, the first packet
  ◀─ {0x06 WELCOME: {name, sid, motd, ver, app}}                         sid = our member id
  ◀─ {0x07 CHANNEL: [rec…]} … {0x08 USER: [rec…]} …                     sync, packets ≤ MDU − 8
  ◀─ {0x0E SYNCED: true}
  ◀─ {0x03 ROOM: [room_id, profile, frame_ms, can_speak]}
  ── {0x01 FRAMES: frame | [frame…], 0x10 SEQ: first} ───────────▶   while talking
  ── {0x13 TALK_END: true} ──────────────────────────────────────▶   after the last batch
  ◀─ {0x01 FRAMES, 0x02 SPEAKER: member_id, 0x10 SEQ}                    one copy per listener
  ◀─ {0x13 TALK_END: member_id}
  ◀─ {0x08 USER: rec}  a bare record is an update        {0x07 CHANNEL: [rec…]}
  ◀▶ TEXT 0x0C · STATE 0x0D · MOVE 0x0A · DENIED 0x0B · POKE 0x0F
     USER_LEFT 0x09 · NOTICE 0x12 · CONFIG 0x15 ["motd", text] · CHANNEL_GONE 0x16
     DIALIN 0x14 (ignored)
```

[Partyline](https://github.com/RFnexus/partyline) is a group voice app for
Reticulum in the manner of Mumble. One server holds rooms, each with a fixed
codec profile; a client holds one Link to the server and talks push-to-talk,
voice-activated or open-mic, and rooms also carry text, private pokes and
mute and deafen. Every packet is one msgpack map with integer keys, sent as a
plain Link packet, and a frame is a msgpack `bin` holding an LXST
(Lightweight Extensible Signal Transport) frame: a codec byte, then an Opus
packet or whole Codec2 sub-frames. The
server neither decodes nor mixes: it checks every frame, rate-limits each
member and forwards one unicast copy per listener, keyed by `SPEAKER`, and
each client mixes the talkers itself.

The node is the client. The partyline task holds the Link to one server,
speaks the room protocol (version 1, as partyline 1.0.5 speaks it) and keeps
the room list, the members and the text in storage. Audio passes through the
[lxst](../lxst) voice session, which owns the microphone, the speaker, the
codecs and the talk keys. The browser window, the LCD app and the CLI are
views of that state and writers of the commands it takes.

## Using it

**Servers.** Servers heard announcing `lxst.room` are listed, with their
description, language, country, hops and whether they want a server password
or keep an allow list; a hidden server announces no name and is shown as
unnamed. A server can also be added by its 32-hex destination hash. Wherever a
hash is typed, spaces are ignored and a full 64-hex hash is taken as its
leading 32. Connecting is an explicit act; one server can be set to connect at
boot. A lost Link is reconnected after 5 s, 15 s, 45 s, 2 min, then every
5 min (±20 %). A refusal that ends the session (wrong server password, ban,
allow list, server full, kicked, protocol version) is shown and not retried
until the user connects again.

**Who you are there.** The Link is identified, so the server and the room know
you by an identity, and an operator's rights, bans and allow lists key on it.
With one LXMF identity on the device it is used; with none, the node's own;
with several, the first connect to a server asks, and the answer is kept for
that server. The name others see is `s.partyline.nick`, or else the display
name of the LXMF identity in use. At boot the server set to connect waits,
for up to a minute, until lxmf has loaded the identities it holds, so the
choice is made among all of them.

**Rooms.** HELLO asks for the room last joined on that server (or the one a
connect names), else the server's default. Moving to a room with a password
asks for it once; it is kept under `secrets.partyline.rooms`, and forgotten when
the server answers "wrong password". The room list shows each room's profile,
member count, whether it is push-to-talk and whether it wants a password.

**Talking.** How this device keys its transmitter (talk key, voice gate or
open) is lxst's setting, `s.lxst.tx`. A room is push-to-talk when its
`ptt_jitter_ms` is above 0: speech is batched into few large packets and
plays at the listeners after that much delay, which is what makes a room
usable over LoRa. Only a member the server lets speak (`can_speak`) transmits,
and only while the server link's first hop (`rnsd.links.<tag>.bitrate`)
carries the room's stream: the full batches the room's push-to-talk rule
builds at the link's MDU, each one Link packet with Reticulum's 19-byte
header and 48-byte token around the 16-byte-padded payload, must come to at
most 60 % of the hop's bitrate, and over a LoRa first hop with an hourly
airtime budget (`lora.<n>.airtime_limit_ms`) its use
(`lora.<n>.airtime_hour_ms`) must stay 15 s short of the limit. That use is
republished by the interface at least every 10 s while it transmits and
the radio spends at most 1 s of airtime per second, so the 15 s reserve
keeps the hour's actual airtime within the limit. Otherwise the node does
not speak and says why in the room log and `partyline.tx_blocked` ("Route
too slow to talk in this room", "Airtime limit reached"), which the device's
talk bar and the browser's talk button show, and speaks again once both hold
— after an airtime block, only once 60 s of budget beyond the reserve is
free, so the block does not flap as each minute ages out. Listening is unaffected. At
SF7/125 kHz (4000 bit/s registered) a Codec2 700C push-to-talk room sends
1932 bit/s and fits; Codec2 3200 sends 5855 bit/s and does not.

**Text only.** With `s.partyline.text_only` on, the node joins without audio:
the server keeps it muted and deafened and sends it no voice, and the node
never sends a STATE, because the server does not enforce text-only and a
STATE would start audio flowing. The node also joins text-only, and says why,
when another room consumer already has the lxst session or lxst is absent; if
the session cannot be opened after joining with audio, the node sends STATE
muted and deafened and carries on with text.

**On a call.** A phone call (lxst's [telephony](../lxst/README.md#telephony))
rings over the room, which keeps playing. Once the call is answered it holds
the room: lxst says so (`EV_HELD`), `partyline.held` is 1, the status reads
"…, on a call", and the node sends STATE muted and deafened, so the server
stops sending audio and nobody hears the room from this member. When the call
ends the node sends STATE again with the user's own mute and deafen, and the
room plays as before; the room log notes both. A mute or deafen changed while
held is saved and sent at the resume. A member joined text-only is never sent
a STATE for a call. Joining while a call is live joins with audio, held from
the start.

## Room profiles

The server enforces these frame limits; a frame must carry the room's codec
byte, be 2 bytes up to the limit (codec byte included), and last exactly the
room's frame time, and the first frame that fails discards the whole packet.
For Opus the limit is min(400, 1 + ceil(bitrate / 8 × frame_ms / 1000)); for
Codec2 it is the server's overall cap of 400.

| Profile | Codec | Frame | Max frame bytes |
|---|---|---|---|
| `opus-high` | Opus voice, 16 kbit/s | 20 ms | 41 |
| `opus-med` | Opus voice, 8 kbit/s | 60 ms | 61 |
| `opus-low` | Opus voice, 6 kbit/s | 60 ms | 46 |
| `c2-3200` | Codec2 3200, 10 × 20 ms sub-frames | 200 ms | 400 |
| `c2-2400` | Codec2 2400, 10 × 20 ms sub-frames | 200 ms | 400 |
| `c2-1200` | Codec2 1200, 10 × 40 ms sub-frames | 400 ms | 400 |
| `c2-700` | Codec2 700C, 10 × 40 ms sub-frames | 400 ms | 400 |
| `music-low` | Opus audio, 14 kbit/s | 40 ms | 71 |
| `music-med` | Opus audio, 28 kbit/s | 40 ms | 141 |
| `music-high` | Opus audio, 56 kbit/s | 40 ms | 281 |

The node always encodes mono, which every profile accepts. A room whose
profile this client does not know stays joined for text, without audio.

`opus-high`, the server's default, sends 50 packets a second per talker and
belongs on WiFi or Ethernet. Over LoRa a room is a Codec2 push-to-talk room
with a handful of members: `c2-700` batched to the 431-byte MDU (maximum data
unit, the most one Link packet carries) costs about 1.1 kbit/s per talker,
`c2-3200` about 4.2 kbit/s, and the server multiplies that by the number of
listeners.

## Storage

`include/partyline.h` is the contract; one writer per field.

Persistent (`s.`, `secrets.`):

| Key | Meaning |
|---|---|
| `s.partyline.nick` | Name to join as; empty = the LXMF display name. |
| `s.partyline.text_only` | 1 = join without audio. |
| `s.partyline.muted`, `s.partyline.deaf` | What HELLO joins with, and what STATE last said. |
| `s.partyline.servers.<dest>.{name,ident,auto,room}` | A saved server. `ident` is the identity presented there (`secrets.lxmf.id.<n>.privkey` or `secrets.rnsd.identity`; empty = not chosen yet); `auto` 1 = connect at boot (one server at most); `room` the room name HELLO asks for, empty = the server's default. |
| `secrets.partyline.servers.<dest>.password` | Server password. |
| `secrets.partyline.rooms.<dest>.<hex of the lowercased room name>` | A room's password. |

RAM:

| Key | Meaning |
|---|---|
| `partyline.heard.<dest>.{name,flags,desc,lang,country,hops,seen}` | Servers heard announcing, the newest 64. `flags`: 1 hidden, 2 server password, 4 allow list. `seen` in monotonic seconds. |
| `partyline.servers.<dest>.saved_pw` | 1 when a server password is saved (frontends never read secrets). |
| `partyline.server` | The server held, empty = none. |
| `partyline.state` | `idle` · `connecting` · `joining` · `live` · `denied` · `lost` |
| `partyline.status` | That state as a finished sentence ("Connected to …, text only", "Connection lost; retrying in 15 s", "Refused: …"). |
| `partyline.{name,motd,sid}` | The server's name and message of the day, and our member id. |
| `partyline.room`, `partyline.can_speak` | The room we are in (empty = none), and 1 when we may transmit there. |
| `partyline.audio` | 1 while the lxst session is open for the room. |
| `partyline.held` | 1 while a live call holds the room's session. |
| `partyline.rooms.<id>.{name,profile,profile_text,frame_ms,access,desc,ptt_jitter_ms,flags,members,saved_pw}` | The room list. `profile_text` is finished ("Codec2 3200 · 200 ms · push-to-talk 2 s"); `access`: 1 identity, 2 allow list, 4 password; `flags`: 1 broadcast; `members` a count. |
| `partyline.members.<mid>.{name,identity,room,muted,deaf,hops,rtt,op,server_muted,text_only,can_speak,speaking,self}` | Every member on the server. `hops` −1 and `rtt` 0 when unknown; `self` 1 on our own record, whose `speaking` follows our transmitter. |
| `partyline.log.<n>.{ts,kind,mid,name,room,text}`, `partyline.log_next` | The last 100 lines; `n` counts from 0 each boot. `kind`: `text` (ours too), `poke` (received), `notice`, `system` (joins, leaves, moves, refusals, pokes sent, connection events). `ts` is unix seconds, or seconds since boot while the clock is unset. |
| `partyline.stats.{tx_drop,rx_drop}` | Packets dropped on a full Link port or lxst session port. |

Commands. The task reads the key, unsets it, acts, then answers on
`partyline.cmd.<verb>.error` (a sentence: refused) or
`partyline.cmd.<verb>.done` (a counter bumped: accepted).

| Key | Value |
|---|---|
| `partyline.cmd.add` | `<dest>` — save a server. |
| `partyline.cmd.forget` | `<dest>` — the server and its passwords. |
| `partyline.cmd.connect` | `{"server", "room"?, "password"?, "server_password"?, "ident"?}` — saves what it is given. Already connected there as that identity, a room is a move. |
| `partyline.cmd.disconnect` | Any non-empty value. |
| `partyline.cmd.auto` | `{"server", "auto": 0\|1}` |
| `partyline.cmd.move` | `{"room": <id> \| "<name>", "password"?}` — no password: the saved one. A move to the current room is not sent (the server never answers one). |
| `partyline.cmd.text` | `<text>`, to the current room (300 bytes at most). |
| `partyline.cmd.poke` | `{"mid", "text"}` |
| `partyline.cmd.state` | `{"muted"?: 0\|1, "deaf"?: 0\|1}` — saved, and sent unless joined text-only. |
| `partyline.cmd.nick` | `<name>`, used from the next join (48 bytes at most). |

## Frontends

**The LCD app** lists the server held, the saved servers and those heard, then
a field to add one by hash. A server's screen shows its description, access,
status, who to connect as, the room to ask for, the passwords and the message
of the day. The room screen has the members on the left (speakers lit,
operator, muted, deaf and text-only marks), the text on the right and a
composer below; its talk bar shows how to talk in this room ("Hold
Mic/Space" on the T-Deck), lights while transmitting with the microphone
level as a strip along its foot, and is itself a hold-to-talk button. Beside
it, Mute (a microphone, struck through while muted) and Deafen (a headset,
struck through while deafened) are icon toggles, amber-edged while on, and
Rooms opens the room list. While that screen is up
and no text field has focus, holding Space talks; the board's talk key always
does. From there, the room list moves between rooms, a member's card shows
their identity, hops and round trip and sends a poke, and the info page shows
the voice session's counters. Pokes and notices arriving while the app is
open also show as toasts. On a board with the tiny OLED, a status page shows
the room, the server, the member count and who is talking.

**The browser window** has the server rail, the room with its connection line,
message of the day, text and talk bar, and the room tree with its members. It
always uses the browser's own microphone and speaker, never the device's: a
connect, a move, a talk press or "Listen here" makes the browser the session's
endpoint over the DataChannel `lxst:2`, asking for the microphone on that
first gesture. Holding Space while no text field has focus, or holding the
talk button, keys the browser's microphone and nothing else. If the
microphone is refused or the channel is down, the window transmits nothing and
says so; a browser audio context the browser has suspended is shown as a
warning too, and releasing the talk button (the gesture touch devices allow)
resumes it. When a device talk key takes the audio back, the window stops
wanting the channel: it closes `lxst:2`, stops its microphone tracks, says
the audio is on the device and offers "Listen here"; only a gesture there
opens the channel again. A talk-key release that cannot be sent closes the
channel, which releases the key on the device, and opens it again. The talk
key is released whenever the tab is hidden or the window loses focus, and
closing the window hands the audio back to the device. On reconnecting to the
device the window replaces its rooms, members, heard servers and text log
with the device's, so records deleted meanwhile do not linger.

## CLI

```
partyline [status]                          server, room, profile, MDU, drops
partyline servers                           saved and heard
partyline connect <hash> [room] [lxmf<n>|node]
partyline disconnect
partyline rooms | members
partyline move <id|name> [password]
partyline say <text>
partyline poke <mid> <text>
partyline mute | unmute | deaf | undeaf
partyline log [n]
```

## Out of scope

Hosting a room on the node; operator actions (operators are marked, but the
node sends no ADMIN and no CONFIG); the server's dial-in bridge (DIALIN is
ignored) and LXST telephony, which reach a room from Sideband, MeshChat or
rnphone through the server rather than through this client. The node's own
phone is lxst's.

## Testing

`esp-idf/test/host/test_wire.cpp` checks the wire module against packets
captured from a real partyline 1.0.5 server and its reference clients: every
captured packet decodes, the named ones field by field, every frame is judged
as partyline's `valid_frame` judges it, and every client packet we have an
encoder for is re-encoded byte for byte.

```sh
cd esp-idf/test/host
g++ -std=c++17 -Wall -Wextra -I../../src -I../../../../lxst/esp-idf/include test_wire.cpp ../../src/partyline_wire.cpp ../../../../lxst/esp-idf/src/lxst_msgpack.cpp ../../../../lxst/esp-idf/src/lxst_frame.cpp -o /tmp/test_wire && /tmp/test_wire
```

It expects an lxst checkout beside this one. `gen_vectors.py` rebuilds
`vectors.h` from a capture (`packets.jsonl` and `meta.json`) under a Python
with partyline 1.0.5 installed.

Against a real server: install partyline (`pip install partyline`), run
`partyline-server` with a Reticulum config whose interfaces reach the device,
`partyline connect <its hash>` from the CLI, and talk to it from partyline's
own client.

See [INTERNALS.md](INTERNALS.md) for how the client works inside.
