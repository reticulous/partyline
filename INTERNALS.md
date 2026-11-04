# partyline — internals

The [README](README.md) covers what the client does and its storage contract.
This file covers how the task works and what must stay true when it changes.
`src/partyline.cpp` is the task; `src/partyline_wire.{h,cpp}` is the protocol.

## 1. The task

`PartylineService` registers with the RNS lifecycle, so the partyline task
(priority 1, secondary core, an 8 KB stack in PSRAM, pseudo-static RAM)
starts once rnsd is up and parks when it stops. Everything runs on that task: storage subscriptions
(`partyline.cmd.*`, `rnsd.links.pl*`) and ITS (inter-task streaming) callbacks
dispatch through its one `itsPoll`, which wakes at least every 500 ms for a
tick that checks deadlines (the WELCOME timeout, the reconnect backoff, the
voice session's reopen) and publishes the drop counters. It holds three kinds
of ITS connection: the announce feed from rnsd, the Link to the server, and
the lxst session. Every receive callback drains its port completely; a packet
larger than the buffer is dropped and the loop goes on to the next
(`recvNext`). The Link buffer is 8 KB; the lxst buffer is sized, per profile,
to the largest event lxst sends under it (a batch within the batch budget, or
one frame at the byte cap). The only waits are the `itsConnect` calls
themselves (the lxst session at most 1 s, the announce feed at most 2 s).

A command is read from storage, not from the change notification, whose value
is cut short, so a long command value arrives whole.

The CLI runs on the console task. It reads storage and writes command keys,
never the task's own state.

## 2. Discovery

The task subscribes to `lxst.room` on rnsd's announce port. The app_data is a
msgpack array of 3 to 6 elements, `[version, name, flags(, description(,
language(, country)))]`, trailing empty fields trimmed; any version other than
1 is skipped. The newest 64 servers are kept under `partyline.heard`.

## 3. Joining

```
connect ── rnsdLinkOpen(dest, "lxst.room", ident, tag pl<n>)
        ── rnsdLinkIdentify(tag, ident)       held by rnsd, sent at establishment
        ── rnsdLinkExpectProofs(tag, false)
rnsd.links.<tag>.state = active ── HELLO ──▶  deadline max(10 s, 15 × rtt)
◀── WELCOME (ver 1) ── CHANNEL … USER … SYNCED ── ROOM
```

Each attempt takes the next of eight Link tags `pl0` … `pl7`, so a late event
from a torn-down Link never matches the session in hand. rnsd holds the
identify and sends it at establishment, ahead of every packet, so the server
knows who we are before HELLO arrives. The server's destination proves
nothing, so the Link is told not to expect proofs, which also stops rnsd
proving the server's packets. HELLO goes once, as the first packet, when the
Link turns active: the server ignores a repeat and drops a Link that sends
none within 25 s. Its `hops` and `rtt` come from `rnsd.links.<tag>.hops` and
`.rtt_ms` (nil when unknown), `ver` is 1, and `app` is "reticulous" and the
build version.

A WELCOME that does not come within max(10 s, 15 × the round trip) tears the
session down and schedules a reconnect. A WELCOME whose `ver` is not 1 ends
the session as a refusal. Records are taken only after WELCOME, and the
session counts as live at SYNCED.

## 4. Reconnecting, and when not to

A Link that fails, closes or loses its ITS connection, and a WELCOME timeout,
schedule a reconnect after 5 s, 15 s, 45 s, 2 min, then every 5 min, each
±20 %; WELCOME resets the ladder. The server held stays in
`partyline.server` throughout. An established Link stays up indefinitely,
idle or not: keeping it alive is Reticulum's Link keepalive, run by rnsd, and
the task sends nothing of its own for it.

A DENIED whose room is nil is final: version, ban, allow list, server
password, kick or server full. Before WELCOME (server full comes even before
HELLO) and after it alike, the session ends in state `denied`, no reconnect is
scheduled, and the reason is shown until the user connects again. Retrying
would also trip the server's password backoff. "wrong server password" also
forgets the saved server password.

A DENIED `[room_id, reason]` is a room refusing us, during a join or a move.
It is logged; "wrong password" forgets that room's saved password; a pending
move reports it on `partyline.cmd.move.error`. After a refused join the server
puts us in its default room, or sends ROOM with no room.

## 5. Records

During sync, CHANNEL and USER each carry a list of records in packets of at
most MDU − 8. Afterwards a CHANNEL update is still a list, and a USER update
is one bare record; the decoder tells them apart by whether `value[0]` is
itself a list, and accepts a bare CHANNEL record the same way.

- CHANNEL record: `[id, name, profile, frame_ms, access_flags, description,
  dialin_number, ptt_jitter_ms, room_flags]`. `ptt_jitter_ms` is clamped to
  0 … 30000.
- USER record: `[member_id, name, identity_hex, room_id, muted, deaf, hops,
  rtt_ms, operator, server_muted, text_only, can_speak]`.

Displayed strings are cleaned as partyline's `clean_text` does it (control and
invisible characters removed, whitespace collapsed, cut on a UTF-8 character
boundary) with its fallbacks ("Room <id>", "guest-<id>", "server"). Joins,
leaves and moves of other members are logged only from USER updates after
SYNCED. Our own USER record carries `can_speak`, which is passed to the
session.

## 6. Audio

