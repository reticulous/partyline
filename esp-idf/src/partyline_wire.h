/**
 * partyline_wire.h — the partyline room protocol on the wire.
 *
 *   node                                          partyline server (lxst.room)
 *     ── {0x05 HELLO: {str keys}} ───────────────▶   once, first packet
 *     ◀── {0x06 WELCOME} {0x07 CHANNEL: [rec…]} {0x08 USER: [rec…]} {0x0E SYNCED}
 *     ◀── {0x03 ROOM: [room_id, profile, frame_ms, can_speak]}
 *     ── {0x01 FRAMES: bin | [bin…], 0x10 SEQ} ──▶   … {0x13 TALK_END: true}
 *     ◀── {0x01 FRAMES, 0x02 SPEAKER, 0x10 SEQ}       {0x13 TALK_END: member_id}
 *
 * Every packet is one msgpack map with integer keys; one packet may carry
 * several keys. decodePacket() turns any packet either side sends into a
 * Packet the task switches on (has(KEY_…)); the encode helpers write exactly
 * the bytes partyline 1.0.5's own client writes.
 *
 * Received display strings (names, text, reasons, MOTD, descriptions) come
 * back cleaned the way partyline's clean_text does it, with its fallbacks.
 * FRAMES entries point into the packet buffer, which must outlive them.
 *
 * Plain C++17 with no platform includes, so the host tests build it as-is.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lxst_frame.h"
#include "lxst_msgpack.h"

namespace plwire {

/** The protocol version in HELLO and WELCOME "ver". The server refuses any other. */
constexpr int64_t PROTOCOL_VERSION = 1;

/* Packet keys. */
enum Key : uint8_t {
    KEY_FRAMES       = 0x01,
    KEY_SPEAKER      = 0x02,
    KEY_ROOM         = 0x03,
    KEY_HELLO        = 0x05,
    KEY_WELCOME      = 0x06,
    KEY_CHANNEL      = 0x07,
    KEY_USER         = 0x08,
    KEY_USER_LEFT    = 0x09,
    KEY_MOVE         = 0x0A,
    KEY_DENIED       = 0x0B,
    KEY_TEXT         = 0x0C,
    KEY_STATE        = 0x0D,
    KEY_SYNCED       = 0x0E,
    KEY_POKE         = 0x0F,
    KEY_SEQ          = 0x10,
    KEY_ADMIN        = 0x11,
    KEY_NOTICE       = 0x12,
    KEY_TALK_END     = 0x13,
    KEY_DIALIN       = 0x14,
    KEY_CONFIG       = 0x15,
    KEY_CHANNEL_GONE = 0x16,
};

/* String limits in UTF-8 bytes. */
constexpr size_t MAX_NAME                 = 48;
constexpr size_t MAX_TEXT                 = 300;
constexpr size_t MAX_DESCRIPTION          = 120;
constexpr size_t MAX_ANNOUNCE_DESCRIPTION = 100;
constexpr size_t MAX_LOCALE               = 16;

/* CHANNEL access flags. */
constexpr uint32_t ACCESS_IDENTITY  = 1;
constexpr uint32_t ACCESS_ALLOWLIST = 2;
constexpr uint32_t ACCESS_PASSWORD  = 4;
/* CHANNEL room flags. */
constexpr uint32_t ROOM_BROADCAST = 1;
/* Announce flags. */
constexpr uint32_t ANNOUNCE_HIDDEN    = 1;
constexpr uint32_t ANNOUNCE_PASSWORD  = 2;
constexpr uint32_t ANNOUNCE_ALLOWLIST = 4;

/* Frames. */
constexpr size_t   MAX_FRAME_BYTES = 400;   /* the server's cap on any frame          */
constexpr uint32_t MAX_BATCH       = 64;    /* frames in one packet, either direction */
constexpr uint32_t MAX_JITTER_MS   = 30000; /* ptt_jitter_ms is clamped to this       */

/* ── profiles ── */

struct Profile {
    const char* name;
    uint8_t     codec;       /* LXST_CODEC_OPUS or LXST_CODEC_CODEC2              */
    uint8_t     mode;        /* Codec2: LXST_C2_* mode byte; Opus: LXST_OPUS_*    */
    uint16_t    frame_ms;
    uint16_t    max_bytes;   /* frame cap, codec byte included                    */
};

