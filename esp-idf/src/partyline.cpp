/**
 * partyline — the Partyline voice-room client task.
 *
 *   node                                            partyline server (lxst.room)
 *     ◀── ANNOUNCE lxst.room [1, name, flags, …]       → partyline.heard.<dest>
 *     ── LINK + LINKIDENTIFY (rnsd holds the identify for establishment) ─▶
 *     ── HELLO (once, on rnsd.links.<tag>.state "active") ─────────────────▶
 *     ◀── WELCOME  (within max(10 s, 15 × rtt), else a new session after backoff)
 *     ◀── CHANNEL … USER … SYNCED, ROOM                → partyline.rooms / members
 *     ◀▶ TEXT, POKE, STATE, MOVE, DENIED, NOTICE, CONFIG …
 *     ◀▶ FRAMES / TALK_END  ⇄  the lxst voice session (LXST_OP_* / LXST_EV_*)
 *
 * One server at a time. A lost Link is a new session after a backoff of 5 s ·
 * 15 s · 45 s · 2 min · 5 min (±20 %); a DENIED without a room is final until
 * the user connects again. Joined text-only (by choice, or because the voice
 * session is not to be had), the task never sends STATE: the server does not
 * enforce text-only, and a STATE would start audio.
 *
 * Everything runs on the partyline task: storage subscriptions and ITS
 * callbacks dispatch through its one itsPoll, plus a coarse tick for
 * deadlines. Storage map and commands: partyline.h.
 */
#include "partyline.h"
#include "partyline_wire.h"
#include "lxst.h"

#include "spangap.h"
#include "ports.h"
#include "rnsd.h"
#include "mem.h"

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

static const char* TAG = "partyline";

#define PL_ASPECT      "lxst.room"
#define PL_TAG_PFX     "pl"                 /* link tags pl0 … pl7, a new one per session */
#define PL_TAGS        8
#define PL_HEARD_MAX   64
#define PL_LOG_KEEP    100
#define PL_LXMF_SLOTS  10
#define PL_TICK_MS     500
#define PL_LINK_BUF    8192
#define PL_VOICE_RETRY_MS 5000
#define PL_IDENT_WAIT_MS    60000
#define PL_IDENT_RECHECK_MS 2000
#define PL_NODE_IDENT  "secrets.rnsd.identity"
#define PL_WRONG_ROOM_PW   "wrong password"
#define PL_WRONG_SERVER_PW "wrong server password"
#define DOT            " \xC2\xB7 "

/* ─────────────── small helpers ─────────────── */

static int64_t nowMs() { return esp_timer_get_time() / 1000; }
static int     monoS() { return (int)(nowMs() / 1000); }
static int64_t s_taskStartAt = 0, s_bootAgainAt = 0;
static void    bootServers();
static uint32_t wallS()
{
    time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0;
}

static std::string hexOf(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}

