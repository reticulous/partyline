#!/usr/bin/env python3
"""Regenerate vectors.h from a partyline 1.0.5 capture.

    /home/spangap/partyline-venv/bin/python gen_vectors.py vectors.h \
        [/home/spangap/partyline-test/capture/out]

The capture directory holds packets.jsonl and meta.json written by
/home/spangap/partyline-test/capture/capture.py. Emitted:
  - every packet the reference clients sent or received (client-side records)
    in ALL_PKTS, with the set of keys umsgpack sees, and for FRAMES the room
    profile the client was in, which frames partyline's valid_frame accepts,
    the SEQ and the SPEAKER;
  - named packets (pkt_<name>) the test checks field by field;
  - the session's constants (member ids, identity hashes, link MDU);
  - partyline's profile table and frame limits.
"""
import json
import os
import sys

from RNS.vendor import umsgpack
from partyline import common

out_path = sys.argv[1] if len(sys.argv) > 1 else None
capture = sys.argv[2] if len(sys.argv) > 2 else "/home/spangap/partyline-test/capture/out"
if out_path:
    sys.stdout = open(out_path, "w")

records = [json.loads(line) for line in open(os.path.join(capture, "packets.jsonl"))]
meta = json.load(open(os.path.join(capture, "meta.json")))
CLIENTS = ("alice", "bob", "carol", "dave")


def carray(data):
    return "{" + ",".join(str(b) for b in data) + "}" if data else "{0}"


def cstr(text):
    return '"' + "".join(c if 32 <= ord(c) < 127 and c not in '"\\' else
                         "".join(f"\\x{b:02x}\"\"" for b in c.encode()) for c in text) + '"'


packets = []   # (label, proc, dir, data, value, profile)
room_profile = {}
counts = {}
for record in records:
    proc, direction = record["proc"], record["dir"]
    if proc not in CLIENTS or direction not in ("tx", "rx"):
        continue
    data = bytes.fromhex(record["hex"])
    value = umsgpack.unpackb(data)
    if direction == "rx" and common.FIELD_ROOM in value:
        room_profile[proc] = value[common.FIELD_ROOM][1]
    n = counts[(proc, direction)] = counts.get((proc, direction), 0) + 1
    packets.append((f"{proc} {direction} #{n}", proc, direction, data, value, room_profile.get(proc)))


def find(proc, direction, predicate, nth=0):
    hits = [p for p in packets if p[1] == proc and p[2] == direction and predicate(p[4])]
    if len(hits) <= nth:
        raise SystemExit(f"no packet {proc} {direction} #{nth} for the predicate")
    return hits[nth]


def key_is(key, test=lambda v: True):
    return lambda value: key in value and test(value[key])