/** The ten room profiles, in partyline's order. */
extern const Profile PROFILES[];
extern const size_t  PROFILE_COUNT;

/** The profile with this name, or nullptr. */
const Profile* profileFind(const char* name, size_t len);
inline const Profile* profileFind(const std::string& s) { return profileFind(s.data(), s.size()); }

/** The server's frame check for a room of this profile. */
inline bool frameValid(const Profile& p, const uint8_t* frame, size_t len)
{
    return lxstFrameValid(frame, len, p.codec, p.frame_ms, p.max_bytes);
}

/* ── batching ──
 *
 * The sender's rule (client.py CountingPacketizer._ingest / target_count):
 * before adding a frame to a non-empty batch, flush if
 *     BATCH_BASE_COST + Σ(len + BATCH_FRAME_COST) > mdu − BATCH_MARGIN
 * with len the frame's length including its codec byte; after adding, flush
 * once the batch holds batchMaxFrames() frames. On key release the partial
 * batch is flushed, then TALK_END. */

constexpr size_t BATCH_BASE_COST  = 8;
constexpr size_t BATCH_FRAME_COST = 3;
constexpr size_t BATCH_MARGIN     = 16;
/** RNS's Link maximum data unit (MDU) at the default 500-byte maximum
 *  transmission unit (MTU); the client falls back to it
 *  only when the link reports none. */
constexpr size_t DEFAULT_MDU      = 431;

/** Frames per packet: 1 in a room without push-to-talk (ptt_jitter_ms 0),
 *  else min(64, max(1, ptt_jitter_ms / frame_ms)). */
uint32_t batchMaxFrames(uint32_t ptt_jitter_ms, uint32_t frame_ms);
/** The byte budget Σ-side of the rule is checked against: mdu − 16. */
inline size_t batchBudget(size_t mdu) { return mdu > BATCH_MARGIN ? mdu - BATCH_MARGIN : 0; }
/** True when a batch of `count` frames whose Σ(len + 3) is `packed` must be
 *  flushed before a frame of next_len bytes joins it. */
inline bool batchFlushBefore(size_t mdu, size_t count, size_t packed, size_t next_len)
{
    return count > 0 && BATCH_BASE_COST + packed + next_len + BATCH_FRAME_COST > batchBudget(mdu);
}

/** Our transmit rate while talking, bits/s: full batches of frameBytes-byte
 *  frames (codec byte included) of frameMs each, as many as the batching rule
 *  above allows at this mdu and maxFrames, each sent as one Link packet with
 *  Reticulum's 19-byte header and 48-byte token around the PKCS#7-padded
 *  payload. 0 for a frame that never fits. */
uint32_t talkBps(size_t mdu, uint32_t maxFrames, size_t frameBytes, uint32_t frameMs);
/** The share of the first hop's bitrate our own transmit may take. */
constexpr uint32_t HOP_SHARE_PCT = 60;
/** talkBps fits a first hop of hopBps bits/s (0 = unknown, fits). */
inline bool talkFitsHop(uint32_t bps, uint32_t hopBps)
{
    return !hopBps || (uint64_t)bps * 100 <= (uint64_t)hopBps * HOP_SHARE_PCT;
}
/** Airtime held back below a LoRa hop's hourly limit: its published use is up
 *  to 10 s old while it transmits and is checked each second, and the radio
 *  spends at most 1 s of airtime per second, so stopping at limit − 15 s keeps
 *  the hour's actual airtime within the limit. */
constexpr uint32_t AIRTIME_RESERVE_MS = 15000;
/** Free budget needed to speak again once blocked by airtime. */
constexpr uint32_t AIRTIME_RESUME_MS  = 60000;
/** The hourly airtime budget (limitMs, 0 = none) of which usedMs is spent
 *  blocks our transmit: used + AIRTIME_RESERVE_MS reaches the limit, or, while
 *  already blocked, less than AIRTIME_RESUME_MS beyond that is free. */
inline bool airtimeBlocks(uint32_t usedMs, uint32_t limitMs, bool blocked)
{
    if (!limitMs) return false;
    uint64_t need = (uint64_t)usedMs + AIRTIME_RESERVE_MS + (blocked ? AIRTIME_RESUME_MS : 0);
    return need >= limitMs;
}