static bool hexTo(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

/* A destination typed with spaces anywhere, or in full (64 hex), is the
 * 32-hex truncated hash. */
static std::string destOf(const std::string& in)
{
    std::string s;
    for (char c : in) if (!std::isspace((unsigned char)c)) s += (char)std::tolower((unsigned char)c);
    if (s.size() == 64) s.resize(32);
    uint8_t b[16];
    return hexTo(s, b, 16) ? s : "";
}

static std::string itos(int64_t v) { return std::to_string((long long)v); }

/** The next packet on `h` into buf: its length, 0 when none is queued. A
 *  packet larger than cap is dropped (itsRecv logs it) and the next one read. */
static size_t recvNext(int h, void* buf, size_t cap)
{
    for (;;) {
        size_t before = itsBytesAvailable(h);
        if (!before) return 0;
        size_t n = itsRecv(h, buf, cap, 0);
        if (n) return n;
        if (itsBytesAvailable(h) >= before) return 0;
    }
}

/* ─────────────── identities ─────────────── */

static bool lxmfSlotLive(int n)
{
    char k[48];
    std::snprintf(k, sizeof k, "s.lxmf.id.%d.kind", n);
    if (storageGetStr(k, "") == "group") return false;
    std::snprintf(k, sizeof k, "lxmf.id.%d.dest_hash", n);
    uint8_t b[16];
    return hexTo(storageGetStr(k, ""), b, 16);
}

/** An LXMF identity is stored but lxmf has not published it yet: right after
 *  boot, before lxmf has loaded its identities. */
static bool lxmfLoading()
{
    for (int n = 0; n < PL_LXMF_SLOTS; ++n) {
        char k[48];
        std::snprintf(k, sizeof k, "s.lxmf.id.%d.kind", n);
        if (storageGetStr(k, "") == "group" || lxmfSlotLive(n)) continue;
        std::snprintf(k, sizeof k, "secrets.lxmf.id.%d.privkey", n);
        if (storageExists(k)) return true;
    }
    return false;
}

/** The LXMF slot of an identity path, or -1 for anything else. */
static int lxmfSlotOf(const std::string& path)
{
    int n = -1;
    char tail[16] = {};
    if (std::sscanf(path.c_str(), "secrets.lxmf.id.%d.%15s", &n, tail) != 2) return -1;
    if (std::strcmp(tail, "privkey") || n < 0 || n >= PL_LXMF_SLOTS) return -1;
    return n;
}

static bool identValid(const std::string& path)
{
    if (path == PL_NODE_IDENT) return true;
    int n = lxmfSlotOf(path);
    return n >= 0 && lxmfSlotLive(n);
}

/** The only LXMF identity; the node's own when there is none; "" when there
 *  are several, which is the user's to choose. */
static std::string identDefault()
{
    int found = -1;
    for (int n = 0; n < PL_LXMF_SLOTS; ++n) {
        if (!lxmfSlotLive(n)) continue;
        if (found >= 0) return "";
        found = n;
    }
    if (found < 0) return PL_NODE_IDENT;
    return "secrets.lxmf.id." + std::to_string(found) + ".privkey";
}

static std::string nickFor(const std::string& ident)
{
    std::string n = storageGetStr("s.partyline.nick", "");
    int slot = lxmfSlotOf(ident);
    if (n.empty() && slot >= 0)
        n = storageGetStr(("s.lxmf.id." + std::to_string(slot) + ".display_name").c_str(), "");
    return plwire::cleanText(n, plwire::MAX_NAME);
}

/* ─────────────── saved servers and secrets ─────────────── */

static std::string srvKey(const std::string& d, const char* f) { return "s.partyline.servers." + d + "." + f; }
static std::string srvPwKey(const std::string& d) { return "secrets.partyline.servers." + d + ".password"; }
static std::string roomPwKey(const std::string& d, const std::string& roomName)
{
    std::string n = lower(trim(roomName));
    return "secrets.partyline.rooms." + d + "." + hexOf((const uint8_t*)n.data(), n.size());
}

static bool serverSaved(const std::string& d) { return storageExists(srvKey(d, "auto").c_str()); }

static void serverSave(const std::string& d)
{
    if (serverSaved(d)) return;
    std::string name = storageGetStr(("partyline.heard." + d + ".name").c_str(), "");
    storageBegin();
    storageSet(srvKey(d, "name").c_str(), name.c_str());
    storageSet(srvKey(d, "ident").c_str(), "");
    storageSet(srvKey(d, "auto").c_str(), 0);
    storageSet(srvKey(d, "room").c_str(), "");
    storageEnd();
}

/* storageSet is asynchronous: a caller that has just written the secret says
 * so rather than have it read back. */
static void serverPwPublish(const std::string& d, int known = -1)
{
    bool saved = known >= 0 ? known != 0 : storageExists(srvPwKey(d).c_str());
    storageSet(("partyline.servers." + d + ".saved_pw").c_str(), saved ? 1 : 0);
}

/* ─────────────── session state ─────────────── */

/* File-local types: an anonymous namespace keeps them from colliding with
 * same-named types in other straddles under the one-definition rule. */
namespace {

enum Phase { PH_IDLE, PH_LINKING, PH_HELLO, PH_JOINED, PH_DENIED, PH_LOST };

struct Member {
    plwire::User u;
    bool         speaking = false;
};

struct Session {
    std::string dest;               /* "" = none */
    std::string ident;              /* identity path presented there */
    std::string tag;
    int         link = -1;
    Phase       ph = PH_IDLE;
    int64_t     welcomeBy = 0;
    bool        synced = false;
    bool        textOnly = false;   /* what HELLO said */
    const char* textWhy = "";       /* why, when not by choice */
    bool        voiceOff = false;   /* the voice session failed after a full join */
    /* the room's audio profile (nullptr: none), and when to reopen a dropped
     * voice session (0: not pending) */
    const plwire::Profile* voiceProf = nullptr;
    int64_t     voiceFrameMs = 0;
    int64_t     voiceReopenAt = 0;
    int64_t     sid = -1;
    std::string selfName;
    bool        inRoom = false;
    int64_t     room = 0;
    bool        canSpeak = false;
    /* the move in flight, so its outcome can save or forget its password */
    bool        movePending = false;
    int64_t     moveRoom = 0;
    bool        moveHasPw = false;
    std::string movePw;
    /* the room HELLO asked for, and with which password */
    std::string helloRoom;
    bool        helloHasPw = false;
};

struct Retry { bool armed = false; int attempt = 0; int64_t at = 0; int shown = -1; };

}  // namespace

static Session s_s;
static int     s_tagNext = 0;
static std::map<int64_t, plwire::Channel> s_rooms;
static std::map<int64_t, int>             s_roomCount;
static std::map<int64_t, Member>          s_members;

static Retry s_retry;

static uint8_t* s_linkBuf = nullptr;

/* the lxst voice session */
static int            s_lx = -1;
static uint8_t*       s_lxBuf = nullptr;
static size_t         s_lxBufLen = 0;
static lxst_profile_t s_prof = {};
static uint16_t       s_seq = 1;
static int            s_lxState = -1;   /* last LXST_OP_STATE sent, packed; -1 none */
static bool           s_lxHeld = false; /* a live call holds the voice session */
/* Why our audio may not go out on the server link's first hop ("" = it may):
 * hopCheck's verdict, published as partyline.tx_blocked. */
static const char*    s_txBlock = "";
/** The voice session may transmit: the server lets us speak and the first hop
 *  carries the room's stream. */
static bool speakNow() { return s_s.canSpeak && !*s_txBlock; }
static std::vector<uint8_t> s_fwd;

static uint32_t s_txDrop = 0, s_rxDrop = 0, s_statShown = UINT32_MAX;

/* ─────────────── publishing ─────────────── */

static void publishState(const char* state, const std::string& status)
{
    storageBegin();
    storageSet("partyline.state", state);
    storageSet("partyline.status", status.c_str());
    storageEnd();
}

static std::string roomName(int64_t id)
{
    auto it = s_rooms.find(id);
    return it != s_rooms.end() ? it->second.name : "Room " + itos(id);
}

static std::string memberName(int64_t mid)
{
    auto it = s_members.find(mid);
    return it != s_members.end() ? it->second.u.name : "guest-" + itos(mid);
}

static void logLine(const char* kind, int64_t mid, const std::string& name, const std::string& text)
{
    static int next = 0;
    int n = next++;
    std::string base = "partyline.log." + std::to_string(n) + ".";
    uint32_t ts = wallS();
    storageBegin();
    storageSet((base + "ts").c_str(), ts ? (int)ts : monoS());
    storageSet((base + "kind").c_str(), kind);
    if (mid >= 0) storageSet((base + "mid").c_str(), (int)mid);
    else storageSet((base + "mid").c_str(), "");
    storageSet((base + "name").c_str(), name.c_str());
    if (s_s.inRoom) storageSet((base + "room").c_str(), (int)s_s.room);
    else storageSet((base + "room").c_str(), "");
    storageSet((base + "text").c_str(), text.c_str());
    storageSet("partyline.log_next", n + 1);
    storageEnd();
    if (n >= PL_LOG_KEEP) storageDeleteTree(("partyline.log." + std::to_string(n - PL_LOG_KEEP)).c_str());
}

static void logSystem(const std::string& text) { logLine("system", -1, "", text); }

static std::string profileText(const plwire::Channel& c)
{
    if (!c.profile) return (c.profile_name.empty() ? std::string("unknown profile") : c.profile_name) + " (not supported)";
    const plwire::Profile& p = *c.profile;
    std::string s;
    if (p.codec == LXST_CODEC_CODEC2) {
        static const char* modes[] = { "700C", "1200", "1300", "1400", "1600", "2400", "3200" };
        s = std::string("Codec2 ") + (p.mode < 7 ? modes[p.mode] : "?");
    } else {
        int kbps = 0;
        bool voice = true;
        switch (p.mode) {
        case LXST_OPUS_VOICE_LOW:    kbps = 6;  break;
        case LXST_OPUS_VOICE_MEDIUM: kbps = 8;  break;
        case LXST_OPUS_VOICE_HIGH:   kbps = 16; break;
        case LXST_OPUS_AUDIO_LOW:    kbps = 14; voice = false; break;
        case LXST_OPUS_AUDIO_MEDIUM: kbps = 28; voice = false; break;
        case LXST_OPUS_AUDIO_HIGH:   kbps = 56; voice = false; break;
        default: break;
        }
        s = std::string("Opus ") + (voice ? "voice " : "music ") + std::to_string(kbps) + " kbps";
    }
    s += DOT + itos(c.frame_ms ? c.frame_ms : p.frame_ms) + " ms";
    if (c.ptt_jitter_ms) {
        char b[24];
        if (c.ptt_jitter_ms % 1000 == 0) std::snprintf(b, sizeof b, "%u s", (unsigned)(c.ptt_jitter_ms / 1000));
        else std::snprintf(b, sizeof b, "%.1f s", c.ptt_jitter_ms / 1000.0);
        s += DOT "push-to-talk " + std::string(b);
    }
    if (c.flags & plwire::ROOM_BROADCAST) s += DOT "broadcast";
    return s;
}

static void roomPwPublish(const plwire::Channel& c, int known = -1)
{
    bool saved = known >= 0 ? known != 0 : storageExists(roomPwKey(s_s.dest, c.name).c_str());
    storageSet(("partyline.rooms." + itos(c.id) + ".saved_pw").c_str(), saved ? 1 : 0);
}

static void publishRoom(const plwire::Channel& c)
{
    std::string b = "partyline.rooms." + itos(c.id) + ".";
    storageBegin();
    storageSet((b + "name").c_str(), c.name.c_str());
    storageSet((b + "profile").c_str(), c.profile_name.c_str());
    storageSet((b + "profile_text").c_str(), profileText(c).c_str());
    storageSet((b + "frame_ms").c_str(), (int)c.frame_ms);
    storageSet((b + "access").c_str(), (int)c.access);
    storageSet((b + "desc").c_str(), c.description.c_str());
    storageSet((b + "ptt_jitter_ms").c_str(), (int)c.ptt_jitter_ms);
    storageSet((b + "flags").c_str(), (int)c.flags);
    if (!s_roomCount.count(c.id)) {
        s_roomCount[c.id] = 0;
        storageSet((b + "members").c_str(), 0);
    }
    storageEnd();
    roomPwPublish(c);
}

/** Recount every room's members and publish the counts that changed. */
static void publishCounts()
{
    std::map<int64_t, int> n;
    for (auto& [id, r] : s_rooms) n[id] = 0;
    for (auto& [mid, m] : s_members)
        if (m.u.in_room && n.count(m.u.room)) n[m.u.room]++;
    for (auto& [id, c] : n) {
        auto it = s_roomCount.find(id);
        if (it != s_roomCount.end() && it->second == c) continue;
        s_roomCount[id] = c;
        storageSet(("partyline.rooms." + itos(id) + ".members").c_str(), c);
    }
}

static void publishMember(const Member& m)
{
    const plwire::User& u = m.u;
    std::string b = "partyline.members." + itos(u.id) + ".";
    storageBegin();
    storageSet((b + "name").c_str(), u.name.c_str());
    storageSet((b + "identity").c_str(), u.identity.c_str());
    if (u.in_room) storageSet((b + "room").c_str(), (int)u.room);
    else storageSet((b + "room").c_str(), "");
    storageSet((b + "muted").c_str(), u.muted ? 1 : 0);
    storageSet((b + "deaf").c_str(), u.deaf ? 1 : 0);
    storageSet((b + "hops").c_str(), (int)u.hops);
    storageSet((b + "rtt").c_str(), u.rtt_ms > 0 ? (int)u.rtt_ms : 0);
    storageSet((b + "op").c_str(), u.op ? 1 : 0);
    storageSet((b + "server_muted").c_str(), u.server_muted ? 1 : 0);
    storageSet((b + "text_only").c_str(), u.text_only ? 1 : 0);
    storageSet((b + "can_speak").c_str(), u.can_speak ? 1 : 0);
    storageSet((b + "speaking").c_str(), m.speaking ? 1 : 0);
    storageSet((b + "self").c_str(), u.id == s_s.sid ? 1 : 0);
    storageEnd();
}

static void publishStats()
{
    if (s_txDrop + s_rxDrop == s_statShown) return;
    s_statShown = s_txDrop + s_rxDrop;
    storageBegin();
    storageSet("partyline.stats.tx_drop", (int)s_txDrop);
    storageSet("partyline.stats.rx_drop", (int)s_rxDrop);
    storageEnd();
}

static std::string serverLabel()
{
    std::string n = storageGetStr("partyline.name", "");
    if (n.empty()) n = storageGetStr(srvKey(s_s.dest, "name").c_str(), "");
    return n.empty() ? s_s.dest.substr(0, 8) : n;
}

static void publishLive()
{
    std::string st = "Connected to " + serverLabel();
    if (s_s.textOnly && !*s_s.textWhy) st += ", text only";
    else if (s_s.textOnly || s_s.voiceOff) st += std::string(", text only: ") + s_s.textWhy;
    if (s_lxHeld) st += ", on a call";
    publishState("live", st);
}

/* ─────────────── the link ─────────────── */

static bool linkSend(const std::vector<uint8_t>& b)
{
    if (s_s.link < 0 || b.empty()) return false;
    if (itsSend(s_s.link, b.data(), b.size(), 0) == 0) {
        s_txDrop++;
        return false;
    }
    return true;
}

/* ─────────────── the voice session ─────────────── */

static void lxSendState();
static void setSpeaking(int64_t mid, bool on);

static void lxBufFree()
{
    if (s_lxBuf) heap_caps_free(s_lxBuf);
    s_lxBuf = nullptr;
    s_lxBufLen = 0;
}

/** Room in the receive buffer for the largest event lxst sends under this
 *  profile: an LXST_EV_BATCH ([op][count] then count × [len u16][frame]) of
 *  several frames stays within batch_budget, a single frame within
 *  max_frame_bytes. */
static bool lxBufFit(const lxst_profile_t& p)
{
    size_t one = 4 + (size_t)p.max_frame_bytes;
    size_t many = p.batch_budget ? 2 + (size_t)p.batch_budget
                                 : 2 + (size_t)LXST_MAX_BATCH * (2 + p.max_frame_bytes);
    size_t need = std::max(one, many);
    if (need <= s_lxBufLen) return true;
    lxBufFree();
    s_lxBuf = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!s_lxBuf) return false;
    s_lxBufLen = need;
    return true;
}

/** A live call holds the voice session (LXST_EV_HELD), or lets it go. Joined
 *  with audio, the server is told muted and deaf while held, so it stops
 *  sending audio, and the user's own muted and deaf again after; text-only,
 *  it is never sent a STATE. `announce`: say so in the room log. */
static void lxHeld(bool held, bool announce)
{
    if (held == s_lxHeld) return;
    s_lxHeld = held;
    storageSet("partyline.held", held ? 1 : 0);
    if (s_s.ph != PH_JOINED) return;
    if (!s_s.textOnly && !s_s.voiceOff) {
        bool muted = held || storageGetInt("s.partyline.muted", 0) != 0;
        bool deaf = held || storageGetInt("s.partyline.deaf", 0) != 0;
        linkSend(plwire::encodeState(muted, deaf));
    }
    if (s_s.synced) publishLive();
    if (announce) logSystem(held ? "On a call: the room is on hold" : "Call over: back in the room");
}

static void lxClose()
{
    lxHeld(false, false);
    if (s_lx >= 0) itsDisconnect(s_lx);
    s_lx = -1;
    s_lxState = -1;
    s_s.voiceReopenAt = 0;
    lxBufFree();
    storageSet("partyline.audio", 0);
    for (auto& [mid, m] : s_members) setSpeaking(mid, false);
}

/** The lxst profile for the room we are in, from ROOM's profile and frame
 *  length, the room's push-to-talk jitter and the Link's maximum data unit
 *  (MDU). */
