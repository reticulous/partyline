/* Host test for the partyline wire module against packets captured from a real
 * partyline 1.0.5 server and client (gen_vectors.py, capture.py).
 *
 *   g++ -std=c++17 -Wall -Wextra -I../../src -I../../../../lxst/esp-idf/include test_wire.cpp ../../src/partyline_wire.cpp ../../../../lxst/esp-idf/src/lxst_msgpack.cpp ../../../../lxst/esp-idf/src/lxst_frame.cpp -o /tmp/test_wire && /tmp/test_wire
 */
#include "partyline_wire.h"
#include "vectors.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace plwire;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static bool same(const std::vector<uint8_t>& a, const uint8_t* b, size_t n)
{
    return a.size() == n && std::memcmp(a.data(), b, n) == 0;
}

template <size_t N>
static Packet dec(const uint8_t (&b)[N])
{
    Packet p;
    if (!decodePacket(b, N, p)) { std::printf("FAIL decodePacket\n"); failures++; }
    if (p.malformed) { std::printf("FAIL malformed 0x%x\n", p.malformed); failures++; }
    return p;
}

/* Re-encode a packet a client sent with our encoders; empty if we have none for it. */
static std::vector<uint8_t> reencode(const Packet& p)
{
    if (p.has(KEY_FRAMES)) return encodeFrames(p.frames.data(), p.frames.size(), p.seq);
    if (p.has(KEY_TALK_END)) return encodeTalkEnd();
    if (p.has(KEY_TEXT)) return encodeText(p.text);
    if (p.has(KEY_MOVE)) return encodeMove(p.move.room, p.move.has_password ? &p.move.password : nullptr);
    if (p.has(KEY_STATE)) return encodeState(p.state.muted, p.state.deaf);
    if (p.has(KEY_POKE)) return encodePoke(p.poke.member, p.poke.text);
    if (p.has(KEY_ADMIN)) return encodeAdmin(p.admin.action.c_str(), p.admin.target, p.admin.has_arg ? &p.admin.arg : nullptr);
    if (p.has(KEY_CONFIG) && p.config_motd) return encodeConfigMotd(p.motd);
    if (p.has(KEY_HELLO)) {
        Hello h;
        h.name = p.hello.name;
        if (p.hello.room_is_id) { h.room_kind = Hello::ROOM_ID; h.room_id = p.hello.room_id; }
        if (p.hello.room_is_name) { h.room_kind = Hello::ROOM_NAME; h.room_name = p.hello.room_name; }
        h.has_password = p.hello.has_password;               h.password = p.hello.password;
        h.has_server_password = p.hello.has_server_password; h.server_password = p.hello.server_password;
        h.text_only = p.hello.text_only; h.muted = p.hello.muted; h.deaf = p.hello.deaf;
        h.hops = p.hello.hops; h.rtt_ms = p.hello.rtt_ms; h.app = p.hello.app;
        return encodeHello(h);
    }
    return {};
}

/* The reference packetizer's batching, frame by frame: returns batch sizes. */
static std::vector<size_t> batches(size_t mdu, uint32_t max_frames, const std::vector<size_t>& lens)
{
    std::vector<size_t> out;
    size_t count = 0, packed = 0;
    for (size_t len : lens) {
        if (batchFlushBefore(mdu, count, packed, len)) { out.push_back(count); count = 0; packed = 0; }
        count++;
        packed += len + BATCH_FRAME_COST;
        if (count >= max_frames) { out.push_back(count); count = 0; packed = 0; }
    }
    if (count) out.push_back(count);
    return out;
}