/* ── text ── */

/** partyline's clean_text: drop control and invisible characters, collapse
 *  whitespace runs to one space, trim, then cut to `limit` bytes on a UTF-8
 *  character boundary. Invalid UTF-8 bytes are dropped. Returns "" when
 *  nothing is left. */
std::string cleanText(const char* p, size_t n, size_t limit);
inline std::string cleanText(const std::string& s, size_t limit) { return cleanText(s.data(), s.size(), limit); }

/* ── records ── */

struct Channel {
    int64_t        id = 0;
    std::string    name;            /* cleaned, fallback "Room <id>"            */
    std::string    profile_name;
    const Profile* profile = nullptr;   /* nullptr: unknown profile — the reference
                                           client skips such a record            */
    int64_t        frame_ms = 0;
    uint32_t       access = 0;      /* ACCESS_*                                 */
    std::string    description;     /* cleaned to MAX_DESCRIPTION               */
    std::string    dialin;          /* 32 hex chars, or empty                   */
    uint32_t       ptt_jitter_ms = 0;   /* clamped 0 … MAX_JITTER_MS; > 0 = push-to-talk room */
    uint32_t       flags = 0;       /* ROOM_*                                   */
};

struct User {
    int64_t     id = 0;
    std::string name;               /* cleaned, fallback "guest-<id>"           */
    std::string identity;           /* 32 hex chars, or empty (anonymous)       */
    bool        in_room = false;
    int64_t     room = 0;
    bool        muted = false, deaf = false;
    int32_t     hops = -1;          /* -1: unknown                              */
    int64_t     rtt_ms = -1;        /* -1: unknown                              */
    bool        op = false, server_muted = false, text_only = false;
    bool        can_speak = true;
};

/* ── decoded packet ── */

struct Bytes {
    const uint8_t* p = nullptr;
    size_t         n = 0;
};

struct Packet {
    uint32_t present = 0;   /* bit k: key k is in the packet and well-formed      */
    uint32_t malformed = 0; /* bit k: key k is in the packet but malformed        */
    bool has(Key k) const   { return present & (1u << k); }

    /* FRAMES (+ SPEAKER, SEQ). Entry i is frame number seq + i. A list entry
     * that is not a bin is kept as an empty Bytes so the numbering holds; at
     * most MAX_BATCH entries are kept. */
    std::vector<Bytes> frames;
    bool     frames_list = false;  /* sent as a list rather than one bare bin       */
    int64_t  speaker = 0;          /* 0 when SPEAKER is absent                       */
    uint16_t seq = 0;              /* valid when has(KEY_SEQ): an int 0 … 0xFFFF     */

    /* ROOM */
    struct {
        bool        none = false;  /* room_id not an int: in no room               */
        int64_t     id = 0;
        std::string profile_name;
        const Profile* profile = nullptr;  /* nullptr with !none: unknown profile — the
                                              reference client gives up the session */
        int64_t     frame_ms = 0;
        bool        can_speak = true;
    } room;

    /* HELLO (client → server; decoded for completeness and tests) */
    struct {
        std::string name;
        bool        room_is_id = false, room_is_name = false;
        int64_t     room_id = 0;
        std::string room_name;
        bool        has_password = false, has_server_password = false;
        std::string password, server_password;
        bool        text_only = false, muted = false, deaf = false;
        int32_t     hops = -1;
        int64_t     rtt_ms = -1;
        bool        has_ver = false;
        int64_t     ver = 0;
        std::string app;
    } hello;

    /* WELCOME */
    struct {
        std::string name;          /* cleaned, fallback "server"                */
        int64_t     sid = 0;       /* our member id                             */
        std::string motd;
        bool        has_ver = false;
        int64_t     ver = 0;       /* anything but PROTOCOL_VERSION ends the session */
        std::string app;
    } welcome;

    /* CHANNEL: always a list. USER: a list of records during sync, a single bare
     * record for an update (users_update true). */
    std::vector<Channel> channels;
    std::vector<User>    users;
    bool                 users_update = false;

    int64_t user_left = 0;
    int64_t channel_gone = 0;