static bool lxProfileFor(const plwire::Profile& p, int64_t frame_ms, lxst_profile_t& out)
{
    const plwire::Channel* c = nullptr;
    auto it = s_rooms.find(s_s.room);
    if (it != s_rooms.end()) c = &it->second;
    uint32_t fms = frame_ms > 0 ? (uint32_t)frame_ms : p.frame_ms;
    uint32_t jitter = c ? c->ptt_jitter_ms : 0;
    std::string mk = "rnsd.links." + s_s.tag + ".mdu";
    int mdu = storageExists(mk.c_str()) ? storageGetInt(mk.c_str(), 0) : 0;
    if (mdu <= 0) mdu = (int)plwire::DEFAULT_MDU;
    out = {};
    out.codec = p.codec;
    out.mode = p.mode;
    out.frame_ms = (uint16_t)fms;
    out.max_frame_bytes = p.max_bytes;
    out.batch_budget = (uint16_t)plwire::batchBudget((size_t)mdu);
    out.batch_base = (uint8_t)plwire::BATCH_BASE_COST;
    out.batch_per_frame = (uint8_t)plwire::BATCH_FRAME_COST;
    out.batch_max_frames = (uint8_t)plwire::batchMaxFrames(jitter, fms);
    out.jitter_ms = (uint16_t)jitter;
    out.max_jitter_ms = jitter ? (uint16_t)plwire::MAX_JITTER_MS : 0;   /* 0: the session's 3 s */
    out.sequenced = 1;
    out.can_speak = speakNow() ? 1 : 0;
    out.tx_mode = LXST_TX_SETTING;
    out.rx_any = 0;
    return true;
}

static void onLxRecv(int handle, size_t);
static void onLxDisc(int);

/** Connect the voice session with this profile (s_lxBuf already fits it). */
static bool lxConnect(const lxst_profile_t& prof)
{
    int h = itsConnect(LXST_TASK, LXST_PORT_SESSION, &prof, sizeof prof, pdMS_TO_TICKS(1000), 0, onLxRecv, onLxDisc);
    if (h < 0) return false;
    s_lx = h;
    s_prof = prof;
    s_seq = 1;
    s_lxState = -1;
    storageSet("partyline.audio", 1);
    lxSendState();
    info("partyline: voice session open (%u ms frames, budget %u, %u per batch)", (unsigned)prof.frame_ms,
         (unsigned)prof.batch_budget, (unsigned)prof.batch_max_frames);
    return true;
}

/** Open the session, or re-profile it, for the room ROOM put us in. A new
 *  room (`moved`) always sends the profile, which releases the old room's
 *  speakers. */
static void lxEnter(const plwire::Profile& p, int64_t frame_ms, bool moved)
{
    lxst_profile_t prof;
    lxProfileFor(p, frame_ms, prof);
    if (s_lx >= 0) {
        /* can_speak travels in LXST_OP_STATE; a change of it alone is no new
         * profile (a PROFILE drops every speaker's buffer). */
        prof.can_speak = s_prof.can_speak;
        if (!moved && !std::memcmp(&prof, &s_prof, sizeof prof)) return;
        if (!lxBufFit(prof)) { err("partyline: no memory for the voice session"); return; }
        uint8_t m[1 + sizeof prof];
        m[0] = LXST_OP_PROFILE;
        std::memcpy(m + 1, &prof, sizeof prof);
        if (itsSend(s_lx, m, sizeof m, 0) == 0) { warn("partyline: lxst profile change not sent"); return; }
        s_prof = prof;
        s_seq = 1;
        return;
    }
    if (!lxBufFit(prof)) { err("partyline: no memory for the voice session"); return; }
    if (!lxConnect(prof)) {
        lxBufFree();
        /* Joined with audio: deafen and mute on the server so it stops
         * sending us frames nobody plays, and stay text-only here. */
        warn("partyline: the voice session could not be opened");
        s_s.voiceOff = true;
        s_s.textWhy = "the voice session is in use";
        linkSend(plwire::encodeState(true, true));
        logSystem("No audio: the voice session is in use");
        if (s_s.synced) publishLive();
    }
}

/** Reopen the voice session lxst dropped while we are still in a room with
 *  audio; a refusal tries again after PL_VOICE_RETRY_MS. */
static void lxReopen(int64_t now)
{
    if (!s_s.voiceReopenAt || now < s_s.voiceReopenAt) return;
    s_s.voiceReopenAt = 0;
    if (s_lx >= 0 || s_s.ph != PH_JOINED || !s_s.inRoom || !s_s.voiceProf || s_s.textOnly || s_s.voiceOff) return;
    lxst_profile_t prof;
    lxProfileFor(*s_s.voiceProf, s_s.voiceFrameMs, prof);
    if (lxBufFit(prof) && lxConnect(prof)) return;
    lxBufFree();
    s_s.voiceReopenAt = now + PL_VOICE_RETRY_MS;
}

static void lxSendState()
{
    if (s_lx < 0) return;
    bool muted = storageGetInt("s.partyline.muted", 0) != 0;
    bool deaf = storageGetInt("s.partyline.deaf", 0) != 0;
    int packed = (speakNow() ? 1 : 0) | (muted ? 2 : 0) | (deaf ? 4 : 0);
    if (packed == s_lxState) return;
    uint8_t m[4] = { LXST_OP_STATE, (uint8_t)(speakNow() ? 1 : 0), (uint8_t)(muted ? 1 : 0), (uint8_t)(deaf ? 1 : 0) };
    if (itsSend(s_lx, m, sizeof m, 0) > 0) s_lxState = packed;
}

static void lxTalkEnd(int64_t mid)
{
    if (s_lx < 0) return;
    uint32_t sp = (uint32_t)mid;
    uint8_t m[5] = { LXST_OP_TALK_END, (uint8_t)sp, (uint8_t)(sp >> 8), (uint8_t)(sp >> 16), (uint8_t)(sp >> 24) };
    if (itsSend(s_lx, m, sizeof m, 0) == 0) s_rxDrop++;
}

static void lxFrames(const plwire::Packet& p)
{
    if (s_lx < 0 || p.frames.empty()) return;
    uint32_t sp = (uint32_t)p.speaker;
    uint16_t seq = p.has(plwire::KEY_SEQ) ? p.seq : 0;
    size_t count = std::min(p.frames.size(), (size_t)LXST_MAX_BATCH);
    s_fwd.clear();
    s_fwd.push_back(LXST_OP_FRAMES);
    for (int i = 0; i < 4; ++i) s_fwd.push_back((uint8_t)(sp >> (8 * i)));
    s_fwd.push_back((uint8_t)seq);
    s_fwd.push_back((uint8_t)(seq >> 8));
    s_fwd.push_back((uint8_t)count);
    for (size_t i = 0; i < count; ++i) {
        const plwire::Bytes& f = p.frames[i];
        s_fwd.push_back((uint8_t)f.n);
        s_fwd.push_back((uint8_t)(f.n >> 8));
        if (f.n) s_fwd.insert(s_fwd.end(), f.p, f.p + f.n);
    }
    if (itsSend(s_lx, s_fwd.data(), s_fwd.size(), 0) == 0) s_rxDrop++;
}

static void setSpeaking(int64_t mid, bool on)
{
    auto it = s_members.find(mid);
    if (it == s_members.end() || it->second.speaking == on) return;
    it->second.speaking = on;
    storageSet(("partyline.members." + itos(mid) + ".speaking").c_str(), on ? 1 : 0);
}

static void onLxRecv(int handle, size_t)
{
    if (!s_lxBuf) return;
    for (;;) {
        size_t n = recvNext(handle, s_lxBuf, s_lxBufLen);
        if (n == 0) return;
        if (handle != s_lx) continue;
        const uint8_t* m = s_lxBuf;
        switch (m[0]) {
        case LXST_EV_BATCH: {
            if (n < 2) break;
            size_t count = m[1];
            plwire::Bytes frames[LXST_MAX_BATCH];
            size_t k = 0, at = 2;
            while (k < count && k < (size_t)LXST_MAX_BATCH && at + 2 <= n) {
                size_t len = m[at] | (size_t)m[at + 1] << 8;
                at += 2;
                if (at + len > n) break;
                frames[k].p = m + at;
                frames[k].n = len;
                k++;
                at += len;
            }
            if (!k) break;
            if (s_s.ph == PH_JOINED && s_s.inRoom) linkSend(plwire::encodeFrames(frames, k, s_seq));
            s_seq = (uint16_t)(s_seq + k);
            break;
        }
        case LXST_EV_TALK_END:
            if (s_s.ph == PH_JOINED && s_s.inRoom) linkSend(plwire::encodeTalkEnd());
            break;
        case LXST_EV_SPEAKING: {
            if (n < 6) break;
            uint32_t sp = m[1] | (uint32_t)m[2] << 8 | (uint32_t)m[3] << 16 | (uint32_t)m[4] << 24;
            setSpeaking(sp == LXST_SPEAKER_SELF ? s_s.sid : (int64_t)sp, m[5] != 0);
            break;
        }
        case LXST_EV_HELD:
            if (n >= 2) lxHeld(m[1] != 0, true);
            break;
        default:
            break;
        }
    }
}

/* ITS passes the connection's ref (0, from lxConnect), not its handle; there is
 * one voice-session connection at a time, so any disconnect is that one. */
static void onLxDisc(int)
{
    if (s_lx < 0) return;
    lxHeld(false, true);
    s_lx = -1;
    s_lxState = -1;
    lxBufFree();
    storageSet("partyline.audio", 0);
    for (auto& [mid, m] : s_members) setSpeaking(mid, false);
    info("partyline: the voice session closed");
    if (s_s.ph == PH_JOINED && s_s.inRoom && s_s.voiceProf && !s_s.textOnly && !s_s.voiceOff)
        s_s.voiceReopenAt = nowMs() + PL_VOICE_RETRY_MS;
}

/* ─────────────── session lifecycle ─────────────── */

static void clearRoster()
{
    s_rooms.clear();
    s_roomCount.clear();
    s_members.clear();
    storageDeleteTree("partyline.rooms");
    storageDeleteTree("partyline.members");
}

/** Close the link and the voice session and forget the server's state. The
 *  server we held stays named in partyline.server unless `forgetServer`. */
static void sessionEnd(bool forgetServer)
{
    lxClose();
    if (s_s.link >= 0) itsDisconnect(s_s.link);
    std::string dest = s_s.dest;
    std::string ident = s_s.ident;
    s_s = Session{};
    if (!forgetServer) { s_s.dest = dest; s_s.ident = ident; }
    clearRoster();
    storageBegin();
    storageSet("partyline.server", s_s.dest.c_str());
    storageSet("partyline.sid", "");
    storageSet("partyline.room", "");
    storageSet("partyline.can_speak", 0);
    storageSet("partyline.audio", 0);
    if (forgetServer) {
        storageSet("partyline.name", "");
        storageSet("partyline.motd", "");
    }
    storageEnd();
}