int main()
{
    /* ── the profile table is partyline's ── */
    CHECK(PROFILE_COUNT == sizeof REF_PROFILES / sizeof REF_PROFILES[0]);
    for (size_t k = 0; k < PROFILE_COUNT && k < sizeof REF_PROFILES / sizeof REF_PROFILES[0]; ++k) {
        CHECK(std::strcmp(PROFILES[k].name, REF_PROFILES[k].name) == 0);
        CHECK(PROFILES[k].codec == REF_PROFILES[k].codec);
        CHECK(PROFILES[k].frame_ms == REF_PROFILES[k].frame_ms);
        CHECK(PROFILES[k].max_bytes == REF_PROFILES[k].limit);
        CHECK(profileFind(REF_PROFILES[k].name) == &PROFILES[k]);
        if (PROFILES[k].codec == LXST_CODEC_OPUS)
            CHECK(PROFILES[k].max_bytes ==
                  1 + lxstOpusMaxBytes(lxstOpusBitrateCeiling(PROFILES[k].mode), PROFILES[k].frame_ms));
    }
    CHECK(profileFind("c2-3200")->mode == LXST_C2_3200 && profileFind("c2-700")->mode == LXST_C2_700C);
    CHECK(profileFind("c2-1200")->mode == LXST_C2_1200 && profileFind("c2-2400")->mode == LXST_C2_2400);
    CHECK(profileFind("opus-high")->mode == LXST_OPUS_VOICE_HIGH && profileFind("music-high")->mode == LXST_OPUS_AUDIO_HIGH);
    CHECK(profileFind("c2-3201") == nullptr && profileFind("") == nullptr);

    /* ── batching ── */
    /* talk rate: full batches, Link packet overhead included */
    CHECK(talkBps(431, 64, 5, 40) == 1932);     /* 50 x 5 B / 2 s in 483 B packets */
    CHECK(talkBps(431, 64, 9, 20) == 5855);     /* 33 x 9 B / 660 ms */
    CHECK(talkBps(431, 1, 9, 20) == 39600);     /* one frame a packet: 99 B / 20 ms */
    CHECK(talkBps(431, 64, 500, 20) == 0);      /* never fits */
    CHECK(talkFitsHop(1932, 4000) && !talkFitsHop(5855, 4000) && talkFitsHop(5855, 0));
    /* airtime: a 15 s reserve below the limit, 60 s free to resume */
    CHECK(!airtimeBlocks(400000, 0, false) && !airtimeBlocks(400000, 0, true));   /* no budget */
    CHECK(!airtimeBlocks(344999, 360000, false));
    CHECK(airtimeBlocks(345000, 360000, false));
    CHECK(airtimeBlocks(360000, 360000, false));
    CHECK(airtimeBlocks(285001, 360000, true));    /* blocked: under 60 s free beyond the reserve */
    CHECK(!airtimeBlocks(284999, 360000, true));
    CHECK(airtimeBlocks(0, 10000, false));         /* a limit within the reserve */
    CHECK(batchMaxFrames(0, 200) == 1);
    CHECK(batchMaxFrames(1000, 200) == 5);
    CHECK(batchMaxFrames(1200, 400) == 3);
    CHECK(batchMaxFrames(100, 200) == 1);
    CHECK(batchMaxFrames(6000, 20) == 64);
    CHECK(batchMaxFrames(2000, 200) == 10);
    CHECK(batchBudget(431) == 415);
    /* 431-byte maximum data unit (MDU), c2-3200 (82 B): 8 + 4 × 85 = 348 fits, 5 × 85 + 8 = 433 does not. */
    CHECK(!batchFlushBefore(431, 3, 3 * 85, 82));
    CHECK(batchFlushBefore(431, 4, 4 * 85, 82));
    CHECK(!batchFlushBefore(431, 0, 0, 1000));   /* a lone frame always goes */
    /* What the reference client did in the capture: 11 × 82 B with ptt_jitter_ms 1000 → 4, 4, 3;
     * 5 × 42 B with ptt_jitter_ms 1200 → 3, 2. */
    CHECK(batches(CAPTURE_MDU, batchMaxFrames(1000, 200), std::vector<size_t>(11, 82)) == (std::vector<size_t>{ 4, 4, 3 }));
    CHECK(batches(CAPTURE_MDU, batchMaxFrames(1200, 400), std::vector<size_t>(5, 42)) == (std::vector<size_t>{ 3, 2 }));

    /* ── every captured packet ── */
    size_t total = 0, reencoded = 0, frame_pkts = 0, frames_checked = 0;
    uint32_t reencoded_keys = 0;
    for (const auto& c : ALL_PKTS) {
        Packet p;
        bool ok = decodePacket(c.b, c.n, p);
        total++;
        if (!ok || p.present != c.keys || p.malformed) {
            std::printf("FAIL %s: ok %d present 0x%x want 0x%x malformed 0x%x\n", c.label, ok, p.present, c.keys, p.malformed);
            failures++;
            continue;
        }
        if (p.has(KEY_FRAMES)) {
            frame_pkts++;
            CHECK(p.frames.size() == c.frame_count);
            CHECK(p.frames_list == c.frames_list);
            CHECK(p.has(KEY_SEQ) && p.seq == c.seq);
            CHECK(p.speaker == c.speaker);
            CHECK(p.has(KEY_SPEAKER) == !c.tx);
            const Profile* prof = c.profile ? profileFind(c.profile) : nullptr;
            CHECK(prof != nullptr);
            for (size_t i = 0; prof && i < p.frames.size(); ++i) {
                bool valid = frameValid(*prof, p.frames[i].p, p.frames[i].n);
                CHECK(valid == (((c.valid_mask >> i) & 1) != 0));
                CHECK(valid);   /* the reference client never sent an invalid frame */
                frames_checked++;
            }
        }
        if (c.tx) {
            std::vector<uint8_t> again = reencode(p);
            if (!again.empty()) {
                if (!same(again, c.b, c.n)) {
                    std::printf("FAIL %s: re-encoding differs (%zu vs %zu bytes)\n", c.label, again.size(), c.n);
                    failures++;
                } else {
                    reencoded++;
                    reencoded_keys |= p.present;
                }
            }
        }
    }
    const uint32_t must = (1u << KEY_HELLO) | (1u << KEY_FRAMES) | (1u << KEY_SEQ) | (1u << KEY_TALK_END) |
                          (1u << KEY_TEXT) | (1u << KEY_MOVE) | (1u << KEY_STATE) | (1u << KEY_POKE) |
                          (1u << KEY_ADMIN) | (1u << KEY_CONFIG);
    CHECK((reencoded_keys & must) == must);

    /* ── HELLO ── */
    {
        Packet p = dec(pkt_hello_alice);
        CHECK(p.hello.name == "alice" && p.hello.room_is_name && p.hello.room_name == "PTT");
        CHECK(!p.hello.has_password && !p.hello.has_server_password);
        CHECK(!p.hello.text_only && !p.hello.muted && !p.hello.deaf);
        CHECK(p.hello.hops == 0 && p.hello.rtt_ms >= 0);
        CHECK(p.hello.has_ver && p.hello.ver == PROTOCOL_VERSION && p.hello.app == "1.0.5");
        p = dec(pkt_hello_carol);
        CHECK(p.hello.text_only && p.hello.muted && p.hello.deaf);
        CHECK(!p.hello.room_is_id && !p.hello.room_is_name);
        /* the same content, built from scratch, gives the reference client's bytes */
        Hello h;
        h.name = "carol";
        h.text_only = h.muted = h.deaf = true;
        h.hops = 0;
        h.rtt_ms = p.hello.rtt_ms;
        h.app = "1.0.5";
        CHECK(same(encodeHello(h), pkt_hello_carol, sizeof pkt_hello_carol));
        /* a name over 48 bytes is cut */
        h.name = std::string(60, 'n');
        Packet q;
        std::vector<uint8_t> b = encodeHello(h);
        CHECK(decodePacket(b.data(), b.size(), q) && q.hello.name.size() == MAX_NAME);
    }

    /* ── WELCOME, sync, ROOM ── */
    {
        Packet p = dec(pkt_welcome_bob);
        CHECK(p.has(KEY_WELCOME));
        CHECK(p.welcome.name == "Capture Server" && p.welcome.sid == ID_BOB && p.welcome.motd == "capture motd");
        CHECK(p.welcome.has_ver && p.welcome.ver == PROTOCOL_VERSION && p.welcome.app == "1.0.5");

        p = dec(pkt_channel_sync_bob);
        CHECK(p.channels.size() == 4);
        const Channel& lobby = p.channels[0];
        CHECK(lobby.id == 1 && lobby.name == "Lobby" && lobby.profile == profileFind("opus-high"));
        CHECK(lobby.frame_ms == 20 && lobby.access == 0 && lobby.description.empty() && lobby.dialin.empty());
        CHECK(lobby.ptt_jitter_ms == 0 && lobby.flags == 0);
        const Channel& ptt = p.channels[1];
        CHECK(ptt.id == 2 && ptt.profile_name == "c2-3200" && ptt.frame_ms == 200 && ptt.ptt_jitter_ms == 1000);
        CHECK(ptt.description == "Codec2 push-to-talk");
        CHECK(p.channels[2].name == "Secret" && p.channels[2].access == ACCESS_PASSWORD);
        CHECK(p.channels[3].name == "PTT700" && p.channels[3].ptt_jitter_ms == 1200 && p.channels[3].frame_ms == 400);

        p = dec(pkt_user_sync_carol);
        CHECK(!p.users_update && p.users.size() == 2);
        CHECK(p.users[0].id == ID_BOB && p.users[0].name == "bob" && p.users[0].identity == IDENTITY_BOB);
        CHECK(p.users[1].id == ID_ALICE && p.users[1].op && p.users[1].identity == IDENTITY_ALICE);

        p = dec(pkt_synced_bob);
        CHECK(p.has(KEY_SYNCED) && p.present == (1u << KEY_SYNCED));

        p = dec(pkt_room_bob);
        CHECK(!p.room.none && p.room.id == 1 && p.room.profile == profileFind("opus-high"));
        CHECK(p.room.frame_ms == 20 && p.room.can_speak);
    }

    /* ── USER update, USER_LEFT ── */
    {
        Packet p = dec(pkt_user_update_bob);
        CHECK(p.users_update && p.users.size() == 1);
        const User& u = p.users[0];
        CHECK(u.id == ID_BOB && u.name == "bob" && u.identity == IDENTITY_BOB && u.in_room && u.room == 1);
        CHECK(!u.muted && !u.deaf && u.hops == 0 && u.rtt_ms >= 0 && !u.op && !u.server_muted);
        CHECK(!u.text_only && u.can_speak);

        /* carol is text-only, yet her STATE [false, false] unmuted and undeafened her */
        p = dec(pkt_user_carol_unmuted_bob);
        CHECK(p.users_update && p.users[0].id == ID_CAROL && p.users[0].text_only);
        CHECK(!p.users[0].muted && !p.users[0].deaf);

        p = dec(pkt_user_left_bob);
        CHECK(p.has(KEY_USER_LEFT) && p.user_left == ID_CAROL);
    }

    /* ── FRAMES ── */
    {
        Packet p = dec(pkt_frames_c2_alice);
        CHECK(p.frames_list && p.frames.size() == 4 && p.has(KEY_SEQ) && p.seq == 1 && !p.has(KEY_SPEAKER));
        for (const auto& f : p.frames) CHECK(f.n == 82 && frameValid(*profileFind("c2-3200"), f.p, f.n));
        /* re-encode from the frames alone */
        CHECK(same(encodeFrames(p.frames.data(), p.frames.size(), 1), pkt_frames_c2_alice, sizeof pkt_frames_c2_alice));
        Packet fwd = dec(pkt_frames_c2_bob);
        CHECK(fwd.speaker == ID_ALICE && fwd.seq == 1 && fwd.frames.size() == 4);
        for (size_t i = 0; i < 4; ++i)
            CHECK(fwd.frames[i].n == p.frames[i].n && std::memcmp(fwd.frames[i].p, p.frames[i].p, p.frames[i].n) == 0);

        p = dec(pkt_frames_c2_last_alice);
        CHECK(p.frames.size() == 3 && p.seq == 9);
        p = dec(pkt_frames_700_alice);
        CHECK(p.frames.size() == 3 && p.seq == 1 && p.frames[0].n == 42);

        p = dec(pkt_frames_opus_alice);
        CHECK(!p.frames_list && p.frames.size() == 1 && p.seq == 1);
        CHECK(frameValid(*profileFind("opus-high"), p.frames[0].p, p.frames[0].n));
        CHECK(same(encodeFrames(p.frames.data(), 1, 1), pkt_frames_opus_alice, sizeof pkt_frames_opus_alice));
        fwd = dec(pkt_frames_opus_bob);
        CHECK(!fwd.frames_list && fwd.speaker == ID_ALICE && fwd.seq == 1);
    }

    /* ── TALK_END ── */
    {
        Packet p = dec(pkt_talk_end_alice);
        CHECK(p.has(KEY_TALK_END) && !p.talk_end_member_set);
        CHECK(same(encodeTalkEnd(), pkt_talk_end_alice, sizeof pkt_talk_end_alice));
        p = dec(pkt_talk_end_bob);
        CHECK(p.talk_end_member_set && p.talk_end_member == ID_ALICE);
    }

    /* ── TEXT ── */
    {
        Packet p = dec(pkt_text_alice);
        CHECK(!p.text_from && p.text == "hello from alice");
        CHECK(same(encodeText("hello from alice"), pkt_text_alice, sizeof pkt_text_alice));
        CHECK(same(encodeText("  hello \t from   alice "), pkt_text_alice, sizeof pkt_text_alice));
        p = dec(pkt_text_from_alice_bob);
        CHECK(p.text_from && p.text_member == ID_ALICE && p.text == "hello from alice");
        const char* utf = "h\xc3\xa9llo w\xc3\xb6rld \xe2\x9c\x93";
        CHECK(same(encodeText(utf), pkt_text_bob, sizeof pkt_text_bob));
        p = dec(pkt_text_from_bob_alice);
        CHECK(p.text_member == ID_BOB && p.text == utf);
        CHECK(encodeText(" \t ").empty());
        Packet q;
        std::vector<uint8_t> b = encodeText(std::string(400, 'x'));
        CHECK(decodePacket(b.data(), b.size(), q) && q.text.size() == MAX_TEXT);
    }

    /* ── POKE ── */
    {
        CHECK(same(encodePoke(ID_ALICE, "ping"), pkt_poke_bob, sizeof pkt_poke_bob));
        Packet p = dec(pkt_poke_from_bob_alice);
        CHECK(p.poke.member == ID_BOB && p.poke.text == "ping");
        CHECK(same(encodePoke(ID_BOB, ""), pkt_poke_alice, sizeof pkt_poke_alice));   /* empty → "poke" */
    }

    /* ── MOVE, DENIED ── */
    {
        std::string wrong = "wrong", pw = "pw";
        CHECK(same(encodeMove(2, nullptr), pkt_move_bob, sizeof pkt_move_bob));
        CHECK(same(encodeMove(3, &wrong), pkt_move_wrong_alice, sizeof pkt_move_wrong_alice));
        CHECK(same(encodeMove(3, &pw), pkt_move_pw_alice, sizeof pkt_move_pw_alice));
        Packet p = dec(pkt_denied_alice);
        CHECK(!p.denied.final && p.denied.room == 3 && p.denied.reason == "wrong password");
        p = dec(pkt_denied_kick_carol);
        CHECK(p.denied.final && p.denied.reason == "kicked by alice");
        p = dec(pkt_denied_dave);
        CHECK(p.denied.final && p.denied.reason == "wrong server password");
    }

    /* ── STATE ── */
    CHECK(same(encodeState(true, false), pkt_state_mute_bob, sizeof pkt_state_mute_bob));

    /* ── ADMIN, NOTICE, CONFIG, CHANNEL update, CHANNEL_GONE, DIALIN ── */
    {
        CHECK(same(encodeAdmin("mute", ID_BOB, nullptr), pkt_admin_mute_alice, sizeof pkt_admin_mute_alice));
        Packet p = dec(pkt_notice_muted_bob);
        CHECK(p.notice == "You were muted by operator alice.");
        p = dec(pkt_notice_ops_only_bob);
        CHECK(p.notice == "operators only");
        CHECK(same(encodeConfigMotd("new message of the day"), pkt_config_motd_alice, sizeof pkt_config_motd_alice));
        p = dec(pkt_config_motd_bob);
        CHECK(p.has(KEY_CONFIG) && p.config_motd && p.motd == "new message of the day");
        p = dec(pkt_config_room_add_alice);
        CHECK(p.has(KEY_CONFIG) && !p.config_motd);
        p = dec(pkt_channel_update_bob);
        CHECK(p.channels.size() == 1 && p.channels[0].name == "Extra" && p.channels[0].profile == profileFind("c2-700"));
        int64_t extra = p.channels[0].id;
        p = dec(pkt_channel_gone_bob);
        CHECK(p.has(KEY_CHANNEL_GONE) && p.channel_gone == extra);
        p = dec(pkt_dialin_alice);
        CHECK(p.present == (1u << KEY_DIALIN));
    }

    /* ── announce app_data ── */
    {
        Announce a;
        CHECK(decodeAnnounce(ann_bob, sizeof ann_bob, a));
        CHECK(a.version == PROTOCOL_VERSION && a.name == "Capture Server" && a.flags == 0);
        CHECK(a.description == "wire capture" && a.language == "en" && a.country == "NL");
        CHECK(same(encodeAnnounce(a), ann_server_a, sizeof ann_server_a));
        CHECK(decodeAnnounce(ann_dave, sizeof ann_dave, a));
        CHECK(a.version == 1 && a.name.empty() && a.flags == (ANNOUNCE_HIDDEN | ANNOUNCE_PASSWORD));
        CHECK(a.description.empty() && a.language.empty() && a.country.empty());
        CHECK(same(encodeAnnounce(a), ann_server_b, sizeof ann_server_b));
        /* trailing empties trimmed, inner empties kept */
        a = Announce{};
        a.version = 1; a.name = "x"; a.country = "NL";
        std::vector<uint8_t> b = encodeAnnounce(a);
        const uint8_t want[] = { 0x96, 0x01, 0xa1, 'x', 0x00, 0xa0, 0xa0, 0xa2, 'N', 'L' };
        CHECK(same(b, want, sizeof want));
        const uint8_t not_int[] = { 0x93, 0xa1, '1', 0xa1, 'x', 0x00 };
        CHECK(!decodeAnnounce(not_int, sizeof not_int, a));
        const uint8_t v2[] = { 0x92, 0x02, 0xa1, 'x' };
        CHECK(decodeAnnounce(v2, sizeof v2, a) && a.version == 2 && a.flags == 0);
    }

    /* ── clean_text ── */
    for (const auto& t : CLEAN_VECS) {
        std::string got = cleanText(t.in, t.in_n, t.limit);
        if (got != std::string(t.out, t.out_n)) {
            std::printf("FAIL cleanText limit %zu: got [%s] want [%.*s]\n", t.limit, got.c_str(), (int)t.out_n, t.out);
            failures++;
        }
    }
    CHECK(cleanText("a\xff\xfe" "b", 4, 48) == "ab");   /* invalid UTF-8 dropped */

    /* ── malformed packets ── */
    {
        Packet p;
        const uint8_t not_map[] = { 0x91, 0x01 };
        CHECK(!decodePacket(not_map, sizeof not_map, p));
        const uint8_t junk[] = { 0xc1 };
        CHECK(!decodePacket(junk, 1, p));
        /* FRAMES an int, DENIED with three elements, SEQ out of range, a str key: the rest still decodes */
        const uint8_t mixed[] = { 0x85, 0x01, 0x05, 0x0b, 0x93, 0xc0, 0xa1, 'r', 0x00, 0x10, 0xce, 0x00, 0x01, 0x00, 0x00,
                                  0xa1, 'k', 0x01, 0x12, 0xa2, 'h', 'i' };
        CHECK(decodePacket(mixed, sizeof mixed, p));
        CHECK(p.malformed == ((1u << KEY_FRAMES) | (1u << KEY_DENIED) | (1u << KEY_SEQ)));
        CHECK(p.present == (1u << KEY_NOTICE) && p.notice == "hi");
        /* a list entry that is not a bin keeps its place */
        const uint8_t holes[] = { 0x82, 0x01, 0x93, 0xc4, 0x01, 0xaa, 0xc0, 0xc4, 0x01, 0xbb, 0x10, 0x05 };
        CHECK(decodePacket(holes, sizeof holes, p) && p.frames.size() == 3);
        CHECK(p.frames[0].n == 1 && p.frames[1].n == 0 && p.frames[2].n == 1 && p.frames[2].p[0] == 0xbb);
        /* ROOM [None, None, None, True]: in no room */
        const uint8_t no_room[] = { 0x81, 0x03, 0x94, 0xc0, 0xc0, 0xc0, 0xc3 };
        CHECK(decodePacket(no_room, sizeof no_room, p) && p.has(KEY_ROOM) && p.room.none);
        /* a USER record with only six fields: the rest default as the reference client does */
        const uint8_t short_user[] = { 0x81, 0x08, 0x96, 0x07, 0xa0, 0xc0, 0xc0, 0xc2, 0xc3 };
        CHECK(decodePacket(short_user, sizeof short_user, p) && p.users_update && p.users.size() == 1);
        CHECK(p.users[0].name == "guest-7" && !p.users[0].in_room && p.users[0].deaf && p.users[0].can_speak);
        CHECK(p.users[0].hops == -1 && p.users[0].rtt_ms == -1 && p.users[0].identity.empty());
        /* a float rtt is truncated as int() does */
        const uint8_t float_rtt[] = { 0x81, 0x08, 0x98, 0x07, 0xa0, 0xc0, 0xc0, 0xc2, 0xc2, 0x01,
                                      0xcb, 0x40, 0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };   /* 12.5 */
        CHECK(decodePacket(float_rtt, sizeof float_rtt, p) && p.users[0].rtt_ms == 12 && p.users[0].hops == 1);
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("test_wire: all passed (%zu captured packets, %zu FRAMES packets, %zu frames validated, "
                "%zu client packets re-encoded byte for byte)\n", total, frame_pkts, frames_checked, reencoded);
    return 0;
}