    /* MOVE (client → server) */
    struct { int64_t room = 0; bool has_password = false; std::string password; } move;

    /* DENIED */
    struct { bool final = false; int64_t room = 0; std::string reason; } denied;

    /* TEXT: server → client [member_id, text] (text_from set), client → server text */
    bool        text_from = false;
    int64_t     text_member = 0;
    std::string text;

    /* STATE (client → server) */
    struct { bool muted = false, deaf = false; } state;

    /* POKE: [member_id, text] — the target when we send, the sender when we receive */
    struct { int64_t member = 0; std::string text; } poke;

    /* ADMIN (client → server) */
    struct { std::string action; int64_t target = 0; bool has_arg = false; int64_t arg = 0; } admin;

    std::string notice;

    /* TALK_END: server → client member_id (talk_end_member set), client → server true */
    bool    talk_end_member_set = false;
    int64_t talk_end_member = 0;

    /* CONFIG: only ["motd", text] matters to a non-operator; other actions just
     * set has(KEY_CONFIG) with config_motd false. */
    bool        config_motd = false;
    std::string motd;
};

/** Decode one packet. False only when it is not a msgpack map; a malformed key
 *  sets its bit in `malformed` and the other keys still decode. DIALIN is
 *  noted in `present` and otherwise ignored. */
bool decodePacket(const uint8_t* p, size_t n, Packet& out);

/* ── encoding (what we send) ── */

struct Hello {
    std::string name;                 /* cut to MAX_NAME                          */
    enum { ROOM_DEFAULT, ROOM_ID, ROOM_NAME } room_kind = ROOM_DEFAULT;
    int64_t     room_id = 0;
    std::string room_name;
    bool        has_password = false;          std::string password;
    bool        has_server_password = false;   std::string server_password;
    bool        text_only = false, muted = false, deaf = false;
    int32_t     hops = -1;            /* -1: nil                                  */
    int64_t     rtt_ms = -1;          /* -1: nil                                  */
    std::string app;                  /* our application version string          */
};

/** {HELLO: {name, room, password, server_password, text_only, muted, deaf,
 *  hops, rtt, ver, app}} in that key order, absent values as nil. */
std::vector<uint8_t> encodeHello(const Hello& h);
/** {FRAMES: bin, SEQ: seq} for one frame, {FRAMES: [bin…], SEQ: seq} for more. */
std::vector<uint8_t> encodeFrames(const Bytes* frames, size_t count, uint16_t seq);
/** {TALK_END: true} */
std::vector<uint8_t> encodeTalkEnd();
/** {TEXT: text}, cleaned to MAX_TEXT. Empty when nothing is left to send. */
std::vector<uint8_t> encodeText(const std::string& text);
/** {MOVE: [room_id, password | nil]} */
std::vector<uint8_t> encodeMove(int64_t room_id, const std::string* password);
/** {STATE: [muted, deaf]} */
std::vector<uint8_t> encodeState(bool muted, bool deaf);
/** {POKE: [target, text]}, text cleaned to MAX_TEXT, "poke" when empty. */
std::vector<uint8_t> encodePoke(int64_t target, const std::string& text);
/** {ADMIN: [action, target, arg | nil]} (operators only). */
std::vector<uint8_t> encodeAdmin(const char* action, int64_t target, const int64_t* arg);
/** {CONFIG: ["motd", text]} (operators only), text cleaned to MAX_TEXT. */
std::vector<uint8_t> encodeConfigMotd(const std::string& text);

/* ── announce app_data on lxst.room ── */

struct Announce {
    int64_t     version = 0;
    std::string name;          /* cleaned; empty = unnamed (a hidden server announces "") */
    uint32_t    flags = 0;     /* ANNOUNCE_*                                     */
    std::string description, language, country;
};

/** [version, name, flags(, description(, language(, country)))]. False when it
 *  is not an array whose first element is an int and that has a name; the
 *  caller skips any version other than PROTOCOL_VERSION. */
bool decodeAnnounce(const uint8_t* p, size_t n, Announce& out);
/** The server's own encoding: trailing empty fields trimmed, never below 3. */
std::vector<uint8_t> encodeAnnounce(const Announce& a);

}  // namespace plwire