static void scheduleRetry()
{
    static const int ladder[] = { 5, 15, 45, 120, 300 };
    int base = ladder[std::min(s_retry.attempt, 4)];
    int jitter = (int)(esp_random() % (unsigned)(base * 40 + 1)) - base * 20;   /* ±20 % in 1/100 s */
    s_retry.armed = true;
    s_retry.at = nowMs() + base * 1000 + jitter * 10;
    s_retry.attempt++;
    s_retry.shown = -1;
    s_s.ph = PH_LOST;
    publishState("lost", "Connection lost; retrying in " + std::to_string(base) + " s");
}

static void onLinkRecv(int handle, size_t);
static void onLinkDisc(int handle);

/** Open a Link to `dest` as `ident`. On failure the state says why. */
static bool sessionOpen(const std::string& dest, const std::string& ident)
{
    uint8_t dh[16], ih[16];
    hexTo(dest, dh, 16);
    s_s = Session{};
    s_s.dest = dest;
    s_s.ident = ident;
    storageSet("partyline.server", dest.c_str());
    if (!rnsdIdentityHash(ident.c_str(), ih)) {
        s_s.ph = PH_IDLE;
        publishState("idle", "Not connected: that identity is not available");
        return false;
    }
    s_s.tag = PL_TAG_PFX + std::to_string(s_tagNext);
    s_tagNext = (s_tagNext + 1) % PL_TAGS;
    int h = rnsdLinkOpen(dh, PL_ASPECT, ident.c_str(), s_s.tag.c_str(), 0, 0, 0, onLinkRecv, onLinkDisc);
    if (h < 0) {
        warn("partyline %s: no link", dest.c_str());
        scheduleRetry();
        return false;
    }
    s_s.link = h;
    s_s.ph = PH_LINKING;
    /* rnsd holds the identify and runs it at establishment, ahead of HELLO.
     * The server's destination proves nothing, so rnsd is told not to wait. */
    if (!rnsdLinkIdentify(s_s.tag.c_str(), ident == PL_NODE_IDENT ? "" : ident.c_str()))
        warn("partyline %s: identify could not be queued", dest.c_str());
    if (!rnsdLinkExpectProofs(s_s.tag.c_str(), false))
        warn("partyline %s: proof expectation could not be queued", dest.c_str());
    storageBegin();
    storageSet("partyline.name", storageGetStr(srvKey(dest, "name").c_str(), "").c_str());
    storageSet("partyline.motd", "");
    storageEnd();
    publishState("connecting", "Connecting");
    info("partyline %s: linking as %s", dest.c_str(), ident.c_str());
    return true;
}

static void sessionLost(const char* why)
{
    if (s_s.ph == PH_IDLE || s_s.ph == PH_DENIED || s_s.ph == PH_LOST) return;
    info("partyline %s: %s", s_s.dest.c_str(), why);
    bool wasJoined = s_s.ph == PH_JOINED;
    sessionEnd(false);
    if (wasJoined) logSystem("Connection lost");
    scheduleRetry();
}

/** The voice session is to be had when lxst is running and nobody holds it. */
static bool lxstAvailable()
{
    if (!storageExists("lxst.state")) return false;
    return s_lx >= 0 || storageGetStr("lxst.state", "idle") != "live";
}

static void sendHello()
{
    plwire::Hello h;
    h.name = nickFor(s_s.ident);
    std::string room = storageGetStr(srvKey(s_s.dest, "room").c_str(), "");
    if (!room.empty()) {
        h.room_kind = plwire::Hello::ROOM_NAME;
        h.room_name = room;
        std::string pk = roomPwKey(s_s.dest, room);
        if (storageExists(pk.c_str())) {
            h.has_password = true;
            h.password = storageGetStr(pk.c_str(), "");
        }
    }
    if (storageExists(srvPwKey(s_s.dest).c_str())) {
        h.has_server_password = true;
        h.server_password = storageGetStr(srvPwKey(s_s.dest).c_str(), "");
    }
    s_s.textOnly = storageGetInt("s.partyline.text_only", 0) != 0;
    s_s.textWhy = "";
    if (!s_s.textOnly && !lxstAvailable()) {
        s_s.textOnly = true;
        s_s.textWhy = storageExists("lxst.state") ? "the voice session is in use" : "no voice session";
    }
    h.text_only = s_s.textOnly;
    h.muted = storageGetInt("s.partyline.muted", 0) != 0;
    h.deaf = storageGetInt("s.partyline.deaf", 0) != 0;
    std::string lk = "rnsd.links." + s_s.tag + ".";
    h.hops = storageExists((lk + "hops").c_str()) ? storageGetInt((lk + "hops").c_str(), 0) : -1;
    int rtt = storageExists((lk + "rtt_ms").c_str()) ? storageGetInt((lk + "rtt_ms").c_str(), 0) : -1;
    h.rtt_ms = rtt;
    std::string ver = storageGetStr("sys.build.version", "");
    h.app = "reticulous" + (ver.empty() ? std::string() : " " + ver);
    s_s.helloRoom = room;
    s_s.helloHasPw = h.has_password;
    s_s.ph = PH_HELLO;
    s_s.welcomeBy = nowMs() + std::max<int64_t>(10000, 15LL * std::max(rtt, 0));
    if (!linkSend(plwire::encodeHello(h))) warn("partyline %s: HELLO not sent", s_s.dest.c_str());
    publishState("joining", "Joining");
}

/* rnsd.links.pl<N>.state — the Link coming up, or failing. */
static void onLinkState(const char* key, const char* val)
{
    if (s_s.tag.empty() || !val) return;
    std::string want = "rnsd.links." + s_s.tag + ".state";
    if (want != key) return;
    if (!std::strcmp(val, "awaiting_path")) {
        if (s_s.ph == PH_LINKING) publishState("connecting", "Connecting: looking for a path");
    } else if (!std::strcmp(val, "establishing")) {
        if (s_s.ph == PH_LINKING) publishState("connecting", "Connecting");
    } else if (!std::strcmp(val, "active")) {
        if (s_s.ph == PH_LINKING) sendHello();
    } else if (!std::strcmp(val, "failed") || !std::strcmp(val, "closed")) {
        std::string e = storageGetStr(("rnsd.links." + s_s.tag + ".last_error").c_str(), "");
        sessionLost(e.empty() ? val : e.c_str());
    }
}

static void onLinkDisc(int handle)
{
    if (handle != s_s.link) return;
    s_s.link = -1;
    sessionLost("link gone");
}

/* ─────────────── inbound ─────────────── */

static void sessionDenied(const std::string& reason)
{
    std::string dest = s_s.dest;
    if (reason == PL_WRONG_SERVER_PW) {
        storageUnset(srvPwKey(dest).c_str());
        storageSet(("partyline.servers." + dest + ".saved_pw").c_str(), 0);
    }
    logSystem("Refused: " + reason);
    sessionEnd(false);
    s_retry = Retry{};
    s_s.ph = PH_DENIED;
    publishState("denied", "Refused: " + reason);
    info("partyline %s: refused: %s", dest.c_str(), reason.c_str());
}

static void onWelcome(const plwire::Packet& p)
{
    if (s_s.ph != PH_HELLO) return;
    const auto& w = p.welcome;
    if (!w.has_ver || w.ver != plwire::PROTOCOL_VERSION) {
        sessionDenied("the server speaks Partyline protocol " + itos(w.ver) + ", this client speaks " +
                      itos(plwire::PROTOCOL_VERSION));
        return;
    }
    s_s.ph = PH_JOINED;
    s_s.sid = w.sid;
    s_retry = Retry{};
    storageBegin();
    storageSet("partyline.name", w.name.c_str());
    storageSet("partyline.motd", w.motd.c_str());
    storageSet("partyline.sid", (int)w.sid);
    if (serverSaved(s_s.dest) && storageGetStr(srvKey(s_s.dest, "name").c_str(), "").empty())
        storageSet(srvKey(s_s.dest, "name").c_str(), w.name.c_str());
    storageEnd();
    info("partyline %s: welcomed by \"%s\" as member %lld", s_s.dest.c_str(), w.name.c_str(), (long long)w.sid);
}

static void onChannels(const plwire::Packet& p)
{
    for (const auto& c : p.channels) {
        s_rooms[c.id] = c;
        publishRoom(c);
        if (s_s.inRoom && c.id == s_s.room && c.profile) {
            s_s.voiceProf = c.profile;
            s_s.voiceFrameMs = c.frame_ms;
            if (s_lx >= 0) lxEnter(*c.profile, c.frame_ms, false);
        }
    }
    publishCounts();
}

static void onUsers(const plwire::Packet& p)
{
    for (const auto& u : p.users) {
        auto it = s_members.find(u.id);
        bool isNew = it == s_members.end();
        bool wasHere = !isNew && s_s.inRoom && it->second.u.in_room && it->second.u.room == s_s.room;
        bool isHere = s_s.inRoom && u.in_room && u.room == s_s.room;
        Member& m = s_members[u.id];
        m.u = u;
        publishMember(m);
        if (u.id == s_s.sid) {
            s_s.selfName = u.name;
            if (s_s.canSpeak != u.can_speak) {
                s_s.canSpeak = u.can_speak;
                storageSet("partyline.can_speak", u.can_speak ? 1 : 0);
                lxSendState();
            }
            continue;
        }
        if (!s_s.synced || !p.users_update) continue;
        if (isHere && !wasHere) logSystem(u.name + (isNew ? " joined" : " came in"));
        else if (wasHere && !isHere)
            logSystem(u.name + (u.in_room ? " went to " + roomName(u.room) : " left the room"));
        if (wasHere && !isHere) { lxTalkEnd(u.id); setSpeaking(u.id, false); }
    }
    publishCounts();
}

static void onUserLeft(int64_t mid)
{
    auto it = s_members.find(mid);
    if (it == s_members.end()) return;
    if (s_s.synced && s_s.inRoom && it->second.u.in_room && it->second.u.room == s_s.room)
        logSystem(it->second.u.name + " left");
    lxTalkEnd(mid);
    s_members.erase(it);
    storageDeleteTree(("partyline.members." + itos(mid)).c_str());
    publishCounts();
}