**The session.** On ROOM, unless joined text-only, the task opens the lxst
session with a profile built from the room: codec and mode from the profile
name, frame_ms from ROOM, the frame cap from the profile table, the batching
rule (§7), sequenced, and the receive depth `ptt_jitter_ms` with a 30 s cap in
a push-to-talk room (in other rooms both are left to lxst). Every ROOM that
changes the room sends `LXST_OP_PROFILE` even when the profile is the same,
which releases the old room's speakers; a CHANNEL update that changes the
current room's profile does too. `can_speak`, mute and deafen travel in
`LXST_OP_STATE`, never in a profile, because a profile change drops every
speaker's buffer. A ROOM with no room closes the session. If lxst drops the
session while the member is joined with audio in a room whose profile it
knows, the task opens it again with the room's profile, trying every 5 s
until it succeeds.

**Received audio.** A packet that holds nothing but FRAMES, SPEAKER and SEQ
takes a fast path: its frames are copied into an `LXST_OP_FRAMES` message
straight from the msgpack bytes, with no decode tree and no heap. Anything
else goes through the full decoder. TALK_END from a member is passed on as
`LXST_OP_TALK_END`; so is a member leaving the room or the server, so its
buffered audio plays out and is released. `LXST_EV_SPEAKING` sets the
member's `speaking`, and our own transmitter's sets ours.

**Held by a call.** lxst's call port shares the session: a live call holds
the room and lxst sends `LXST_EV_HELD 1`, then `0` when the call ends. Held,
the room's frames are dropped by lxst and nothing is transmitted for it, so
the task, joined with audio, sends STATE muted and deafened to stop the
server sending audio nobody plays, and at the resume sends STATE with
`s.partyline.muted` and `s.partyline.deaf`. A STATE command while held is
saved and passed to lxst (which keeps it as the room's) but not sent to the
server until the resume. Joined text-only, nothing is sent. `partyline.held`
mirrors the hold, the status line adds "on a call", and the room log notes
both edges. The session closing (or lxst dropping it) clears the hold. A
call that is live when the room's session opens holds it from the start;
`lxst.state` is then `call`, which does not make the join text-only.

**Text only.** HELLO says `text_only` when the setting is on, or when lxst is
absent or another room consumer has its session (`lxst.state` `live` and not
ours). Joined that way, the task never opens the session and never sends
STATE, since the server enforces neither and a STATE that unmutes would start
audio. If the session fails to open after a join with audio, the task sends
STATE muted and deafened once, so the server stops sending frames nobody
plays, and carries on as text-only.

## 7. Transmit: batching and SEQ

The task does not encode; lxst hands it batches. The batching rule is
partyline's own client's, and the server forwards batches as they come, so
the sender's batching is the only batching on the path:

- before a frame joins a non-empty batch, flush if `8 + Σ(len + 3)` would pass
  MDU − 16, `len` counting the codec byte;
- after it joins, flush at `min(64, max(1, ptt_jitter_ms / frame_ms))` frames
  in a push-to-talk room, and at every frame in any other room;
- at the end of a spurt, flush the partial batch, then send TALK_END.

The rule counts frames and bytes, not time. The MDU (maximum data unit) is
read from `rnsd.links.<tag>.mdu` when the room's session is set up; when the
key is missing the task takes 431, the MDU at Reticulum's default MTU (maximum transmission unit) of 500
bytes.

`SEQ` is the 16-bit number of the first frame in the packet. The counter
starts at 1 for each session and each profile change and advances by the
number of frames sent. One frame goes as a bare `bin`, more as a list. The
server forwards SEQ untouched but re-bases it when it splits a batch for a
listener with a smaller MDU, so the receiver numbers frame *i* of a packet
SEQ + *i* and never assumes packets match the sender's batches.

## 8. Identity

The identity presented is the one a connect names, else the one saved for the
server, else the device's only LXMF identity (group identities do not count),
else the node's own when there is no LXMF identity. With several and none
chosen, the connect is refused with a request to choose, and the frontends
offer the choice. The choice is saved per server, because the server, its
operators and its allow lists know the member by it.

At boot, lxmf publishes its identities some time after their keys are
readable. For the first minute after the task starts, while an LXMF identity
has a stored key but no published destination, the boot connect waits and
looks again every 2 s, so "the only LXMF identity" is not decided
before all of them are known.

## 9. The wire module

`partyline_wire.{h,cpp}` encodes and decodes every message on top of lxst's
msgpack, with partyline's string limits (name 48 bytes, text 300,
description 120). It has no platform includes, so the host test builds it as
is. `decodePacket` never fails a whole packet for one bad key: a malformed key
sets its bit in `malformed` and the others still decode. The encoders write
exactly the bytes partyline 1.0.5's client writes, key order and nil
placeholders included; the host test proves it by re-encoding the captured
client packets.

The protocol version is the one constant `PROTOCOL_VERSION`; the server
rejects any other.

## 10. Rules worth keeping

- File-local types live in anonymous namespaces. A global `struct Session`
  here and one of the same name in another straddle are one type to the
  linker under the one-definition rule, and with different layouts one
  corrupts the other and the image crashes.
- Drain every ITS port on every receive callback; one notification can stand
  for many packets. An oversize packet is skipped, never the end of the loop.
- Read a command's value from storage, not from the notification.
- Never send a MOVE to the current room: the server never answers it.
- Never send STATE when joined text-only.
- Room changes always send `LXST_OP_PROFILE`; state changes never do.
