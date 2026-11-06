/**
 * partyline — Partyline voice-room client task.
 *
 * The node is the client: this task holds one Link to one partyline server
 * (`lxst.room`), speaks the room protocol and keeps room and member state in
 * storage. Audio passes through the lxst voice session. Every frontend (the
 * browser window, the LCD app, the CLI) is a view over storage — storage is the
 * API, one writer per field, commands are self-clearing keys answered on
 * <cmd>.error / <cmd>.done. Zero µReticulum includes: everything rides rnsd.h.
 *
 * Persistent:
 *   s.partyline.nick                       name we join as ("" = the LXMF display name)
 *   s.partyline.text_only                  1 = join without audio
 *   s.partyline.muted / s.partyline.deaf   what we join with, and what STATE last said
 *   s.partyline.servers.<dest>.{name,ident,auto,room}
 *                                          saved servers: <dest> is the 32-hex room
 *                                          destination; ident the storage path of the
 *                                          identity we present there ("" = asked);
 *                                          auto 1 = connect at boot; room the room name
 *                                          to ask for in HELLO ("" = server default),
 *                                          set by connect and by a move that succeeds;
 *                                          ident is secrets.lxmf.id.<n>.privkey or
 *                                          secrets.rnsd.identity; the task is the only
 *                                          writer of this subtree
 *   secrets.partyline.servers.<dest>.password        server password
 *   secrets.partyline.rooms.<dest>.<hex of the lowercased room name>   room passwords
 * RAM:
 *   partyline.heard.<dest>.{name,flags,desc,lang,country,hops,seen}
 *                                          servers heard announcing (the newest 64);
 *                                          flags is the announce bitmask, seen
 *                                          monotonic seconds; name "" = unnamed (hidden)
 *   partyline.servers.<dest>.saved_pw      1 when a server password is saved (UIs never
 *                                          read secrets)
 *   partyline.server                       <dest> of the server we hold, "" = none
 *   partyline.state                        idle | connecting | joining | live | denied | lost
 *   partyline.status                       finished text for that state, for a UI line
 *   partyline.name / partyline.motd        server name and message of the day (WELCOME,
 *                                          CONFIG)
 *   partyline.sid                          our member id
 *   partyline.room                         the id of the room we are in ("" = none)
 *   partyline.rejoined                     the room's name once a retry after a lost Link
 *                                          lands in it; "" from the loss, a connect or a
 *                                          disconnect on
 *   partyline.can_speak                    1 when this member may transmit here
 *   partyline.audio                        1 when a voice session is open for the room
 *   partyline.rooms.<id>.{name,profile,profile_text,frame_ms,access,desc,ptt_jitter_ms,
 *                         flags,members,saved_pw}
 *                                          the room list (CHANNEL); members a count,
 *                                          profile_text finished ("Codec2 3200 · 200 ms ·
 *                                          push-to-talk 2 s"), saved_pw 1 when a password
 *                                          is saved for the room
 *   partyline.members.<mid>.{name,identity,room,muted,deaf,hops,rtt,op,server_muted,
 *                           text_only,can_speak,speaking,self}
 *                                          room an id ("" = none), hops -1 and rtt 0 when
 *                                          unknown; self 1 on our own record, whose
 *                                          speaking follows our transmitter
 *   partyline.log.<n>.{ts,kind,mid,name,room,text}
 *                                          the last 100 lines; n counts up from 0 each
 *                                          boot and partyline.log_next is the next n. ts
 *                                          is unix seconds, or seconds since boot while
 *                                          the clock is unset. kind: text (our own lines
 *                                          too, mid = our sid), poke (received), notice,
 *                                          system (joins, leaves, moves, refusals, pokes
 *                                          we sent, connection events)
 *   partyline.stats.{tx_drop,rx_drop}      packets dropped on a full link / session port
 * Commands. A command is one key holding a plain string or a JSON object as a
 * string. The task reads it, deletes it, acts, then answers on
 * partyline.cmd.<verb>.error (a sentence: refused) or partyline.cmd.<verb>.done
 * (a per-boot counter bumped: accepted).
 *   partyline.cmd.add         "<dest>"                        save a server
 *   partyline.cmd.forget      "<dest>"                        with its passwords
 *   partyline.cmd.connect     {"server","room"?,"password"?,"server_password"?,"ident"?}
 *                             saves the server, its ident, a given room (the room
 *                             asked for in HELLO) and the passwords. ident absent:
 *                             the saved one, else the only LXMF identity, else the
 *                             node's when there is none; several and none saved is
 *                             an .error. Connected there already, a room is a move.
 *   partyline.cmd.disconnect  any non-empty value (the UIs write "1")
 *   partyline.cmd.auto        {"server","auto": 0|1}          autoconnect at boot; at
 *                                                             most one server has it
 *   partyline.cmd.move        {"room": <id> | "<name>", "password"?}
 *                             no password: the saved one for that room. A move that
 *                             succeeds with a password saves it; DENIED "wrong
 *                             password" forgets it, and its reason also lands on
 *                             partyline.cmd.move.error. A move to the current room
 *                             is not sent (.done at once: the server never answers it)
 *   partyline.cmd.text        "<text>"                        to the current room
 *   partyline.cmd.poke        {"mid": <id>, "text"}
 *   partyline.cmd.state       {"muted"?: 0|1, "deaf"?: 0|1}   saved; sent to the server
 *                             unless joined text-only (then an .error)
 *   partyline.cmd.nick        "<name>"                        used from the next join
 */
#pragma once

#include "service.h"

/** Boot-registered service: starts the partyline task with the RNS lifecycle. */
class PartylineService : public Service {
public:
    void onInit() override;
};