static void onRoom(const plwire::Packet& p)
{
    const auto& r = p.room;
    s_s.canSpeak = r.can_speak && !r.none;
    if (r.none) {
        s_s.inRoom = false;
        storageBegin();
        storageSet("partyline.room", "");
        storageSet("partyline.can_speak", 0);
        storageEnd();
        lxClose();
        s_s.voiceProf = nullptr;
        s_s.movePending = false;
        logSystem("You are in no room");
        return;
    }
    bool moved = !s_s.inRoom || s_s.room != r.id;
    s_s.inRoom = true;
    s_s.room = r.id;
    storageBegin();
    storageSet("partyline.room", (int)r.id);
    storageSet("partyline.can_speak", r.can_speak ? 1 : 0);
    storageEnd();
    std::string name = roomName(r.id);
    if (s_s.movePending && s_s.moveRoom == r.id) {
        /* our move went through: remember the room, and its password */
        storageSet(srvKey(s_s.dest, "room").c_str(), name.c_str());
        if (s_s.moveHasPw) storageSet(roomPwKey(s_s.dest, name).c_str(), s_s.movePw.c_str());
        auto c = s_rooms.find(r.id);
        if (c != s_rooms.end() && s_s.moveHasPw) roomPwPublish(c->second, 1);
    }
    s_s.movePending = false;
    s_s.movePw.clear();
    if (moved) logSystem("You are in " + name);
    for (auto& [mid, m] : s_members) setSpeaking(mid, false);

    s_s.voiceProf = r.profile;
    s_s.voiceFrameMs = r.frame_ms;
    if (s_s.textOnly || s_s.voiceOff) return;
    if (!r.profile) {
        lxClose();
        logSystem("No audio here: this client does not know the profile " + r.profile_name);
        return;
    }
    lxEnter(*r.profile, r.frame_ms, moved);
    lxSendState();
}

static void onDenied(const plwire::Packet& p)
{
    const auto& d = p.denied;
    if (d.final) {
        sessionDenied(d.reason);
        return;
    }
    std::string name = roomName(d.room);
    logSystem(name + ": " + d.reason);
    if (d.reason == PL_WRONG_ROOM_PW) {
        auto c = s_rooms.find(d.room);
        std::string rn = c != s_rooms.end() ? c->second.name : "";
        if (!rn.empty()) {
            storageUnset(roomPwKey(s_s.dest, rn).c_str());
            storageSet(("partyline.rooms." + itos(d.room) + ".saved_pw").c_str(), 0);
        }
        if (!s_s.helloRoom.empty() && lower(trim(s_s.helloRoom)) == lower(trim(rn)) && s_s.helloHasPw)
            s_s.helloHasPw = false;
    }
    if (s_s.movePending && s_s.moveRoom == d.room) {
        s_s.movePending = false;
        storageSet("partyline.cmd.move.error", (name + ": " + d.reason).c_str());
    }
}

static void handlePacket(const uint8_t* b, size_t n)
{
    using namespace plwire;
    static Packet* pp = new Packet;   /* reused: the decode is too big for the stack */
    Packet& p = *pp;
    if (!decodePacket(b, n, p)) {
        dbg("partyline: undecodable packet (%u B)", (unsigned)n);
        return;
    }
    /* Version, ban, allow list, server password and server full are refused
     * before WELCOME (server full even before HELLO): final all the same. */
    if ((s_s.ph == PH_LINKING || s_s.ph == PH_HELLO) && p.has(KEY_DENIED) && p.denied.final) {
        sessionDenied(p.denied.reason);
        return;
    }
    if (p.has(KEY_WELCOME)) onWelcome(p);
    if (s_s.ph != PH_JOINED) return;
    if (p.has(KEY_CHANNEL)) onChannels(p);
    if (p.has(KEY_USER)) onUsers(p);
    if (p.has(KEY_USER_LEFT)) onUserLeft(p.user_left);
    if (p.has(KEY_SYNCED) && !s_s.synced) {
        s_s.synced = true;
        publishLive();
        logSystem("Connected to " + serverLabel());
    }
    if (p.has(KEY_ROOM)) onRoom(p);
    if (p.has(KEY_DENIED)) {
        onDenied(p);
        if (s_s.ph != PH_JOINED) return;
    }
    if (p.has(KEY_NOTICE)) logLine("notice", -1, "", p.notice);
    if (p.has(KEY_TEXT) && p.text_from) logLine("text", p.text_member, memberName(p.text_member), p.text);
    if (p.has(KEY_POKE)) logLine("poke", p.poke.member, memberName(p.poke.member), p.poke.text);
    if (p.has(KEY_CONFIG) && p.config_motd) storageSet("partyline.motd", p.motd.c_str());
    if (p.has(KEY_CHANNEL_GONE)) {
        s_rooms.erase(p.channel_gone);
        s_roomCount.erase(p.channel_gone);
        storageDeleteTree(("partyline.rooms." + itos(p.channel_gone)).c_str());
    }
    if (p.has(KEY_TALK_END) && p.talk_end_member_set) lxTalkEnd(p.talk_end_member);
    if (p.has(KEY_FRAMES)) lxFrames(p);
}

/* msgpack reads for the audio path: an unsigned int, a bin. */
static bool mpUint(const uint8_t*& p, const uint8_t* e, uint64_t& v)
{
    if (p >= e) return false;
    uint8_t b = *p++;
    if (b <= 0x7f) { v = b; return true; }
    int w = b == 0xcc ? 1 : b == 0xcd ? 2 : b == 0xce ? 4 : b == 0xcf ? 8 : 0;
    if (!w || e - p < w) return false;
    v = 0;
    while (w--) v = v << 8 | *p++;
    return true;
}

static bool mpBin(const uint8_t*& p, const uint8_t* e, size_t& len)
{
    if (p >= e) return false;
    uint8_t b = *p++;
    int w = b == 0xc4 ? 1 : b == 0xc5 ? 2 : b == 0xc6 ? 4 : 0;
    if (!w || e - p < w) return false;
    len = 0;
    while (w--) len = len << 8 | *p++;
    if ((size_t)(e - p) < len) return false;
    p += len;
    return true;
}

/** A packet that is {FRAMES: bin | [bin…], SPEAKER?, SEQ?} and nothing else
 *  goes to the voice session without a decode tree, so received audio costs
 *  no heap. False: not such a packet, and the general decode takes it. */
static bool audioFast(const uint8_t* b, size_t n)
{
    using namespace plwire;
    const uint8_t* p = b;
    const uint8_t* e = b + n;
    if (!n || (*p & 0xf0) != 0x80) return false;
    int entries = *p++ & 0x0f;
    const uint8_t* fp = nullptr;
    size_t count = 0;
    uint64_t speaker = 0, seq = 0;
    for (int i = 0; i < entries; ++i) {
        uint64_t key;
        if (!mpUint(p, e, key)) return false;
        if (key == KEY_FRAMES && !fp) {
            if (p >= e) return false;
            if ((*p & 0xf0) == 0x90) count = *p++ & 0x0f;
            else if (*p == 0xdc && e - p >= 3) { count = (size_t)p[1] << 8 | p[2]; p += 3; }
            else count = 1;
            if (!count) return false;
            fp = p;
            for (size_t k = 0, len; k < count; ++k) if (!mpBin(p, e, len)) return false;
        } else if (key == KEY_SPEAKER) {
            if (!mpUint(p, e, speaker)) return false;
        } else if (key == KEY_SEQ) {
            if (!mpUint(p, e, seq) || seq > 0xffff) return false;
        } else {
            return false;
        }
    }
    if (!fp || p != e) return false;
    if (s_s.ph != PH_JOINED || s_lx < 0) return true;
    count = std::min(count, (size_t)LXST_MAX_BATCH);
    uint32_t sp = (uint32_t)speaker;
    s_fwd.clear();
    s_fwd.push_back(LXST_OP_FRAMES);
    for (int i = 0; i < 4; ++i) s_fwd.push_back((uint8_t)(sp >> (8 * i)));
    s_fwd.push_back((uint8_t)seq);
    s_fwd.push_back((uint8_t)(seq >> 8));
    s_fwd.push_back((uint8_t)count);
    p = fp;
    for (size_t k = 0, len; k < count; ++k) {
        mpBin(p, e, len);
        s_fwd.push_back((uint8_t)len);
        s_fwd.push_back((uint8_t)(len >> 8));
        s_fwd.insert(s_fwd.end(), p - len, p);
    }
    if (itsSend(s_lx, s_fwd.data(), s_fwd.size(), 0) == 0) s_rxDrop++;
    return true;
}

static void onLinkRecv(int handle, size_t)
{
    for (;;) {
        size_t n = recvNext(handle, s_linkBuf, PL_LINK_BUF);
        if (n == 0) return;
        if (handle == s_s.link && !audioFast(s_linkBuf, n)) handlePacket(s_linkBuf, n);
    }
}

/* ─────────────── announces ─────────────── */

static int s_annHandle = -1;
static std::map<std::string, int> s_heard;   /* dest → seen */
constexpr size_t ANN_HDR = 1 + 16 + 16 + 64 + 32;

static void onAnnounce(int handle, size_t)
{
    PSRAM_BSS static uint8_t buf[ANN_HDR + 512];
    for (;;) {
        size_t n = recvNext(handle, buf, sizeof buf);
        if (n == 0) return;
        if (handle != s_annHandle || n < ANN_HDR) continue;
        plwire::Announce a;
        if (!plwire::decodeAnnounce(buf + ANN_HDR, n - ANN_HDR, a) || a.version != plwire::PROTOCOL_VERSION) continue;
        std::string dest = hexOf(buf + 1, 16);
        if (!s_heard.count(dest) && s_heard.size() >= PL_HEARD_MAX) {
            auto old = std::min_element(s_heard.begin(), s_heard.end(),
                                        [](auto& x, auto& y) { return x.second < y.second; });
            storageDeleteTree(("partyline.heard." + old->first).c_str());
            s_heard.erase(old);
        }
        s_heard[dest] = monoS();
        std::string b = "partyline.heard." + dest + ".";
        storageBegin();
        storageSet((b + "name").c_str(), a.name.c_str());
        storageSet((b + "flags").c_str(), (int)a.flags);
        storageSet((b + "desc").c_str(), a.description.c_str());
        storageSet((b + "lang").c_str(), a.language.c_str());
        storageSet((b + "country").c_str(), a.country.c_str());
        storageSet((b + "hops").c_str(), (int)buf[0]);
        storageSet((b + "seen").c_str(), monoS());
        storageEnd();
    }
}