F = common
NAMED = {
    "hello_bob": find("bob", "tx", key_is(F.FIELD_HELLO)),
    "hello_alice": find("alice", "tx", key_is(F.FIELD_HELLO)),
    "hello_carol": find("carol", "tx", key_is(F.FIELD_HELLO)),
    "hello_dave": find("dave", "tx", key_is(F.FIELD_HELLO)),
    "welcome_bob": find("bob", "rx", key_is(F.FIELD_WELCOME)),
    "channel_sync_bob": find("bob", "rx", key_is(F.FIELD_CHANNEL)),
    "user_sync_carol": find("carol", "rx", key_is(F.FIELD_USER, lambda v: isinstance(v[0], list))),
    "synced_bob": find("bob", "rx", key_is(F.FIELD_SYNCED)),
    "room_bob": find("bob", "rx", key_is(F.FIELD_ROOM)),
    "user_update_bob": find("bob", "rx", key_is(F.FIELD_USER)),
    "move_bob": find("bob", "tx", key_is(F.FIELD_MOVE)),
    "frames_c2_alice": find("alice", "tx", key_is(F.FIELD_FRAMES, lambda v: isinstance(v, list))),
    "frames_c2_bob": find("bob", "rx", key_is(F.FIELD_FRAMES, lambda v: isinstance(v, list))),
    "frames_c2_last_alice": find("alice", "tx", key_is(F.FIELD_FRAMES, lambda v: isinstance(v, list)), 2),
    "frames_700_alice": find("alice", "tx", key_is(F.FIELD_FRAMES, lambda v: isinstance(v, list) and len(v[0]) == 42)),
    "frames_opus_alice": find("alice", "tx", key_is(F.FIELD_FRAMES, lambda v: isinstance(v, bytes) and v[0] == 1)),
    "frames_opus_bob": find("bob", "rx", key_is(F.FIELD_FRAMES, lambda v: isinstance(v, bytes) and v[0] == 1)),
    "talk_end_alice": find("alice", "tx", key_is(F.FIELD_TALK_END)),
    "talk_end_bob": find("bob", "rx", key_is(F.FIELD_TALK_END)),
    "text_alice": find("alice", "tx", key_is(F.FIELD_TEXT)),
    "text_from_alice_bob": find("bob", "rx", key_is(F.FIELD_TEXT, lambda v: v[0] == 2)),
    "text_bob": find("bob", "tx", key_is(F.FIELD_TEXT)),
    "text_from_bob_alice": find("alice", "rx", key_is(F.FIELD_TEXT, lambda v: v[0] == 1)),
    "poke_bob": find("bob", "tx", key_is(F.FIELD_POKE)),
    "poke_from_bob_alice": find("alice", "rx", key_is(F.FIELD_POKE)),
    "poke_alice": find("alice", "tx", key_is(F.FIELD_POKE)),
    "move_wrong_alice": find("alice", "tx", key_is(F.FIELD_MOVE, lambda v: v[1] == "wrong")),
    "denied_alice": find("alice", "rx", key_is(F.FIELD_DENIED)),
    "move_pw_alice": find("alice", "tx", key_is(F.FIELD_MOVE, lambda v: v[1] == "pw")),
    "state_mute_bob": find("bob", "tx", key_is(F.FIELD_STATE)),
    "user_carol_unmuted_bob": find("bob", "rx", key_is(F.FIELD_USER, lambda v: v[0] == 3 and v[4] is False)),
    "admin_mute_alice": find("alice", "tx", key_is(F.FIELD_ADMIN)),
    "notice_muted_bob": find("bob", "rx", key_is(F.FIELD_NOTICE)),
    "notice_ops_only_bob": find("bob", "rx", key_is(F.FIELD_NOTICE, lambda v: v == "operators only")),
    "config_motd_alice": find("alice", "tx", key_is(F.FIELD_CONFIG)),
    "config_motd_bob": find("bob", "rx", key_is(F.FIELD_CONFIG)),
    "config_room_add_alice": find("alice", "tx", key_is(F.FIELD_CONFIG, lambda v: v[0] == "room_add")),
    "channel_update_bob": find("bob", "rx", key_is(F.FIELD_CHANNEL), 1),
    "channel_gone_bob": find("bob", "rx", key_is(F.FIELD_CHANNEL_GONE)),
    "dialin_alice": find("alice", "tx", key_is(F.FIELD_DIALIN)),
    "denied_kick_carol": find("carol", "rx", key_is(F.FIELD_DENIED)),
    "user_left_bob": find("bob", "rx", key_is(F.FIELD_USER_LEFT)),
    "denied_dave": find("dave", "rx", key_is(F.FIELD_DENIED)),
}

announces = {}
for record in records:
    if record["dir"] == "announce" and record["proc"] in ("bob", "dave"):
        announces.setdefault(record["proc"], bytes.fromhex(record["hex"]))
    if record["dir"] == "app_data_set":
        announces.setdefault(record["proc"], bytes.fromhex(record["hex"]))

print("/* Generated by gen_vectors.py from a partyline 1.0.5 capture (capture.py). */")
print("#pragma once\n#include <cstdint>\n#include <cstddef>\n")

mdus = {r["mdu"] for r in records if r.get("mdu")}
assert len(mdus) == 1, mdus
print(f"static const size_t CAPTURE_MDU = {mdus.pop()};")
print("static const int64_t ID_BOB = 1, ID_ALICE = 2, ID_CAROL = 3;")
for name in ("alice", "bob", "carol"):
    print(f"static const char IDENTITY_{name.upper()}[] = \"{meta['identities'][name]}\";")
print()

