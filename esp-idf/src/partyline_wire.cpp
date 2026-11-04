#include "partyline_wire.h"

#include <cstring>

namespace plwire {

using lxstmp::Value;
using lxstmp::Writer;

const Profile PROFILES[] = {
    { "opus-high",  LXST_CODEC_OPUS,   LXST_OPUS_VOICE_HIGH,   20,  41 },
    { "opus-med",   LXST_CODEC_OPUS,   LXST_OPUS_VOICE_MEDIUM, 60,  61 },
    { "opus-low",   LXST_CODEC_OPUS,   LXST_OPUS_VOICE_LOW,    60,  46 },
    { "c2-3200",    LXST_CODEC_CODEC2, LXST_C2_3200,           200, MAX_FRAME_BYTES },
    { "c2-2400",    LXST_CODEC_CODEC2, LXST_C2_2400,           200, MAX_FRAME_BYTES },
    { "c2-1200",    LXST_CODEC_CODEC2, LXST_C2_1200,           400, MAX_FRAME_BYTES },
    { "c2-700",     LXST_CODEC_CODEC2, LXST_C2_700C,           400, MAX_FRAME_BYTES },
    { "music-low",  LXST_CODEC_OPUS,   LXST_OPUS_AUDIO_LOW,    40,  71 },
    { "music-med",  LXST_CODEC_OPUS,   LXST_OPUS_AUDIO_MEDIUM, 40,  141 },
    { "music-high", LXST_CODEC_OPUS,   LXST_OPUS_AUDIO_HIGH,   40,  281 },
};
const size_t PROFILE_COUNT = sizeof PROFILES / sizeof PROFILES[0];

const Profile* profileFind(const char* name, size_t len)
{
    for (size_t k = 0; k < PROFILE_COUNT; ++k)
        if (std::strlen(PROFILES[k].name) == len && std::memcmp(PROFILES[k].name, name, len) == 0)
            return &PROFILES[k];
    return nullptr;
}

uint32_t batchMaxFrames(uint32_t ptt_jitter_ms, uint32_t frame_ms)
{
    if (ptt_jitter_ms == 0 || frame_ms == 0) return 1;
    uint32_t n = ptt_jitter_ms / frame_ms;
    if (n < 1) n = 1;
    return n > MAX_BATCH ? MAX_BATCH : n;
}

uint32_t talkBps(size_t mdu, uint32_t maxFrames, size_t frameBytes, uint32_t frameMs)
{
    size_t budget = batchBudget(mdu);
    if (!frameMs || !frameBytes || BATCH_BASE_COST + frameBytes + BATCH_FRAME_COST > budget) return 0;
    uint32_t n = (uint32_t)((budget - BATCH_BASE_COST) / (frameBytes + BATCH_FRAME_COST));
    if (maxFrames && n > maxFrames) n = maxFrames;
    if (n < 1) n = 1;
    size_t payload = BATCH_BASE_COST + n * (frameBytes + BATCH_FRAME_COST);
    size_t packet = 19 + 48 + (payload / 16 + 1) * 16;
    uint64_t ms = (uint64_t)n * frameMs;
    return (uint32_t)(((uint64_t)packet * 8 * 1000 + ms - 1) / ms);
}

/* ── clean_text ── */

namespace {

/* The characters partyline's _unprintable pattern removes. */
bool unprintable(uint32_t c)
{
    return c <= 0x1f || (c >= 0x7f && c <= 0x9f) || c == 0xa0 || c == 0xad || c == 0x61c ||
           c == 0x115f || c == 0x1160 || c == 0x180e || c == 0x2007 || c == 0x200b ||
           c == 0x200e || c == 0x200f || (c >= 0x202a && c <= 0x202f) ||
           (c >= 0x2060 && c <= 0x206f) || c == 0x2800 || c == 0x3164 || c == 0xfeff ||
           c == 0xffa0;
}

/* Python str.split() whitespace that survives the removal above. */
bool space(uint32_t c)
{
    return c == 0x20 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
           c == 0x2029 || c == 0x205f || c == 0x3000;
}

/* Length of the valid UTF-8 sequence at p[0..n), 0 if invalid. */
size_t utf8Seq(const uint8_t* p, size_t n, uint32_t& cp)
{
    uint8_t c = p[0];
    if (c < 0x80) { cp = c; return 1; }
    size_t more;
    uint32_t min;
    if ((c & 0xe0) == 0xc0)      { more = 1; cp = c & 0x1f; min = 0x80; }
    else if ((c & 0xf0) == 0xe0) { more = 2; cp = c & 0x0f; min = 0x800; }
    else if ((c & 0xf8) == 0xf0) { more = 3; cp = c & 0x07; min = 0x10000; }
    else return 0;
    if (n <= more) return 0;
    for (size_t k = 1; k <= more; ++k) {
        if ((p[k] & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (p[k] & 0x3f);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    return more + 1;
}

}  // namespace

std::string cleanText(const char* s, size_t n, size_t limit)
{
    const uint8_t* p = (const uint8_t*)s;
    std::string out;
    out.reserve(n < limit ? n : limit + 4);
    bool pending_space = false;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        size_t len = utf8Seq(p + i, n - i, cp);
        if (len == 0) { i++; continue; }
        if (!unprintable(cp)) {
            if (space(cp)) {
                pending_space = !out.empty();
            } else {
                if (pending_space) out.push_back(' ');
                pending_space = false;
                out.append(s + i, len);
            }
        }
        i += len;
    }
    if (out.size() > limit) {
        /* drop a character the cut splits */
        size_t k = limit;
        while (k > 0 && ((uint8_t)out[k - 1] & 0xc0) == 0x80) k--;
        if (k > 0) {
            uint8_t lead = (uint8_t)out[k - 1];
            size_t need = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : 4;
            out.resize(k - 1 + need <= limit ? limit : k - 1);
        } else {
            out.resize(0);
        }
    }
    return out;
}

/* ── decoding ── */

namespace {

std::string cleaned(const Value* v, size_t limit)
{
    if (!v || !v->isStr()) return std::string();
    return cleanText((const char*)v->data, v->len, limit);
}

bool intOf(const Value* v, int64_t& out)
{
    return v && v->getInt(out);
}

bool decodeChannel(const Value& r, Channel& c)
{
    if (!r.isArray() || r.size() < 6) return false;
    int64_t access;
    if (!intOf(r.item(0), c.id) || !intOf(r.item(3), c.frame_ms) || !intOf(r.item(4), access)) return false;
    c.access = (uint32_t)access;
    if (r.item(2)->isStr()) {
        c.profile_name = r.item(2)->str();
        c.profile = profileFind(c.profile_name);
    }
    c.name = cleaned(r.item(1), MAX_NAME);
    if (c.name.empty()) c.name = "Room " + std::to_string(c.id);
    c.description = cleaned(r.item(5), MAX_DESCRIPTION);
    const Value* d = r.item(6);
    if (d && d->isStr() && d->len == 32) c.dialin = d->str();
    int64_t v;
    if (intOf(r.item(7), v)) c.ptt_jitter_ms = (uint32_t)(v < 0 ? 0 : v > MAX_JITTER_MS ? MAX_JITTER_MS : v);
    if (intOf(r.item(8), v)) c.flags = (uint32_t)v;
    return true;
}

bool decodeUser(const Value& r, User& u)
{
    if (!r.isArray() || r.size() < 6) return false;
    if (!intOf(r.item(0), u.id)) return false;
    u.name = cleaned(r.item(1), MAX_NAME);
    if (u.name.empty()) u.name = "guest-" + std::to_string(u.id);
    const Value* id = r.item(2);
    if (id->isStr() && id->len == 32) u.identity = id->str();
    u.in_room = intOf(r.item(3), u.room);
    u.muted = r.item(4)->truthy();
    u.deaf = r.item(5)->truthy();
    int64_t v;
    if (intOf(r.item(6), v)) u.hops = (int32_t)v;
    double rtt;
    if (r.item(7) && r.item(7)->getNumber(rtt)) u.rtt_ms = (int64_t)rtt;
    u.op = r.item(8) && r.item(8)->truthy();
    u.server_muted = r.item(9) && r.item(9)->truthy();
    u.text_only = r.item(10) && r.item(10)->truthy();
    const Value* s = r.item(11);
    u.can_speak = !s || s->isNil() || s->truthy();
    return true;
}

bool decodeHello(const Value& m, Packet& out)
{
    if (!m.isMap()) return false;
    auto& h = out.hello;
    h.name = cleaned(m.at("name"), MAX_NAME);
    if (const Value* r = m.at("room")) {
        if (r->isStr()) { h.room_is_name = true; h.room_name = r->str(); }
        else h.room_is_id = r->getInt(h.room_id);
    }
    if (const Value* v = m.at("password"); v && v->isStr()) { h.has_password = true; h.password = v->str(); }
    if (const Value* v = m.at("server_password"); v && v->isStr()) {
        h.has_server_password = true;
        h.server_password = v->str();
    }
    if (const Value* v = m.at("text_only")) h.text_only = v->truthy();
    if (const Value* v = m.at("muted")) h.muted = v->truthy();
    if (const Value* v = m.at("deaf")) h.deaf = v->truthy();
    int64_t i;
    if (intOf(m.at("hops"), i) && i >= 0 && i < 256) h.hops = (int32_t)i;
    double rtt;
    if (const Value* v = m.at("rtt"); v && v->getNumber(rtt) && rtt >= 0 && rtt < 1e6) h.rtt_ms = (int64_t)rtt;
    h.has_ver = intOf(m.at("ver"), h.ver);
    if (const Value* v = m.at("app"); v && v->isStr()) h.app = v->str();
    return true;
}

bool decodeWelcome(const Value& m, Packet& out)
{
    if (!m.isMap()) return false;
    auto& w = out.welcome;
    if (!intOf(m.at("sid"), w.sid)) return false;
    w.name = cleaned(m.at("name"), MAX_NAME);
    if (w.name.empty()) w.name = "server";
    w.motd = cleaned(m.at("motd"), MAX_TEXT);
    w.has_ver = intOf(m.at("ver"), w.ver);
    w.app = cleaned(m.at("app"), 32);
    return true;
}

bool decodeFrames(const Value& v, Packet& out)
{
    if (v.isBin()) {
        out.frames.push_back(Bytes{ v.data, v.len });
        return true;
    }
    if (!v.isArray() || v.size() == 0) return false;
    out.frames_list = true;
    size_t n = v.size() < MAX_BATCH ? v.size() : MAX_BATCH;
    out.frames.reserve(n);
    for (size_t k = 0; k < n; ++k) {
        const Value* f = v.item(k);
        out.frames.push_back(f->isBin() ? Bytes{ f->data, f->len } : Bytes{});
    }
    return true;
}

bool decodeRoom(const Value& r, Packet& out)
{
    if (!r.isArray() || r.size() < 3) return false;
    auto& room = out.room;
    room.none = !intOf(r.item(0), room.id);
    if (room.none) return true;
    if (!r.item(1)->isStr() || !intOf(r.item(2), room.frame_ms)) return false;
    room.profile_name = r.item(1)->str();
    room.profile = profileFind(room.profile_name);
    room.can_speak = r.size() > 3 ? r.item(3)->truthy() : true;
    return true;
}

bool decodeKey(int64_t key, const Value& v, Packet& out)
{
    switch (key) {
    case KEY_FRAMES:  return decodeFrames(v, out);
    case KEY_SPEAKER: return v.getInt(out.speaker);
    case KEY_SEQ: {
        int64_t s;
        if (!v.getInt(s) || s < 0 || s > 0xffff) return false;
        out.seq = (uint16_t)s;
        return true;
    }
    case KEY_ROOM:    return decodeRoom(v, out);
    case KEY_HELLO:   return decodeHello(v, out);
    case KEY_WELCOME: return decodeWelcome(v, out);
    case KEY_CHANNEL: {
        if (!v.isArray()) return false;
        /* a list of records; a bare record is accepted the same way USER's is */
        bool list = v.size() > 0 && v.item(0)->isArray();
        if (!list) {
            Channel c;
            if (!decodeChannel(v, c)) return false;
            out.channels.push_back(std::move(c));
            return true;
        }
        out.channels.reserve(v.size());
        for (size_t k = 0; k < v.size(); ++k) {
            Channel c;
            if (decodeChannel(*v.item(k), c)) out.channels.push_back(std::move(c));
        }
        return true;
    }
    case KEY_USER: {
        if (!v.isArray()) return false;
        bool list = v.size() > 0 && v.item(0)->isArray();
        out.users_update = !list;
        if (!list) {
            User u;
            if (!decodeUser(v, u)) return false;
            out.users.push_back(std::move(u));
            return true;
        }
        out.users.reserve(v.size());
        for (size_t k = 0; k < v.size(); ++k) {
            User u;
            if (decodeUser(*v.item(k), u)) out.users.push_back(std::move(u));
        }
        return true;
    }
    case KEY_USER_LEFT:    return v.getInt(out.user_left);
    case KEY_CHANNEL_GONE: return v.getInt(out.channel_gone);
    case KEY_MOVE: {
        if (!v.isArray() || v.size() < 2 || !intOf(v.item(0), out.move.room)) return false;
        if (v.item(1)->isStr()) { out.move.has_password = true; out.move.password = v.item(1)->str(); }
        return true;
    }
    case KEY_DENIED: {
        if (!v.isArray() || v.size() != 2) return false;
        out.denied.final = v.item(0)->isNil();
        intOf(v.item(0), out.denied.room);
        out.denied.reason = cleaned(v.item(1), MAX_TEXT);
        return true;
    }
    case KEY_TEXT: {
        if (v.isStr()) { out.text = cleaned(&v, MAX_TEXT); return true; }
        if (!v.isArray() || v.size() != 2 || !intOf(v.item(0), out.text_member)) return false;
        out.text_from = true;
        out.text = cleaned(v.item(1), MAX_TEXT);
        return true;
    }
    case KEY_STATE: {
        if (!v.isArray() || v.size() < 2) return false;
        out.state.muted = v.item(0)->truthy();
        out.state.deaf = v.item(1)->truthy();
        return true;
    }
    case KEY_SYNCED: return true;
    case KEY_POKE: {
        if (!v.isArray() || v.size() != 2 || !intOf(v.item(0), out.poke.member)) return false;
        out.poke.text = cleaned(v.item(1), MAX_TEXT);
        return true;
    }
    case KEY_ADMIN: {
        if (!v.isArray() || v.size() < 2 || !v.item(0)->isStr() || !intOf(v.item(1), out.admin.target))
            return false;
        out.admin.action = v.item(0)->str();
        out.admin.has_arg = v.size() > 2 && intOf(v.item(2), out.admin.arg);
        return true;
    }
    case KEY_NOTICE: out.notice = cleaned(&v, MAX_TEXT); return true;
    case KEY_TALK_END:
        out.talk_end_member_set = v.getInt(out.talk_end_member);
        return true;
    case KEY_DIALIN: return true;
    case KEY_CONFIG: {
        if (!v.isArray() || v.size() < 1) return false;
        if (v.item(0)->strEq("motd") && v.size() > 1) {
            out.config_motd = true;
            out.motd = cleaned(v.item(1), MAX_TEXT);
        }
        return true;
    }
    default: return true;   /* unknown keys are ignored */
    }
}

}  // namespace

bool decodePacket(const uint8_t* p, size_t n, Packet& out)
{
    out = Packet{};
    Value root;
    if (!lxstmp::decode(p, n, root) || !root.isMap()) return false;
    for (size_t k = 0; k < root.size(); ++k) {
        int64_t key;
        if (!root.key(k)->getInt(key) || key < 1 || key > 31) continue;
        if (decodeKey(key, *root.value(k), out)) out.present |= 1u << key;
        else out.malformed |= 1u << key;
    }
    return true;
}

/* ── encoding ── */

namespace {

void strOrNil(Writer& w, bool has, const std::string& s)
{
    if (has) w.str(s);
    else w.nil();
}

}  // namespace

std::vector<uint8_t> encodeHello(const Hello& h)
{
    Writer w;
    w.map(1);
    w.uint(KEY_HELLO);
    w.map(11);
    w.str("name");            w.str(cleanText(h.name, MAX_NAME));
    w.str("room");
    switch (h.room_kind) {
    case Hello::ROOM_ID:   w.sint(h.room_id); break;
    case Hello::ROOM_NAME: w.str(h.room_name); break;
    default:               w.nil(); break;
    }
    w.str("password");        strOrNil(w, h.has_password, h.password);
    w.str("server_password"); strOrNil(w, h.has_server_password, h.server_password);
    w.str("text_only");       w.boolean(h.text_only);
    w.str("muted");           w.boolean(h.muted);
    w.str("deaf");            w.boolean(h.deaf);
    w.str("hops");            if (h.hops >= 0) w.sint(h.hops); else w.nil();
    w.str("rtt");             if (h.rtt_ms >= 0) w.sint(h.rtt_ms); else w.nil();
    w.str("ver");             w.sint(PROTOCOL_VERSION);
    w.str("app");             w.str(h.app);
    return std::move(w.out);
}

std::vector<uint8_t> encodeFrames(const Bytes* frames, size_t count, uint16_t seq)
{
    Writer w;
    size_t total = 8;
    for (size_t k = 0; k < count; ++k) total += frames[k].n + 3;
    w.out.reserve(total);
    w.map(2);
    w.uint(KEY_FRAMES);
    if (count == 1) {
        w.bin(frames[0].p, frames[0].n);
    } else {
        w.array((uint32_t)count);
        for (size_t k = 0; k < count; ++k) w.bin(frames[k].p, frames[k].n);
    }
    w.uint(KEY_SEQ);
    w.uint(seq);
    return std::move(w.out);
}

std::vector<uint8_t> encodeTalkEnd()
{
    Writer w;
    w.map(1);
    w.uint(KEY_TALK_END);
    w.boolean(true);
    return std::move(w.out);
}

std::vector<uint8_t> encodeText(const std::string& text)
{
    std::string t = cleanText(text, MAX_TEXT);
    if (t.empty()) return {};
    Writer w;
    w.map(1);
    w.uint(KEY_TEXT);
    w.str(t);
    return std::move(w.out);
}

std::vector<uint8_t> encodeMove(int64_t room_id, const std::string* password)
{
    Writer w;
    w.map(1);
    w.uint(KEY_MOVE);
    w.array(2);
    w.sint(room_id);
    strOrNil(w, password != nullptr, password ? *password : std::string());
    return std::move(w.out);
}

std::vector<uint8_t> encodeState(bool muted, bool deaf)
{
    Writer w;
    w.map(1);
    w.uint(KEY_STATE);
    w.array(2);
    w.boolean(muted);
    w.boolean(deaf);
    return std::move(w.out);
}

std::vector<uint8_t> encodePoke(int64_t target, const std::string& text)
{
    std::string t = cleanText(text, MAX_TEXT);
    if (t.empty()) t = "poke";
    Writer w;
    w.map(1);
    w.uint(KEY_POKE);
    w.array(2);
    w.sint(target);
    w.str(t);
    return std::move(w.out);
}

std::vector<uint8_t> encodeAdmin(const char* action, int64_t target, const int64_t* arg)
{
    Writer w;
    w.map(1);
    w.uint(KEY_ADMIN);
    w.array(3);
    w.str(action);
    w.sint(target);
    if (arg) w.sint(*arg);
    else w.nil();
    return std::move(w.out);
}

std::vector<uint8_t> encodeConfigMotd(const std::string& text)
{
    Writer w;
    w.map(1);
    w.uint(KEY_CONFIG);
    w.array(2);
    w.str("motd");
    w.str(cleanText(text, MAX_TEXT));
    return std::move(w.out);
}

/* ── announce ── */

bool decodeAnnounce(const uint8_t* p, size_t n, Announce& out)
{
    out = Announce{};
    Value v;
    if (!lxstmp::decode(p, n, v) || !v.isArray() || v.size() < 2) return false;
    if (!v.item(0)->getInt(out.version)) return false;
    out.name = cleaned(v.item(1), MAX_NAME);
    int64_t flags;
    if (v.size() > 2 && v.item(2)->getInt(flags)) out.flags = (uint32_t)flags;
    out.description = cleaned(v.item(3), MAX_ANNOUNCE_DESCRIPTION);
    out.language = cleaned(v.item(4), MAX_LOCALE);
    out.country = cleaned(v.item(5), MAX_LOCALE);
    return true;
}

std::vector<uint8_t> encodeAnnounce(const Announce& a)
{
    std::string fields[3] = { cleanText(a.description, MAX_ANNOUNCE_DESCRIPTION),
                              cleanText(a.language, MAX_LOCALE), cleanText(a.country, MAX_LOCALE) };
    uint32_t count = 6;
    while (count > 3 && fields[count - 4].empty()) count--;
    Writer w;
    w.array(count);
    w.sint(a.version);
    w.str(a.name);
    w.uint(a.flags);
    for (uint32_t k = 3; k < count; ++k) w.str(fields[k - 3]);
    return std::move(w.out);
}

}  // namespace plwire