static void onAnnounceDisc(int) { s_annHandle = -1; }

static void connectAnnounces()
{
    if (s_annHandle >= 0) return;
    rnsd_announces_connect_t req = {};
    safeStrncpy(req.aspect, PL_ASPECT, sizeof req.aspect);
    int h = itsConnect("rnsd", RNSD_PORT_ANNOUNCES, &req, sizeof req, pdMS_TO_TICKS(2000), 0,
                       onAnnounce, onAnnounceDisc);
    if (h >= 0) s_annHandle = h;
}

/* ─────────────── commands ─────────────── */

static int s_ack = 0;

static void cmdDone(const char* verb)
{
    storageBegin();
    storageSet((std::string("partyline.cmd.") + verb + ".error").c_str(), "");
    storageSet((std::string("partyline.cmd.") + verb + ".done").c_str(), ++s_ack);
    storageEnd();
}

static void cmdError(const char* verb, const std::string& why)
{
    storageSet((std::string("partyline.cmd.") + verb + ".error").c_str(), why.c_str());
}

static std::string jStr(const cJSON* o, const char* k)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    if (cJSON_IsNumber(v)) return itos((int64_t)v->valuedouble);
    return "";
}

static bool jHas(const cJSON* o, const char* k) { return cJSON_GetObjectItemCaseSensitive(o, k) != nullptr; }

/** An integer field given as a number or digits; false when absent or not one. */
static bool jInt(const cJSON* o, const char* k, int64_t& out)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) { out = (int64_t)v->valuedouble; return true; }
    if (cJSON_IsBool(v)) { out = cJSON_IsTrue(v) ? 1 : 0; return true; }
    if (cJSON_IsString(v) && v->valuestring && *v->valuestring) {
        char* end = nullptr;
        long long n = std::strtoll(v->valuestring, &end, 10);
        if (end && !*end) { out = n; return true; }
    }
    return false;
}

/** The room with this id or name (case-insensitive), or -1. */
static int64_t roomFind(const std::string& s)
{
    std::string t = trim(s);
    char* end = nullptr;
    long long n = std::strtoll(t.c_str(), &end, 10);
    if (!t.empty() && end && !*end && s_rooms.count(n)) return n;
    for (auto& [id, c] : s_rooms) if (lower(c.name) == lower(t)) return id;
    return -1;
}

static void sendMove(int64_t id, bool hasPw, const std::string& pw)
{
    s_s.movePending = true;
    s_s.moveRoom = id;
    s_s.moveHasPw = hasPw;
    s_s.movePw = pw;
    linkSend(plwire::encodeMove(id, hasPw ? &pw : nullptr));
}

static void cmdAdd(const std::string& v)
{
    std::string d = destOf(v);
    if (d.empty()) { cmdError("add", "That is not a server address (32 hex characters)."); return; }
    serverSave(d);
    serverPwPublish(d);
    cmdDone("add");
}

static void cmdForget(const std::string& v)
{
    std::string d = destOf(v);
    if (d.empty()) { cmdError("forget", "That is not a server address."); return; }
    if (s_s.dest == d) {
        sessionEnd(true);
        s_retry = Retry{};
        publishState("idle", "Not connected");
    }
    storageDeleteTree(("s.partyline.servers." + d).c_str());
    storageDeleteTree(("secrets.partyline.servers." + d).c_str());
    storageDeleteTree(("secrets.partyline.rooms." + d).c_str());
    storageDeleteTree(("partyline.servers." + d).c_str());
    cmdDone("forget");
}

static void cmdConnect(const std::string& v)
{
    cJSON* o = cJSON_Parse(v.c_str());
    std::string d = o && cJSON_IsObject(o) ? destOf(jStr(o, "server")) : destOf(v);
    if (d.empty()) {
        cJSON_Delete(o);
        cmdError("connect", "That is not a server address (32 hex characters).");
        return;
    }
    std::string ident = o ? trim(jStr(o, "ident")) : "";
    if (!ident.empty() && !identValid(ident)) {
        cJSON_Delete(o);
        cmdError("connect", "That identity is not on this device.");
        return;
    }
    if (ident.empty()) {
        std::string saved = storageGetStr(srvKey(d, "ident").c_str(), "");
        ident = identValid(saved) ? saved : identDefault();
    }
    if (ident.empty()) {
        cJSON_Delete(o);
        cmdError("connect", "Choose who to connect as: this device has several LXMF identities.");
        return;
    }
    serverSave(d);
    storageSet(srvKey(d, "ident").c_str(), ident.c_str());
    bool roomGiven = o && jHas(o, "room");
    std::string room = roomGiven ? plwire::cleanText(jStr(o, "room"), plwire::MAX_NAME) : "";
    if (roomGiven) storageSet(srvKey(d, "room").c_str(), room.c_str());
    std::string wantRoom = roomGiven ? room : storageGetStr(srvKey(d, "room").c_str(), "");
    std::string pw = o ? jStr(o, "password") : "";
    std::string spw = o ? jStr(o, "server_password") : "";
    cJSON_Delete(o);
    if (!pw.empty() && !wantRoom.empty()) storageSet(roomPwKey(d, wantRoom).c_str(), pw.c_str());
    if (!spw.empty()) storageSet(srvPwKey(d).c_str(), spw.c_str());
    serverPwPublish(d, spw.empty() ? -1 : 1);

    bool active = s_s.ph == PH_LINKING || s_s.ph == PH_HELLO || s_s.ph == PH_JOINED;
    if (active && s_s.dest == d && s_s.ident == ident) {
        /* Already there as that identity: a room asked for is a move. */
        if (roomGiven && !room.empty() && s_s.ph == PH_JOINED && s_s.synced) {
            int64_t id = roomFind(room);
            if (id < 0) { cmdError("connect", "There is no room " + room + " on this server."); return; }
            if (!s_s.inRoom || id != s_s.room) {
                std::string k = roomPwKey(d, roomName(id));
                bool has = !pw.empty() || storageExists(k.c_str());
                sendMove(id, has, pw.empty() ? storageGetStr(k.c_str(), "") : pw);
            }
        }
        cmdDone("connect");
        return;
    }
    if (s_s.ph != PH_IDLE) sessionEnd(true);
    s_retry = Retry{};
    if (!sessionOpen(d, ident) && s_s.ph == PH_IDLE) {
        cmdError("connect", "That identity is not available.");
        return;
    }
    cmdDone("connect");
}

static void cmdDisconnect()
{
    bool held = s_s.ph != PH_IDLE;
    s_retry = Retry{};
    sessionEnd(true);
    publishState("idle", "Not connected");
    if (held) logSystem("Disconnected");
    cmdDone("disconnect");
}

static bool joined(const char* verb)
{
    if (s_s.ph == PH_JOINED && s_s.synced) return true;
    cmdError(verb, "Not connected to a server.");
    return false;
}

static void cmdMove(const std::string& v)
{
    if (!joined("move")) return;
    cJSON* o = cJSON_Parse(v.c_str());
    if (!o || !cJSON_IsObject(o)) {
        cJSON_Delete(o);
        cmdError("move", "Which room?");
        return;
    }
    int64_t id = roomFind(jStr(o, "room"));
    bool pwGiven = jHas(o, "password") && !jStr(o, "password").empty();
    std::string pw = jStr(o, "password");
    cJSON_Delete(o);
    if (id < 0) { cmdError("move", "There is no such room on this server."); return; }
    if (s_s.inRoom && id == s_s.room) { cmdDone("move"); return; }   /* the server would not answer */
    if (!pwGiven) {
        std::string k = roomPwKey(s_s.dest, roomName(id));
        if (storageExists(k.c_str())) { pwGiven = true; pw = storageGetStr(k.c_str(), ""); }
        sendMove(id, pwGiven, pw);
        s_s.moveHasPw = false;   /* already saved */
    } else {
        sendMove(id, true, pw);
    }
    cmdDone("move");
}

static void cmdText(const std::string& v)
{
    if (!joined("text")) return;
    if (!s_s.inRoom) { cmdError("text", "You are in no room."); return; }
    std::string t = plwire::cleanText(v, plwire::MAX_TEXT);
    if (t.empty()) { cmdError("text", "Nothing to send."); return; }
    if (!linkSend(plwire::encodeText(t))) { cmdError("text", "The link is busy; try again."); return; }
    logLine("text", s_s.sid, s_s.selfName.empty() ? nickFor(s_s.ident) : s_s.selfName, t);
    cmdDone("text");
}

static void cmdPoke(const std::string& v)
{
    if (!joined("poke")) return;
    cJSON* o = cJSON_Parse(v.c_str());
    int64_t mid = -1;
    std::string text;
    if (o && cJSON_IsObject(o)) {
        jInt(o, "mid", mid);
        text = jStr(o, "text");
    }
    cJSON_Delete(o);
    if (!s_members.count(mid)) { cmdError("poke", "That member is not on the server."); return; }
    std::string t = plwire::cleanText(text, plwire::MAX_TEXT);
    if (!linkSend(plwire::encodePoke(mid, t))) { cmdError("poke", "The link is busy; try again."); return; }
    logSystem("You poked " + memberName(mid) + (t.empty() ? "" : ": " + t));
    cmdDone("poke");
}

static void cmdState(const std::string& v)
{
    cJSON* o = cJSON_Parse(v.c_str());
    if (!o || !cJSON_IsObject(o)) {
        cJSON_Delete(o);
        cmdError("state", "Expected {\"muted\", \"deaf\"}.");
        return;
    }
    int64_t mu, de;
    bool hasMu = jInt(o, "muted", mu), hasDe = jInt(o, "deaf", de);
    cJSON_Delete(o);
    storageBegin();
    if (hasMu) storageSet("s.partyline.muted", mu ? 1 : 0);
    if (hasDe) storageSet("s.partyline.deaf", de ? 1 : 0);
    storageEnd();
    bool muted = hasMu ? mu != 0 : storageGetInt("s.partyline.muted", 0) != 0;
    bool deaf = hasDe ? de != 0 : storageGetInt("s.partyline.deaf", 0) != 0;
    if (s_s.ph == PH_JOINED && (s_s.textOnly || s_s.voiceOff)) {
        cmdError("state", "Joined without audio: the server keeps you muted and deafened.");
        return;
    }
    if (s_s.ph == PH_JOINED) {
        if (!s_lxHeld) linkSend(plwire::encodeState(muted, deaf));   /* held: sent at resume */
        if (s_lx >= 0) {
            int packed = (speakNow() ? 1 : 0) | (muted ? 2 : 0) | (deaf ? 4 : 0);
            if (packed != s_lxState) {
                uint8_t m[4] = { LXST_OP_STATE, (uint8_t)(speakNow() ? 1 : 0), (uint8_t)muted, (uint8_t)deaf };
                if (itsSend(s_lx, m, sizeof m, 0) > 0) s_lxState = packed;
            }
        }
    }
    cmdDone("state");
}