print("/* partyline.common.PROFILES and FRAME_LIMITS */")
print("struct RefProfile { const char* name; uint8_t codec; uint16_t frame_ms; uint16_t limit; };")
print("static const RefProfile REF_PROFILES[] = {")
for name, (codec_class, argument, frame_ms, _) in common.PROFILES.items():
    print(f"  {{\"{name}\", {common.codec_byte(name)[0]}, {frame_ms}, {common.FRAME_LIMITS[name]}}},")
print("};\n")

CLEAN = [
    ("plain", 48), ("  lots   of\t\tspace \n here  ", 300), ("\x00ctl\x1f\x7fchars\x85\x9f", 48),
    ("zero​width‎‏marks‪emb‮⁦iso⁩", 300), ("﻿bom­soft", 48),
    ("ideo　space en fig line para math ogham", 300),
    ("é" * 30, 48), ("é" * 30, 47), ("a" + "€" * 20, 48), ("a" + "€" * 20, 47), ("a" + "€" * 20, 46),
    ("😀" * 20, 48), ("😀" * 20, 50), ("😀" * 20, 49), ("abc d", 4), ("   ", 48), ("", 48),
    ("ㅤﾠᅟᅠ⠀᠎؜", 48), ("xㅤy", 48), ("tab\tnl\ncr\r", 48),
    ("hidden⁠⁡⁢⁣⁤⁪⁯end", 300), ("nbsp nbsp narrow", 48),
]
print("struct CleanVec { const char* in; size_t in_n; size_t limit; const char* out; size_t out_n; };")
print("static const CleanVec CLEAN_VECS[] = {")
for text, limit in CLEAN:
    cleaned = common.clean_text(text, limit)
    print(f"  {{{cstr(text)}, {len(text.encode())}, {limit}, {cstr(cleaned)}, {len(cleaned.encode())}}},")
print("};\n")

for proc, data in sorted(announces.items()):
    print(f"static const uint8_t ann_{proc}[] = {carray(data)};   /* {umsgpack.unpackb(data)!r} */")
print()

for name, (label, proc, direction, data, value, profile) in NAMED.items():
    if F.FIELD_FRAMES in value:
        frames = value[F.FIELD_FRAMES]
        sizes = [len(f) for f in frames] if isinstance(frames, list) else len(frames)
        rest = {k: v for k, v in value.items() if k != F.FIELD_FRAMES}
        shown = f"FRAMES {'list' if isinstance(frames, list) else 'bin'} {sizes} {rest!r}"
    else:
        shown = repr(value)
    if len(shown) > 160:
        shown = shown[:160] + "…"
    shown = shown.replace("*/", "* /")
    print(f"/* {label}: {shown} */")
    print(f"static const uint8_t pkt_{name}[] = {carray(data)};")
print()

print("struct CapPkt {")
print("    const char* label; const uint8_t* b; size_t n; bool tx; uint32_t keys;")
print("    const char* profile;      /* FRAMES: the room the client was in */")
print("    uint8_t frame_count; bool frames_list; uint64_t valid_mask; int32_t seq; int64_t speaker;")
print("};")
for k, (label, proc, direction, data, value, profile) in enumerate(packets):
    print(f"static const uint8_t cap_{k}[] = {carray(data)};")
print("static const CapPkt ALL_PKTS[] = {")
for k, (label, proc, direction, data, value, profile) in enumerate(packets):
    keys = 0
    for key in value:
        keys |= 1 << key
    frame_count, frames_list, mask, seq, speaker = 0, False, 0, -1, 0
    if F.FIELD_FRAMES in value:
        frames = value[F.FIELD_FRAMES]
        frames_list = isinstance(frames, list)
        batch = frames if frames_list else [frames]
        frame_count = len(batch)
        for i, frame in enumerate(batch):
            if common.valid_frame(frame, profile):
                mask |= 1 << i
        seq = value.get(F.FIELD_SEQ, -1)
        speaker = value.get(F.FIELD_SPEAKER, 0)
    print(f"  {{\"{label}\", cap_{k}, sizeof cap_{k}, {'true' if direction == 'tx' else 'false'}, 0x{keys:x}u, "
          f"{cstr(profile) if profile else 'nullptr'}, {frame_count}, {'true' if frames_list else 'false'}, "
          f"0x{mask:x}ull, {seq}, {speaker}}},")
print("};")