static void cmdNick(const std::string& v)
{
    storageSet("s.partyline.nick", plwire::cleanText(v, plwire::MAX_NAME).c_str());
    cmdDone("nick");
}

static void cmdAuto(const std::string& v)
{
    cJSON* o = cJSON_Parse(v.c_str());
    std::string d;
    int64_t on = 0;
    bool ok = o && cJSON_IsObject(o) && !(d = destOf(jStr(o, "server"))).empty() && jInt(o, "auto", on);
    cJSON_Delete(o);
    if (!ok) { cmdError("auto", "Expected {\"server\", \"auto\": 0|1}."); return; }
    serverSave(d);
    if (on) {
        /* One server at a time: autoconnect goes to one. */
        std::vector<std::string> others;
        static std::vector<std::string>* acc;
        acc = &others;
        storageForEach("s.partyline.servers.", [](const char* key, const char* val) {
            const char* tail = key + sizeof("s.partyline.servers.") - 1;
            const char* dot = std::strchr(tail, '.');
            if (dot && !std::strcmp(dot + 1, "auto") && val && std::atoi(val)) acc->emplace_back(tail, dot - tail);
        });
        for (auto& x : others) if (x != d) storageSet(srvKey(x, "auto").c_str(), 0);
    }
    storageSet(srvKey(d, "auto").c_str(), on ? 1 : 0);
    cmdDone("auto");
}

/* Commands are consumed: read, unset, act, then answer on <cmd>.error or
 * <cmd>.done. Only the command keys themselves are acted on; their answer
 * keys beneath them come through this subscription too and are ignored. */
static void onCmd(const char* key, const char* val)
{
    static const char P[] = "partyline.cmd.";
    if (!key || std::strncmp(key, P, sizeof P - 1)) return;
    const char* verb = key + sizeof P - 1;
    if (std::strchr(verb, '.') || !val || !*val) return;
    std::string v = storageGetStr(key, "");   /* val is cut short; the stored value is whole */
    if (v.empty()) return;
    storageUnset(key);
    if (!std::strcmp(verb, "add")) cmdAdd(v);
    else if (!std::strcmp(verb, "forget")) cmdForget(v);
    else if (!std::strcmp(verb, "connect")) cmdConnect(v);
    else if (!std::strcmp(verb, "disconnect")) cmdDisconnect();
    else if (!std::strcmp(verb, "move")) cmdMove(v);
    else if (!std::strcmp(verb, "text")) cmdText(v);
    else if (!std::strcmp(verb, "poke")) cmdPoke(v);
    else if (!std::strcmp(verb, "state")) cmdState(v);
    else if (!std::strcmp(verb, "nick")) cmdNick(v);
    else if (!std::strcmp(verb, "auto")) cmdAuto(v);
}

/* ─────────────── tick ─────────────── */

/* Our own audio stays within what the server link's first hop carries:
 * the room's talk rate (plwire::talkBps, from its profile, frame length,
 * push-to-talk batching and the link's MDU) must fit the hop
 * (rnsd.links.<tag>.bitrate, talkFitsHop), and over a LoRa first hop with an
 * hourly airtime budget (lora.<n>.airtime_{hour,limit}_ms, limit 0 = none) the
 * budget must not be spent (plwire::airtimeBlocks: a reserve below the limit,
 * and once blocked 60 s of budget free before speaking again). Otherwise the
 * voice session is told it may not speak and the reason is shown; it speaks
 * again once both hold. Receiving is the server's sending and is left alone.
 * Runs every tick. */
static const char AIR_BLOCK[] = "Airtime limit reached";
static void hopCheck()
{
    const char* why = "";
    uint32_t bps = 0, hop = 0;
    if (s_s.ph == PH_JOINED && s_s.inRoom && s_s.voiceProf && !s_s.tag.empty()) {
        std::string lk = "rnsd.links." + s_s.tag + ".";
        int b = storageGetInt((lk + "bitrate").c_str(), 0);
        hop = b > 0 ? (uint32_t)b : 0;
        int mdu = storageGetInt((lk + "mdu").c_str(), 0);
        if (mdu <= 0) mdu = (int)plwire::DEFAULT_MDU;
        auto it = s_rooms.find(s_s.room);
        uint32_t jitter = it != s_rooms.end() ? it->second.ptt_jitter_ms : 0;
        uint32_t fms = s_s.voiceFrameMs > 0 ? (uint32_t)s_s.voiceFrameMs : s_s.voiceProf->frame_ms;
        bps = plwire::talkBps((size_t)mdu, plwire::batchMaxFrames(jitter, fms), s_s.voiceProf->max_bytes, fms);
        if (!plwire::talkFitsHop(bps, hop)) why = "Route too slow to talk in this room";
        std::string iface = storageGetStr((lk + "iface").c_str(), "");
        if (!*why && iface.rfind("lora/", 0) == 0) {
            int n = atoi(iface.c_str() + 5);
            char k[48];
            snprintf(k, sizeof k, "lora.%d.airtime_limit_ms", n);
            int lim = storageGetInt(k, 0);
            snprintf(k, sizeof k, "lora.%d.airtime_hour_ms", n);
            int used = storageGetInt(k, 0);
            bool was = std::strcmp(s_txBlock, AIR_BLOCK) == 0;
            if (plwire::airtimeBlocks((uint32_t)std::max(0, used), (uint32_t)std::max(0, lim), was)) why = AIR_BLOCK;
        }
    }
    if (std::strcmp(why, s_txBlock) == 0) return;
    if (*why) {
        warn("partyline: not transmitting: %s (talk %u bit/s, first hop %u bit/s)", why, (unsigned)bps, (unsigned)hop);
        logSystem(why);
    } else if (s_s.ph == PH_JOINED) {
        info("partyline: first hop carries the room again (talk %u bit/s, first hop %u bit/s)", (unsigned)bps, (unsigned)hop);
        logSystem("You can talk again");
    }
    s_txBlock = why;
    storageSet("partyline.tx_blocked", why);
    lxSendState();
}

static void tick()
{
    int64_t now = nowMs();
    hopCheck();
    lxSendState(); /* resends a state the voice session's queue refused */
    if (s_s.ph == PH_HELLO && now >= s_s.welcomeBy) {
        warn("partyline %s: no WELCOME in time", s_s.dest.c_str());
        sessionEnd(false);
        scheduleRetry();
    }
    if (s_retry.armed && s_s.ph == PH_LOST) {
        if (now >= s_retry.at) {
            s_retry.armed = false;
            std::string d = s_s.dest, id = s_s.ident;
            sessionOpen(d, id);
        } else {
            int left = (int)((s_retry.at - now + 999) / 1000);
            if (left != s_retry.shown && (left % 5 == 0 || left < 5)) {
                s_retry.shown = left;
                storageSet("partyline.status", ("Connection lost; retrying in " + std::to_string(left) + " s").c_str());
            }
        }
    }
    lxReopen(now);
    if (s_bootAgainAt && now >= s_bootAgainAt) bootServers();
    publishStats();
}

/* Saved servers: publish which have a saved password, and connect the one
 * set to autoconnect. While lxmf is still loading identities in the first
 * minute, the choice waits for them and this runs again from the tick. */
static void bootServers()
{
    int64_t now = nowMs();
    s_bootAgainAt = 0;
    if (now - s_taskStartAt < PL_IDENT_WAIT_MS && lxmfLoading()) {
        s_bootAgainAt = now + PL_IDENT_RECHECK_MS;
        return;
    }
    static std::vector<std::string>* acc;
    std::vector<std::string> all;
    acc = &all;
    storageForEach("s.partyline.servers.", [](const char* key, const char*) {
        const char* tail = key + sizeof("s.partyline.servers.") - 1;
        const char* dot = std::strchr(tail, '.');
        if (dot && !std::strcmp(dot + 1, "auto")) acc->emplace_back(tail, dot - tail);
    });
    std::string autoDest;
    for (auto& d : all) {
        serverPwPublish(d);
        if (autoDest.empty() && storageGetInt(srvKey(d, "auto").c_str(), 0)) autoDest = d;
    }
    if (autoDest.empty() || s_s.ph != PH_IDLE) return;
    std::string ident = storageGetStr(srvKey(autoDest, "ident").c_str(), "");
    if (!identValid(ident)) ident = identDefault();
    if (ident.empty()) {
        storageSet("partyline.server", autoDest.c_str());
        publishState("idle", "Not connected: choose who to connect as");
        return;
    }
    sessionOpen(autoDest, ident);
}

/* ─────────────── CLI ─────────────── */

/* The CLI runs on the console task: it reads storage and writes command keys,
 * never the task's own state. */

static std::vector<std::pair<std::string, std::string>>* s_cliAcc = nullptr;
static void cliLeaf(const char* key, const char* val)
{
    if (s_cliAcc && key) s_cliAcc->emplace_back(key, val ? val : "");
}

/** Records under `prefix` (<id>.<field>), by id. */
static std::map<std::string, std::map<std::string, std::string>> cliRecords(const char* prefix)
{
    std::vector<std::pair<std::string, std::string>> leaves;
    s_cliAcc = &leaves;
    storageForEach(prefix, cliLeaf);
    s_cliAcc = nullptr;
    std::map<std::string, std::map<std::string, std::string>> out;
    size_t pl = std::strlen(prefix);
    for (auto& [k, v] : leaves) {
        if (k.size() <= pl) continue;
        std::string tail = k.substr(pl);
        size_t dot = tail.find('.');
        if (dot == std::string::npos) continue;
        out[tail.substr(0, dot)][tail.substr(dot + 1)] = v;
    }
    return out;
}

static void cliCmd(const char* verb, const std::string& val)
{
    storageSet((std::string("partyline.cmd.") + verb).c_str(), val.c_str());
    cliPrintf("sent\n");
}

static std::string cliJson(cJSON* o)
{
    char* s = cJSON_PrintUnformatted(o);
    std::string r = s ? s : "";
    cJSON_free(s);
    cJSON_Delete(o);
    return r;
}

static void cliPartyline(const char* args)
{
    if (args && !std::strcmp(args, "help")) {
        cliPrintf("%-*s Partyline voice rooms: connect, rooms, say\n", CLI_HELP_COL, "partyline [...]");
        return;
    }
    if (args && cliWantsHelp(args)) {
        cliPrintf("partyline [status]                 server, room, voice\n");
        cliPrintf("partyline servers                  saved and heard\n");
        cliPrintf("partyline connect <hash> [room] [lxmf<n>|node]\n");
        cliPrintf("partyline disconnect\n");
        cliPrintf("partyline rooms | members\n");
        cliPrintf("partyline move <id|name> [password]\n");
        cliPrintf("partyline say <text> | poke <mid> <text>\n");
        cliPrintf("partyline mute | unmute | deaf | undeaf\n");
        cliPrintf("partyline log [n]\n");
        return;
    }
    std::string a = args ? trim(args) : "";
    std::string verb = a.substr(0, a.find(' '));
    std::string rest = a.find(' ') == std::string::npos ? "" : trim(a.substr(a.find(' ') + 1));
    auto first = [](std::string& s) {
        std::string f = s.substr(0, s.find(' '));
        s = s.find(' ') == std::string::npos ? "" : trim(s.substr(s.find(' ') + 1));
        return f;
    };

    if (verb.empty() || verb == "status") {
        std::string d = storageGetStr("partyline.server", "");
        cliPrintf("%s: %s\n", storageGetStr("partyline.state", "idle").c_str(),
                  storageGetStr("partyline.status", "").c_str());
        if (d.empty()) return;
        cliPrintf("server %s \"%s\"  member %s\n", d.c_str(), storageGetStr("partyline.name", "").c_str(),
                  storageGetStr("partyline.sid", "").c_str());
        std::string room = storageGetStr("partyline.room", "");
        if (room.empty()) { cliPrintf("in no room\n"); return; }
        std::string rb = "partyline.rooms." + room + ".";
        cliPrintf("room %s \"%s\"  %d members  can_speak %d  audio %d\n", room.c_str(),
                  storageGetStr((rb + "name").c_str(), "").c_str(), storageGetInt((rb + "members").c_str(), 0),
                  storageGetInt("partyline.can_speak", 0), storageGetInt("partyline.audio", 0));
        std::string tag;
        for (int i = 0; i < PL_TAGS && tag.empty(); ++i) {
            std::string t = PL_TAG_PFX + std::to_string(i);
            if (storageGetStr(("rnsd.links." + t + ".state").c_str(), "") == "active") tag = t;
        }
        cliPrintf("profile %s  MDU %d  tx_drop %d\n", storageGetStr((rb + "profile_text").c_str(), "").c_str(),
                  tag.empty() ? 0 : storageGetInt(("rnsd.links." + tag + ".mdu").c_str(), 0),
                  storageGetInt("partyline.stats.tx_drop", 0));
        return;
    }
    if (verb == "servers") {
        auto saved = cliRecords("s.partyline.servers.");
        for (auto& [d, f] : saved)
            cliPrintf("%s %s %-20s %s\n", d.c_str(), f["auto"] == "1" ? "auto" : "    ", f["name"].c_str(),
                      f["ident"].c_str());
        for (auto& [d, f] : cliRecords("partyline.heard.")) {
            if (saved.count(d)) continue;
            cliPrintf("%s heard %-20s %s hops\n", d.c_str(), f["name"].empty() ? "(unnamed)" : f["name"].c_str(),
                      f["hops"].c_str());
        }
        return;
    }
    if (verb == "connect") {
        std::string d = destOf(first(rest));
        if (d.empty()) { cliPrintf("usage: partyline connect <hash> [room] [lxmf<n>|node]\n"); return; }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "server", d.c_str());
        std::string room = first(rest), id = first(rest);
        if (room == "node" || (room.rfind("lxmf", 0) == 0 && room.size() > 4 && std::isdigit((unsigned char)room[4]))) {
            id = room;
            room.clear();
        }
        if (!room.empty()) cJSON_AddStringToObject(o, "room", room.c_str());
        if (id == "node") cJSON_AddStringToObject(o, "ident", PL_NODE_IDENT);
        else if (id.rfind("lxmf", 0) == 0) cJSON_AddStringToObject(o, "ident", ("secrets.lxmf.id." + id.substr(4) + ".privkey").c_str());
        else if (!id.empty()) cJSON_AddStringToObject(o, "ident", id.c_str());
        cliCmd("connect", cliJson(o));
        return;
    }
    if (verb == "disconnect") { cliCmd("disconnect", "1"); return; }
    if (verb == "rooms") {
        std::string cur = storageGetStr("partyline.room", "");
        for (auto& [id, f] : cliRecords("partyline.rooms."))
            cliPrintf("%s%-4s %-20s %2s  %s\n", id == cur ? "*" : " ", id.c_str(), f["name"].c_str(),
                      f["members"].c_str(), f["profile_text"].c_str());
        return;
    }
    if (verb == "members") {
        for (auto& [mid, f] : cliRecords("partyline.members.")) {
            std::string fl;
            if (f["self"] == "1") fl += " you";
            if (f["op"] == "1") fl += " op";
            if (f["muted"] == "1") fl += " muted";
            if (f["deaf"] == "1") fl += " deaf";
            if (f["text_only"] == "1") fl += " text";
            if (f["speaking"] == "1") fl += " speaking";
            cliPrintf("%-4s %-20s room %-4s%s\n", mid.c_str(), f["name"].c_str(), f["room"].c_str(), fl.c_str());
        }
        return;
    }
    if (verb == "move") {
        if (rest.empty()) { cliPrintf("usage: partyline move <id|name> [password]\n"); return; }
        /* the whole rest when it names a room, else a room then a password */
        std::string room = rest, pw;
        bool named = false;
        for (auto& [id, f] : cliRecords("partyline.rooms."))
            if (id == rest || lower(f["name"]) == lower(rest)) { room = id; named = true; }
        if (!named) { room = first(rest); pw = rest; }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "room", room.c_str());
        if (!pw.empty()) cJSON_AddStringToObject(o, "password", pw.c_str());
        cliCmd("move", cliJson(o));
        return;
    }
    if (verb == "say") {
        if (rest.empty()) { cliPrintf("usage: partyline say <text>\n"); return; }
        cliCmd("text", rest);
        return;
    }
    if (verb == "poke") {
        std::string mid = first(rest);
        if (mid.empty()) { cliPrintf("usage: partyline poke <mid> <text>\n"); return; }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "mid", std::atof(mid.c_str()));
        cJSON_AddStringToObject(o, "text", rest.c_str());
        cliCmd("poke", cliJson(o));
        return;
    }
    if (verb == "mute" || verb == "unmute") { cliCmd("state", verb == "mute" ? "{\"muted\":1}" : "{\"muted\":0}"); return; }
    if (verb == "deaf" || verb == "undeaf") { cliCmd("state", verb == "deaf" ? "{\"deaf\":1}" : "{\"deaf\":0}"); return; }
    if (verb == "log") {
        int n = rest.empty() ? 10 : std::max(1, std::atoi(rest.c_str()));
        int next = storageGetInt("partyline.log_next", 0);
        for (int i = std::max(0, next - n); i < next; ++i) {
            std::string b = "partyline.log." + std::to_string(i) + ".";
            if (!storageExists((b + "kind").c_str())) continue;
            std::string kind = storageGetStr((b + "kind").c_str(), ""), name = storageGetStr((b + "name").c_str(), "");
            std::string text = storageGetStr((b + "text").c_str(), "");
            if (kind == "text") cliPrintf("<%s> %s\n", name.c_str(), text.c_str());
            else if (kind == "poke") cliPrintf("poke from %s: %s\n", name.c_str(), text.c_str());
            else cliPrintf("-- %s\n", text.c_str());
        }
        return;
    }
    cliPrintf("unknown subcommand. try `partyline -h`\n");
}

/* ─────────────── task ─────────────── */

static TaskHandle_t  s_task = nullptr;
static volatile bool s_stop = false, s_parked = false;

static void publishIdle()
{
    clearRoster();
    storageBegin();
    storageSet("partyline.server", "");
    storageSet("partyline.state", "idle");
    storageSet("partyline.status", "Not connected");
    storageSet("partyline.name", "");
    storageSet("partyline.motd", "");
    storageSet("partyline.sid", "");
    storageSet("partyline.room", "");
    storageSet("partyline.can_speak", 0);
    storageSet("partyline.audio", 0);
    storageSet("partyline.held", 0);
    storageEnd();
}

static void partylineTaskMain(void*)
{
    info("task up");
    itsClientInit(6);
    s_linkBuf = (uint8_t*)heap_caps_malloc(PL_LINK_BUF, MALLOC_CAP_SPIRAM);
    if (!s_linkBuf) { err("partyline: no memory for the link buffer"); killSelf(); }

    storageSubscribeChanges("partyline.cmd.", onCmd);
    storageSubscribeChanges("rnsd.links." PL_TAG_PFX, onLinkState);
    storageSet("partyline.log_next", 0);

    for (;;) {
        publishIdle();
        connectAnnounces();
        s_taskStartAt = nowMs();
        bootServers();
        while (!s_stop) {
            itsPoll(pdMS_TO_TICKS(PL_TICK_MS));
            tick();
            if (s_annHandle < 0) connectAnnounces();
        }
        sessionEnd(true);
        s_retry = Retry{};
        publishIdle();
        if (s_annHandle >= 0) { itsDisconnect(s_annHandle); s_annHandle = -1; }
        s_parked = true;
        info("stopped");
        while (s_stop) itsPoll(portMAX_DELAY);
        s_parked = false;
    }
}

static void partylineStart()
{
    s_stop = false;
    if (!s_task) s_task = spawnTask(partylineTaskMain, TAG, 8192, nullptr, 1, 1, STACK_PSRAM);
    else xTaskNotifyGive(s_task);
}

static void partylineStop()
{
    if (!s_task || s_stop) return;
    s_stop = true;
    xTaskNotifyGive(s_task);
    for (int i = 0; i < 300 && !s_parked; i++) delay(10);
    if (!s_parked) warn("[%s] stop timed out", TAG);
}

void PartylineService::onInit()
{
    storageBegin();
    storageDefault("s.partyline.nick", "");
    storageDefault("s.partyline.text_only", 0);
    storageDefault("s.partyline.muted", 0);
    storageDefault("s.partyline.deaf", 0);
    storageEnd();

    cliRegisterCmd("partyline", cliPartyline);
    rnsServiceRegister(TAG, partylineStart, partylineStop);
}
