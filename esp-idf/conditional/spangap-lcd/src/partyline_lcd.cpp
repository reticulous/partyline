/**
 * partyline_lcd.cpp — the on-device Partyline app (LVGL).
 *
 *   servers ──tap server──▶ server ──Connect (live)──▶ room ──Rooms──▶ room list
 *      │                                                ▲ │
 *      └──tap "now in"─────────────────────────────────┘ ├─tap header──▶ info
 *                                                         └─tap member──▶ member card
 *
 * Seven screens on the one app layer, one shown at a time:
 *   - Servers (launcher entry, unless a server is held — then the room): the
 *     server the node holds now, the saved servers (status dot), the servers
 *     heard announcing that are not saved (hops, age, password and allow-list
 *     markers; a hidden one is "unnamed" and the head of its hash), then a
 *     field to add one by its 32-hex hash.
 *   - Server: name, hash in fours, its announced description, access, the
 *     status sentence, who we connect as (the LXMF identities and the node
 *     itself are offered when that is not settled), the room to ask for, the
 *     room and server passwords, the message of the day, Connect / Disconnect
 *     and Forget (which asks first).
 *   - Room: the header names the room and server, with the room's codec
 *     profile and a PTT marker for a push-to-talk room. Under it the talk bar,
 *     lit while lxst.talking is 1, saying how to talk here (or "listen only",
 *     "text only", or partyline.tx_blocked's reason when the first hop
 *     carries none of our audio), with the Mute / Deafen toggles and Rooms. The members of
 *     the room are a column on the left, the speaking ones lit green, with
 *     operator (@), muted, deafened and text-only marks; the text log is on
 *     the right; the composer is at the bottom. The talk bar is also a hold
 *     button: held, it writes lxst.cmd.ptt "1", and "0" on release. While this
 *     screen is up, the app is in the foreground, the screen is awake and no
 *     text field is focused, the held Space key is claimed (lcd_keys.h) so
 *     holding Space talks; the board's dedicated talk key needs nothing here.
 *     Focus rests off the composer: a printable key moves into it, Back or
 *     Enter on an empty line leaves it.
 *   - Room list: every room on the server with its member count, profile,
 *     PTT and password marks; a tap moves there (asking for the password when
 *     the room's access has bit 4). The current room is marked and never a
 *     move target.
 *   - Info: server, status, room, profile, description, the voice session's
 *     state and counters, and the message of the day; Disconnect.
 *   - Member card: name, identity, hops, round trip, what they may do; Poke.
 * Pokes and notices arriving while the app is open show as toasts as well as
 * log lines; a refused command's <cmd>.error shows as a toast.
 *
 * Storage is the API (partyline.h has the contract). Everything runs on the
 * lcd task: the subscriptions are made from it, so their callbacks may touch
 * LVGL. They only mark what changed; one debounced timer folds a burst into one
 * refresh. List rows are reconciled, not rebuilt, and the log is appended to
 * line by line — a rebuild on the lcd task stalls the keyboard poll.
 */
#include "lcd.h"
#include "lcd_app.h"
#include "lcd_input_box.h"
#include "lcd_keys.h"
#include "partyline_app.h"
#include "storage.h"
#include "esp_timer.h"
#include "cJSON.h"

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

/* ---- look & feel: rrc's palette, so the two read as one family ---- */

const uint32_t C_BG      = 0x101418;   /* app background */
const uint32_t C_SUB     = 0x9098A0;   /* secondary text */
const uint32_t C_ACCENT  = 0x2563A0;   /* primary buttons, section titles */
const uint32_t C_AMBER   = 0xE8A33A;   /* errors, pokes, password marks */
const uint32_t C_RULE    = 0x232B33;   /* row and bar rules */
const uint32_t C_OK      = 0x36C06A;   /* live dot, talking */
const uint32_t C_SEL     = 0x1B2A3C;   /* selected / focused row */
const uint32_t C_BAR     = 0x0C1014;   /* composer bar */
const uint32_t C_FIELD   = 0x2F3B47;   /* field and ghost-button borders */
const uint32_t C_GUTTER  = 0x5F6A75;   /* log times */
const uint32_t C_PRES    = 0x6C7783;   /* system lines, placeholders */
const uint32_t C_NOTE    = 0x8FA3B6;   /* notices */
const uint32_t C_ACT     = 0xCFD8E3;   /* ghost-button text, chevrons */
const uint32_t C_HOLLOW  = 0x8A96A3;   /* a server being connected */
const uint32_t C_DANGER  = 0x7A2F2F;   /* Forget / Disconnect border */
const uint32_t C_DANGERT = 0xF0A0A0;   /* Forget / Disconnect text */
const uint32_t C_FAIL    = 0xE06060;   /* over the text limit, server-muted */
const uint32_t C_DIALOG  = 0x20262E;   /* dialog box, toast */
const uint32_t C_SPEAK   = 0x173A24;   /* a speaking member's row */
const uint32_t C_TALKRDY = 0x172331;   /* talk bar, ready */
const uint32_t C_TALKDN  = 0x1D4F80;   /* talk bar, held before the session keys */

const lv_font_t* kFont      = &lv_font_montserrat_12_latin;
const lv_font_t* kBold      = &lv_font_montserrat_12_latin;
const lv_font_t* kSmall     = &lv_font_montserrat_12_latin;
const lv_font_t* kSmallBold = &lv_font_montserrat_12_latin;
const lv_font_t* kBtn       = &lv_font_montserrat_12_latin;
void refreshFont() {
    kFont      = lcdFont(LcdFace::UI,      lcdPx(14));
    kBold      = lcdFont(LcdFace::UI_BOLD, lcdPx(14));
    kSmall     = lcdFont(LcdFace::UI,      lcdPx(11));
    kSmallBold = lcdFont(LcdFace::UI_BOLD, lcdPx(11));
    kBtn       = lcdFont(LcdFace::UI_BOLD, lcdPx(12));
}

int hdrH()    { return lcdPx(26); }
int membersW() { return lcdPx(104); }

const uint32_t DEBOUNCE_MS = 80;       /* storage bursts fold into one refresh */
const uint32_t TOAST_MS    = 3500;
const size_t   LOG_MAX     = 120;      /* log lines drawn; older rows are dropped */
const int      TEXT_LIMIT  = 300;      /* partyline's text limit, bytes */
const int      LXMF_SLOTS  = 10;       /* identity slots offered in the chooser */

const char* const NODE_IDENT = "secrets.rnsd.identity";

/* ---- text ---- */

bool zeroWidth(uint32_t cp) {
    return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
           (cp >= 0xE0000 && cp <= 0xE007F);
}

/* Network text made drawable in the UI font: controls and zero-width
 * modifiers dropped, each run of glyphless codepoints shown as one '?'.
 * `oneLine` folds CR/LF/TAB to a space. */
std::string printable(std::string_view in, bool oneLine) {
    std::string out;
    out.reserve(in.size());
    bool inRun = false;
    size_t i = 0, n = in.size();
    while (i < n) {
        uint8_t b = (uint8_t)in[i];
        uint32_t cp; size_t len;
        if      (b < 0x80)           { cp = b;        len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; len = 4; }
        else { i++; continue; }
        if (i + len > n) break;
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            uint8_t c = (uint8_t)in[i + k];
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok) { i++; continue; }
        size_t at = i;
        i += len;
        if (cp == '\n' || cp == '\r' || cp == '\t') { out += oneLine ? ' ' : (char)cp; inRun = false; continue; }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || zeroWidth(cp)) continue;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(kFont, &g, cp, 0) || g.is_placeholder) {
            if (!inRun) out += '?';
            inRun = true;
            continue;
        }
        inRun = false;
        out.append(in.data() + at, len);
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
bool isHex32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}
std::string fours(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i < hex.size(); i += 4) {
        if (i) out += ' ';
        out += hex.substr(i, 4);
    }
    return out;
}
bool startsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }
bool endsWith(const char* s, const char* p) {
    size_t a = strlen(s), b = strlen(p);
    return a >= b && !strcmp(s + a - b, p);
}
const char* DOT = " \xC2\xB7 ";   /* " · " */

long monoS() { return (long)(esp_timer_get_time() / 1000000); }

std::string hhmm(uint32_t ts) {
    if (!ts) return "";
    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[8];
    snprintf(b, sizeof b, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return b;
}

std::string age(long secs) {
    if (secs < 0) secs = 0;
    char b[16];
    if      (secs < 60)    snprintf(b, sizeof b, "%ld s", secs);
    else if (secs < 3600)  snprintf(b, sizeof b, "%ld min", secs / 60);
    else if (secs < 86400) snprintf(b, sizeof b, "%ld h", secs / 3600);
    else                   snprintf(b, sizeof b, "%ld d", secs / 86400);
    return b;
}

std::string hopsText(int h) { return std::to_string(h) + (h == 1 ? " hop" : " hops"); }

std::string rttText(int ms) {
    if (ms <= 0) return "";
    char b[16];
    snprintf(b, sizeof b, "%d.%d s", ms / 1000, (ms % 1000) / 100);
    return b;
}

/* rrc's (and lxmf's) peerAvatar hue, over the first 8 characters. */
int peerHue(const std::string& peer) {
    uint32_t h = 0;
    for (size_t i = 0; i < peer.size() && i < 8; i++) h = h * 31u + (uint8_t)peer[i];
    return (int)(h % 360u);
}
lv_color_t hsl(int h, int s, int l) {
    float S = s / 100.0f, L = l / 100.0f;
    float C = (1.0f - fabsf(2.0f * L - 1.0f)) * S;
    float Hp = (float)(h % 360) / 60.0f;
    float X = C * (1.0f - fabsf(fmodf(Hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if      (Hp < 1) { r = C; g = X; }
    else if (Hp < 2) { r = X; g = C; }
    else if (Hp < 3) { g = C; b = X; }
    else if (Hp < 4) { g = X; b = C; }
    else if (Hp < 5) { r = X; b = C; }
    else             { r = C; b = X; }
    float m = L - C / 2.0f;
    auto u8 = [](float v) { int x = (int)lroundf(v * 255.0f); return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); };
    return lv_color_make(u8(r + m), u8(g + m), u8(b + m));
}

/* ---- commands ---- */

void sendJson(const char* key, cJSON* o) {
    char* s = cJSON_PrintUnformatted(o);
    if (s) { storageSet(key, s); cJSON_free(s); }
    cJSON_Delete(o);
}

/* ---- model (lcd task only) ---- */

struct Saved { std::string dest, name, ident, room; int autoc = 0; };
struct Heard { std::string dest, name, desc, lang, country; int flags = 0, hops = 0; long seen = 0; };
struct RoomRec {
    std::string id, name, profile, profileText, desc;
    int frameMs = 0, access = 0, pttJitter = 0, flags = 0, members = 0;
};
struct Member {
    std::string mid, name, identity, room;
    int muted = 0, deaf = 0, hops = 0, rtt = 0, op = 0, serverMuted = 0, textOnly = 0,
        canSpeak = 0, speaking = 0, self = 0;
};
struct LogLine { long n = 0; uint32_t ts = 0; std::string kind, mid, name, room, text; };

std::map<std::string, Saved>   g_saved;
std::map<std::string, Heard>   g_heard;
std::map<std::string, RoomRec> g_rooms;
std::map<std::string, Member>  g_members;

/* "<prefix><id>.<field>", no further dot: the id and the field. */
bool splitRec(const char* key, size_t pfx, std::string& id, const char*& field) {
    if (!key || strlen(key) <= pfx) return false;
    const char* tail = key + pfx;
    const char* dot = strchr(tail, '.');
    if (!dot || dot == tail || strchr(dot + 1, '.')) return false;
    id.assign(tail, dot - tail);
    field = dot + 1;
    return true;
}

std::map<std::string, Saved>* s_savedAcc = nullptr;
void savedLeaf(const char* key, const char* val) {
    std::string id; const char* f;
    if (!s_savedAcc || !splitRec(key, sizeof("s.partyline.servers.") - 1, id, f)) return;
    id = lower(id);
    if (!isHex32(id)) return;
    Saved& s = (*s_savedAcc)[id];
    s.dest = id;
    std::string v = val ? val : "";
    if      (!strcmp(f, "name"))  s.name = v;
    else if (!strcmp(f, "ident")) s.ident = v;
    else if (!strcmp(f, "room"))  s.room = v;
    else if (!strcmp(f, "auto"))  s.autoc = atoi(v.c_str());
}
void loadSaved() {
    std::map<std::string, Saved> acc;
    s_savedAcc = &acc;
    storageForEach("s.partyline.servers.", savedLeaf);
    s_savedAcc = nullptr;
    g_saved.swap(acc);
}

std::map<std::string, Heard>* s_heardAcc = nullptr;
void heardLeaf(const char* key, const char* val) {
    std::string id; const char* f;
    if (!s_heardAcc || !splitRec(key, sizeof("partyline.heard.") - 1, id, f)) return;
    id = lower(id);
    if (!isHex32(id)) return;
    Heard& h = (*s_heardAcc)[id];
    h.dest = id;
    std::string v = val ? val : "";
    if      (!strcmp(f, "name"))    h.name = v;
    else if (!strcmp(f, "flags"))   h.flags = atoi(v.c_str());
    else if (!strcmp(f, "desc"))    h.desc = v;
    else if (!strcmp(f, "lang"))    h.lang = v;
    else if (!strcmp(f, "country")) h.country = v;
    else if (!strcmp(f, "hops"))    h.hops = atoi(v.c_str());
    else if (!strcmp(f, "seen"))    h.seen = atol(v.c_str());
}
void loadHeard() {
    std::map<std::string, Heard> acc;
    s_heardAcc = &acc;
    storageForEach("partyline.heard.", heardLeaf);
    s_heardAcc = nullptr;
    g_heard.swap(acc);
}

std::map<std::string, RoomRec>* s_roomAcc = nullptr;
void roomLeaf(const char* key, const char* val) {
    std::string id; const char* f;
    if (!s_roomAcc || !splitRec(key, sizeof("partyline.rooms.") - 1, id, f)) return;
    RoomRec& r = (*s_roomAcc)[id];
    r.id = id;
    std::string v = val ? val : "";
    int n = atoi(v.c_str());
    if      (!strcmp(f, "name"))          r.name = v;
    else if (!strcmp(f, "profile"))       r.profile = v;
    else if (!strcmp(f, "profile_text"))  r.profileText = v;
    else if (!strcmp(f, "frame_ms"))      r.frameMs = n;
    else if (!strcmp(f, "access"))        r.access = n;
    else if (!strcmp(f, "desc"))          r.desc = v;
    else if (!strcmp(f, "ptt_jitter_ms")) r.pttJitter = n;
    else if (!strcmp(f, "flags"))         r.flags = n;
    else if (!strcmp(f, "members"))       r.members = n;
}
void loadRooms() {
    std::map<std::string, RoomRec> acc;
    s_roomAcc = &acc;
    storageForEach("partyline.rooms.", roomLeaf);
    s_roomAcc = nullptr;
    g_rooms.swap(acc);
}

std::map<std::string, Member>* s_memAcc = nullptr;
void memberLeaf(const char* key, const char* val) {
    std::string id; const char* f;
    if (!s_memAcc || !splitRec(key, sizeof("partyline.members.") - 1, id, f)) return;
    Member& m = (*s_memAcc)[id];
    m.mid = id;
    std::string v = val ? val : "";
    int n = atoi(v.c_str());
    if      (!strcmp(f, "name"))         m.name = v;
    else if (!strcmp(f, "identity"))     m.identity = v;
    else if (!strcmp(f, "room"))         m.room = v;
    else if (!strcmp(f, "muted"))        m.muted = n;
    else if (!strcmp(f, "deaf"))         m.deaf = n;
    else if (!strcmp(f, "hops"))         m.hops = n;
    else if (!strcmp(f, "rtt"))          m.rtt = n;
    else if (!strcmp(f, "op"))           m.op = n;
    else if (!strcmp(f, "server_muted")) m.serverMuted = n;
    else if (!strcmp(f, "text_only"))    m.textOnly = n;
    else if (!strcmp(f, "can_speak"))    m.canSpeak = n;
    else if (!strcmp(f, "speaking"))     m.speaking = n;
    else if (!strcmp(f, "self"))         m.self = n;
}
void loadMembers() {
    std::map<std::string, Member> acc;
    s_memAcc = &acc;
    storageForEach("partyline.members.", memberLeaf);
    s_memAcc = nullptr;
    g_members.swap(acc);
}

/* ---- connection state ---- */

std::string plServer() { return lower(storageGetStr("partyline.server", "")); }
std::string plState()  { return storageGetStr("partyline.state", "idle"); }
std::string plStatus() { return printable(storageGetStr("partyline.status", ""), true); }
bool stateActive(const std::string& st) {
    return st == "connecting" || st == "joining" || st == "live" || st == "lost";
}
bool heldActive() { return !plServer().empty() && stateActive(plState()); }
bool isSaved(const std::string& d) { return g_saved.count(d) > 0; }

/* The room we are in, as an id of the room list. partyline.room is matched
 * against the ids first and the names second. */
std::string curRoomId() {
    std::string r = storageGetStr("partyline.room", "");
    if (r.empty() || g_rooms.count(r)) return r;
    for (auto& [id, rec] : g_rooms) if (rec.name == r) return id;
    return r;
}
std::string roomName(const std::string& id) {
    auto it = g_rooms.find(id);
    std::string n = it != g_rooms.end() ? it->second.name : id;
    return printable(n, true);
}
bool memberInRoom(const Member& m, const std::string& rid) {
    if (rid.empty()) return false;
    if (m.room == rid) return true;
    auto it = g_rooms.find(rid);
    return it != g_rooms.end() && !it->second.name.empty() && m.room == it->second.name;
}

const Member* selfMember() {
    std::string sid = storageGetStr("partyline.sid", "");
    for (auto& [mid, m] : g_members) if (m.self || (!sid.empty() && mid == sid)) return &m;
    return nullptr;
}
bool selfMuted() {
    const Member* m = selfMember();
    return m ? m->muted != 0 : storageGetInt("s.partyline.muted", 0) != 0;
}
bool selfDeaf() {
    const Member* m = selfMember();
    return m ? m->deaf != 0 : storageGetInt("s.partyline.deaf", 0) != 0;
}

/* The saved name, else (for the held server) WELCOME's, else the announced
 * one; a server with none is "unnamed" and the head of its hash. */
std::string serverName(const std::string& d) {
    std::string n;
    auto s = g_saved.find(d);
    if (s != g_saved.end()) n = s->second.name;
    if (n.empty() && d == plServer()) n = storageGetStr("partyline.name", "");
    if (n.empty()) { auto h = g_heard.find(d); if (h != g_heard.end()) n = h->second.name; }
    n = printable(n, true);
    return n.empty() ? "unnamed" + std::string(DOT) + d.substr(0, 8) : n;
}

/* "password · allow list" from the announce flags. */
std::string accessMarks(int flags) {
    std::string s;
    if (flags & 2) s = "password";
    if (flags & 4) s += (s.empty() ? "" : DOT) + std::string("allow list");
    return s;
}

/* ---- identities: the storage path of the identity a server knows us by ---- */

struct IdChoice { std::string path, label, sub; };

std::vector<IdChoice> identChoices() {
    std::vector<IdChoice> out;
    for (int n = 0; n < LXMF_SLOTS; n++) {
        char k[48];
        snprintf(k, sizeof k, "lxmf.id.%d.dest_hash", n);
        std::string dest = storageGetStr(k, "");
        if (!isHex32(dest)) continue;
        snprintf(k, sizeof k, "s.lxmf.id.%d.kind", n);
        if (storageGetStr(k, "") == "group") continue;
        snprintf(k, sizeof k, "s.lxmf.id.%d.display_name", n);
        std::string name = printable(storageGetStr(k, ""), true);
        snprintf(k, sizeof k, "secrets.lxmf.id.%d.privkey", n);
        out.push_back({ k, name.empty() ? "LXMF identity " + std::to_string(n) : name,
                        "LXMF " + fours(dest.substr(0, 16)) + " \xE2\x80\xA6" });
    }
    out.push_back({ NODE_IDENT, "This node", "the node's own identity, kept apart from LXMF" });
    return out;
}

std::string identLabel(const std::string& path) {
    if (path.empty()) return "";
    for (auto& c : identChoices()) if (c.path == path) return c.label;
    return path == NODE_IDENT ? "This node" : "an identity no longer here";
}

/* The only LXMF identity, the node when there is none, "" when there are
 * several (the operator's to choose). */
std::string identDefault() {
    auto cs = identChoices();
    if (cs.size() == 1) return cs[0].path;
    if (cs.size() == 2) return cs[0].path;
    return "";
}

/* ---- widgets (rrc's) ---- */

lv_obj_t* s_layer = nullptr;

lv_obj_t* mkLabel(lv_obj_t* parent, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f ? f : kFont, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt.c_str());
    return l;
}
lv_obj_t* mkFill(lv_obj_t* row, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = mkLabel(row, txt, color, f);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, 1);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_set_height(l, lv_font_get_line_height(f ? f : kFont));
    return l;
}
lv_obj_t* mkWrap(lv_obj_t* row, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = mkLabel(row, txt, color, f);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, 1);
    lv_obj_set_flex_grow(l, 1);
    return l;
}
lv_obj_t* mkWrapFull(lv_obj_t* parent, const std::string& txt, uint32_t color, const lv_font_t* f) {
    lv_obj_t* l = mkLabel(parent, txt, color, f);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}
lv_obj_t* mkBox(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}
lv_obj_t* mkRowBox(lv_obj_t* parent, int gap) {
    lv_obj_t* o = mkBox(parent);
    lv_obj_set_width(o, lv_pct(100));
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}
lv_obj_t* mkColBox(lv_obj_t* parent) {
    lv_obj_t* o = mkBox(parent);
    lv_obj_set_width(o, lv_pct(100));
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    return o;
}
lv_obj_t* mkScreen() {
    lv_obj_t* o = lv_obj_create(s_layer);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(o, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
lv_obj_t* mkBody(lv_obj_t* screen) {
    lv_obj_t* b = lv_obj_create(screen);
    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(b, lcdPx(6), 0);
    lv_obj_set_style_pad_ver(b, lcdPx(4), 0);
    lv_obj_set_style_pad_row(b, lcdPx(2), 0);
    return b;
}
lv_obj_t* mkActionBar(lv_obj_t* screen) {
    lv_obj_t* bar = mkRowBox(screen, lcdPx(6));
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, lcdPx(8), 0);
    lv_obj_set_style_pad_ver(bar, lcdPx(5), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_RULE), 0);
    return bar;
}
lv_obj_t* mkSpacer(lv_obj_t* row) {
    lv_obj_t* s = mkBox(row);
    lv_obj_set_flex_grow(s, 1);
    return s;
}
void focusRing(lv_obj_t* o) {
    lv_obj_set_style_outline_width(o, lcdPx(2), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(o, lv_color_hex(0x7FA6FF), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(o, 1, LV_STATE_FOCUS_KEY);
}
void showIf(lv_obj_t* o, bool on) {
    if (!o) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

enum BtnStyle { BTN_PRIMARY, BTN_GHOST, BTN_DANGER };

lv_obj_t* mkButton(lv_obj_t* parent, const std::string& text, BtnStyle st,
                   lv_event_cb_t cb, void* ud = nullptr, lv_obj_t** lblOut = nullptr) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(b, lcdPx(4), 0);
    lv_obj_set_style_pad_hor(b, lcdPx(10), 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    uint32_t tc = 0xFFFFFF;
    if (st == BTN_PRIMARY) {
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(st == BTN_DANGER ? C_DANGER : C_FIELD), 0);
        tc = st == BTN_DANGER ? C_DANGERT : C_ACT;
    }
    lv_obj_set_style_bg_color(b, lv_color_hex(st == BTN_PRIMARY ? 0x1D4F80 : C_SEL), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    /* A toggle that is on (Mute, Deafen) is amber-edged. */
    lv_obj_set_style_border_color(b, lv_color_hex(C_AMBER), LV_STATE_CHECKED);
    focusRing(b);
    lv_obj_t* l = mkLabel(b, text, tc, kBtn);
    if (lblOut) *lblOut = l;
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), b);
    return b;
}

lv_obj_t* mkRow(lv_obj_t* parent) {
    lv_obj_t* r = lv_button_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, lcdPx(2), 0);
    lv_obj_set_style_pad_hor(r, lcdPx(6), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(C_RULE), 0);
    for (lv_state_t s : { (lv_state_t)LV_STATE_FOCUSED, (lv_state_t)LV_STATE_PRESSED, (lv_state_t)LV_STATE_CHECKED }) {
        lv_obj_set_style_bg_color(r, lv_color_hex(C_SEL), s);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, s);
    }
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), r);
    return r;
}

enum { DOT_NONE = -1, DOT_HOLLOW = 0, DOT_LIVE = 1, DOT_BAD = 2 };
lv_obj_t* mkDot(lv_obj_t* parent, int kind) {
    lv_obj_t* d = mkBox(parent);
    int sz = lcdPx(7);
    lv_obj_set_size(d, sz, sz);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    if (kind == DOT_LIVE) {
        lv_obj_set_style_bg_color(d, lv_color_hex(C_OK), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    } else if (kind == DOT_HOLLOW || kind == DOT_BAD) {
        lv_obj_set_style_border_width(d, lcdPx(1) < 1 ? 1 : lcdPx(1), 0);
        lv_obj_set_style_border_color(d, lv_color_hex(kind == DOT_BAD ? C_AMBER : C_HOLLOW), 0);
    }
    return d;
}

lv_obj_t* mkSection(lv_obj_t* parent, const std::string& text) {
    lv_obj_t* l = mkLabel(parent, text, C_ACCENT, kSmallBold);
    lv_obj_set_style_pad_top(l, lcdPx(5), 0);
    lv_obj_set_style_pad_left(l, lcdPx(6), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

lv_obj_t* mkKv(lv_obj_t* body, const char* key, const std::string& val, uint32_t color, const lv_font_t* f,
               lv_obj_t** rowOut = nullptr) {
    lv_obj_t* row = mkRowBox(body, lcdPx(4));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t* k = mkLabel(row, key, C_SUB, kSmall);
    lv_obj_set_width(k, lcdPx(60));
    lv_obj_set_style_pad_top(k, lcdPx(2), 0);
    if (rowOut) *rowOut = row;
    return mkWrap(row, val, color, f);
}

lv_obj_t* mkField(lv_obj_t* parent, const char* placeholder, bool password) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_set_style_text_font(ta, kFont, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_PRES), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_FIELD), 0);
    lv_obj_set_style_radius(ta, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(ta, lcdPx(3), 0);
    lv_obj_set_style_pad_hor(ta, lcdPx(6), 0);
    lv_obj_set_width(ta, 1);
    lv_obj_set_flex_grow(ta, 1);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), ta);
    lcdKeyboardAttach(ta);
    return ta;
}

struct TitleLine { lv_obj_t* sg = nullptr; lv_span_t* title = nullptr; lv_span_t* sub = nullptr; };
TitleLine mkTitleLine(lv_obj_t* row) {
    TitleLine t;
    t.sg = lv_spangroup_create(row);
    lv_obj_remove_flag(t.sg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(t.sg, 1);
    lv_obj_set_flex_grow(t.sg, 1);
    lv_obj_set_height(t.sg, lv_font_get_line_height(kBold));
    lv_spangroup_set_max_lines(t.sg, 1);
    lv_spangroup_set_overflow(t.sg, LV_SPAN_OVERFLOW_ELLIPSIS);
    lv_obj_set_style_text_font(t.sg, kBold, 0);
    t.title = lv_spangroup_add_span(t.sg);
    lv_style_set_text_color(lv_span_get_style(t.title), lv_color_white());
    lv_style_set_text_font(lv_span_get_style(t.title), kBold);
    lv_spangroup_set_span_text(t.sg, t.title, "");
    t.sub = lv_spangroup_add_span(t.sg);
    lv_style_set_text_color(lv_span_get_style(t.sub), lv_color_hex(C_SUB));
    lv_style_set_text_font(lv_span_get_style(t.sub), kSmall);
    lv_spangroup_set_span_text(t.sg, t.sub, "");
    return t;
}
void setTitleLine(TitleLine& t, const std::string& title, const std::string& sub) {
    if (!t.sg) return;
    lv_spangroup_set_span_text(t.sg, t.title, title.c_str());
    lv_spangroup_set_span_text(t.sg, t.sub, sub.c_str());
}

struct Header { lv_obj_t* bar = nullptr; lv_obj_t* back = nullptr; TitleLine line; lv_obj_t* right = nullptr; };
Header mkHeader(lv_obj_t* screen, lv_event_cb_t onBack, lv_event_cb_t onTap) {
    Header h;
    h.bar = mkRowBox(screen, lcdPx(6));
    lv_obj_set_height(h.bar, hdrH());
    lv_obj_set_style_pad_hor(h.bar, lcdPx(6), 0);
    lv_obj_set_style_border_side(h.bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(h.bar, 1, 0);
    lv_obj_set_style_border_color(h.bar, lv_color_hex(C_RULE), 0);
    if (onTap) {
        lv_obj_add_flag(h.bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(h.bar, onTap, LV_EVENT_CLICKED, nullptr);
    }
    if (onBack) {
        h.back = mkLabel(h.bar, LV_SYMBOL_LEFT, C_ACT, kFont);
        lv_obj_add_flag(h.back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(h.back, lcdPx(12));
        lv_obj_add_event_cb(h.back, onBack, LV_EVENT_CLICKED, nullptr);
        focusRing(h.back);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), h.back);
    }
    h.line = mkTitleLine(h.bar);
    h.right = mkLabel(h.bar, "", C_SUB, kSmall);
    return h;
}

void onFreeString(lv_event_t* e) { delete static_cast<std::string*>(lv_event_get_user_data(e)); }
void bindString(lv_obj_t* o, lv_event_cb_t cb, const std::string& s) {
    auto* p = new std::string(s);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, p);
    lv_obj_add_event_cb(o, onFreeString, LV_EVENT_DELETE, p);
}
const std::string& boundString(lv_event_t* e) { return *static_cast<std::string*>(lv_event_get_user_data(e)); }

/* ---- deferred focus (rrc's): a tile or tapped row takes the focus on
 * click-release, after our handlers, so focus is placed a beat later. A text
 * field gets it only where the board has keys. ---- */

void updateSpaceClaim();

lv_obj_t* g_focusTarget = nullptr;
bool onScreenNow(lv_obj_t* o) {
    for (; o; o = lv_obj_get_parent(o)) if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}
void focusTimerCb(lv_timer_t*) {
    if (g_focusTarget && lv_obj_is_valid(g_focusTarget) && onScreenNow(g_focusTarget) && lcdInputGroup())
        lv_group_focus_obj(g_focusTarget);
    g_focusTarget = nullptr;
    updateSpaceClaim();
}
void deferFocus(lv_obj_t* o) {
    if (!o) return;
    if (lcdKeyboardOnScreen() && lv_obj_check_type(o, &lv_textarea_class)) return;
    g_focusTarget = o;
    lv_timer_t* t = lv_timer_create(focusTimerCb, 40, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

/* ---- toast: a self-clearing line at the top, never in the way of a tap ---- */

lv_obj_t*   s_toast = nullptr;
lv_obj_t*   s_toastLbl = nullptr;
lv_timer_t* g_toastTimer = nullptr;

void closeToast() {
    if (g_toastTimer) { lv_timer_delete(g_toastTimer); g_toastTimer = nullptr; }
    if (s_toast) { lv_obj_delete(s_toast); s_toast = nullptr; s_toastLbl = nullptr; }
}
void showToast(const std::string& text, uint32_t color) {
    if (!s_layer || text.empty()) return;
    if (!s_toast) {
        s_toast = lv_obj_create(s_layer);
        lv_obj_remove_style_all(s_toast);
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(s_toast, lv_pct(90));
        lv_obj_set_height(s_toast, LV_SIZE_CONTENT);
        lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, hdrH() + lcdPx(4));
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(C_DIALOG), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_toast, lcdPx(6), 0);
        lv_obj_set_style_pad_all(s_toast, lcdPx(6), 0);
        lv_obj_set_style_border_side(s_toast, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(s_toast, lcdPx(3), 0);
        s_toastLbl = mkWrapFull(s_toast, "", 0xFFFFFF, kFont);
    }
    lv_obj_set_style_border_color(s_toast, lv_color_hex(color), 0);
    lv_label_set_text(s_toastLbl, text.c_str());
    lv_obj_move_foreground(s_toast);
    if (g_toastTimer) lv_timer_delete(g_toastTimer);
    g_toastTimer = lv_timer_create([](lv_timer_t*) {
        g_toastTimer = nullptr;   /* one-shot: LVGL frees it after this returns */
        if (s_toast) { lv_obj_delete(s_toast); s_toast = nullptr; s_toastLbl = nullptr; }
    }, TOAST_MS, nullptr);
    lv_timer_set_repeat_count(g_toastTimer, 1);
}

/* ---- dialogs: a confirm (the verb or Cancel) and a password prompt ---- */

lv_obj_t* s_dialog = nullptr;
lv_obj_t* s_dialogTa = nullptr;
void (*g_promptOk)(const std::string&) = nullptr;

void closeDialog() {
    if (!s_dialog) return;
    lv_obj_t* o = s_dialog;
    s_dialog = nullptr;
    s_dialogTa = nullptr;
    g_promptOk = nullptr;
    lcdKeyboardClose();
    lv_obj_delete(o);
    updateSpaceClaim();
}

lv_obj_t* dialogBox(const std::string& text) {
    closeDialog();
    s_dialog = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_dialog);
    lv_obj_set_size(s_dialog, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_50, 0);
    lv_obj_add_flag(s_dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(s_dialog);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, lv_pct(85));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_DIALOG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, lcdPx(6), 0);
    lv_obj_set_style_pad_all(box, lcdPx(10), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, lcdPx(8), 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    mkWrapFull(box, text, 0xFFFFFF, kFont);
    lcdModalTrack(s_dialog, [](void*) { closeDialog(); });
    return box;
}

void showConfirm(const std::string& text, const char* verb, lv_event_cb_t onOk) {
    lv_obj_t* box = dialogBox(text);
    lv_obj_t* btns = mkRowBox(box, lcdPx(8));
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mkButton(btns, verb, BTN_DANGER, onOk);
    lv_obj_t* cancel = mkButton(btns, "Cancel", BTN_GHOST, [](lv_event_t*) { closeDialog(); });
    deferFocus(cancel);
    updateSpaceClaim();
}

void onPromptOk(lv_event_t*) {
    if (!s_dialogTa || !g_promptOk) return;
    std::string v = lv_textarea_get_text(s_dialogTa);
    auto ok = g_promptOk;
    closeDialog();
    ok(v);
}

void showPrompt(const std::string& text, const char* placeholder, const char* verb,
                void (*onOk)(const std::string&)) {
    lv_obj_t* box = dialogBox(text);
    lv_obj_t* fr = mkRowBox(box, 0);
    s_dialogTa = mkField(fr, placeholder, true);
    lv_obj_add_event_cb(s_dialogTa, onPromptOk, LV_EVENT_READY, nullptr);
    g_promptOk = onOk;
    lv_obj_t* btns = mkRowBox(box, lcdPx(8));
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mkButton(btns, "Cancel", BTN_GHOST, [](lv_event_t*) { closeDialog(); });
    mkButton(btns, verb, BTN_PRIMARY, onPromptOk);
    if (lcdKeyboardOnScreen()) lcdKeyboardOpen(s_dialogTa);
    else deferFocus(s_dialogTa);
    updateSpaceClaim();
}

/* ---- screens ---- */

enum Screen { SC_LIST, SC_SERVER, SC_ROOM, SC_ROOMS, SC_INFO, SC_MEMBER };
Screen g_screen = SC_LIST;
bool   g_shown = false;

lv_obj_t* s_list = nullptr;     /* servers (built once) */
lv_obj_t* s_server = nullptr;   /* a server (rebuilt per open) */
lv_obj_t* s_room = nullptr;     /* the room (built once) */
lv_obj_t* s_roomsPg = nullptr;  /* room list (rebuilt per open) */
lv_obj_t* s_info = nullptr;     /* info (rebuilt per open) */
lv_obj_t* s_member = nullptr;   /* member card (rebuilt per open) */

void openServer(const std::string& dest);
void openRoom();
void openRooms();
void openInfo();
void openMember(const std::string& mid);
void rebuildList();
void refreshServer();
void refreshRoom();
void refreshRoomsPg();
void refreshInfo();
void pttRelease();

void dropPage(lv_obj_t*& o) {
    if (o) lv_obj_delete(o);
    o = nullptr;
}

void showScreen(Screen sc) {
    if (g_screen == SC_ROOM && sc != SC_ROOM) pttRelease();
    g_screen = sc;
    lv_obj_t* want = sc == SC_LIST ? s_list : sc == SC_SERVER ? s_server : sc == SC_ROOM ? s_room
                   : sc == SC_ROOMS ? s_roomsPg : sc == SC_INFO ? s_info : s_member;
    for (lv_obj_t* o : { s_list, s_server, s_room, s_roomsPg, s_info, s_member }) {
        if (!o) continue;
        showIf(o, o == want);
    }
    if (s_toast) lv_obj_move_foreground(s_toast);
    updateSpaceClaim();
}

/* ---- the Space claim ----
 * Claimed while the room screen is up, the app is in the foreground, the
 * screen is awake, no dialog stands over it and no text field holds the
 * focus; released the moment any of that stops being true. */

bool g_spaceClaimed = false;

bool textFocused() {
    lv_group_t* g = lcdInputGroup();
    lv_obj_t* f = g ? lv_group_get_focused(g) : nullptr;
    return f && lv_obj_check_type(f, &lv_textarea_class) && onScreenNow(f);
}

void updateSpaceClaim() {
    bool want = s_layer && g_shown && g_screen == SC_ROOM && !s_dialog &&
                storageGetInt("sys.standby", 0) == 0 && !textFocused();
    if (want == g_spaceClaimed) return;
    g_spaceClaimed = want;
    lcdKeyClaim(LCD_KEY_SPACE, want);
}

void releaseSpace() {
    if (!g_spaceClaimed) return;
    g_spaceClaimed = false;
    lcdKeyClaim(LCD_KEY_SPACE, false);
}

/* ---- servers list ---- */

lv_obj_t* s_listHdrRight = nullptr;
lv_obj_t* s_listRows = nullptr;
lv_obj_t* s_addTa = nullptr;
bool g_listStale = false;

enum RowKind { RK_CUR, RK_SAVED, RK_HEARD, RK_SECT, RK_EMPTY, RK_ADD, RK_ROOM, RK_MEMBER };
struct RowSpec {
    RowKind kind = RK_EMPTY;
    std::string key, sig, target, title, right, sub;
    int dot = DOT_NONE;
    uint32_t color = 0xFFFFFF, rightColor = C_SUB;
    bool checked = false, speaking = false;
};
struct ListRow { std::string key, sig; lv_obj_t* obj = nullptr; };
std::vector<ListRow> g_rowsL, g_rowsM, g_rowsR;

typedef lv_obj_t* (*row_builder_t)(lv_obj_t* list, const RowSpec& s);

/* Reconcile `list` to `target`: a row whose key and signature still match is
 * kept, a changed one is recreated, a gone one deleted, the rest reordered. */
void applyRows(lv_obj_t* list, std::vector<ListRow>& model, const std::vector<RowSpec>& target, row_builder_t build) {
    std::map<std::string, size_t> have;
    for (size_t i = 0; i < model.size(); i++) if (model[i].obj) have.emplace(model[i].key, i);
    std::vector<char> used(model.size(), 0);
    std::vector<ListRow> out;
    out.reserve(target.size());
    for (auto& t : target) {
        lv_obj_t* obj = nullptr;
        auto it = have.find(t.key);
        if (it != have.end() && !used[it->second]) {
            used[it->second] = 1;
            ListRow& ex = model[it->second];
            if (ex.sig == t.sig) obj = ex.obj;
            else {
                if (s_addTa && lv_obj_get_parent(s_addTa) == ex.obj) s_addTa = nullptr;
                lv_obj_delete(ex.obj);
            }
        }
        out.push_back({ t.key, t.sig, obj });
    }
    for (size_t i = 0; i < model.size(); i++) {
        if (used[i] || !model[i].obj) continue;
        if (s_addTa && lv_obj_get_parent(s_addTa) == model[i].obj) s_addTa = nullptr;
        lv_obj_delete(model[i].obj);
    }
    model.swap(out);
    int32_t sy = lv_obj_get_scroll_y(list);
    for (size_t i = 0; i < model.size(); i++)
        if (!model[i].obj) model[i].obj = build(list, target[i]);
    int32_t idx = 0;
    for (auto& r : model) if (r.obj) lv_obj_move_to_index(r.obj, idx++);
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, sy, LV_ANIM_OFF);
}

RowSpec sectSpec(const std::string& t) {
    RowSpec s; s.kind = RK_SECT; s.key = "\x01.s." + t; s.sig = t; s.title = t; return s;
}
RowSpec emptySpec(const std::string& t) {
    RowSpec s; s.kind = RK_EMPTY; s.key = "\x01.e." + t; s.sig = t; s.title = t; return s;
}

void onServerRow(lv_event_t* e) { openServer(boundString(e)); }
void onCurRow(lv_event_t*) { openRoom(); }

void onAddServer(lv_event_t*) {
    if (!s_addTa) return;
    std::string h = lower(lv_textarea_get_text(s_addTa));
    h.erase(std::remove(h.begin(), h.end(), ' '), h.end());
    if (!isHex32(h)) {
        lv_obj_set_style_border_color(s_addTa, lv_color_hex(C_AMBER), 0);
        return;
    }
    storageSet("partyline.cmd.add", h.c_str());
    lv_textarea_set_text(s_addTa, "");
    openServer(h);
}

lv_obj_t* buildListRow(lv_obj_t* list, const RowSpec& s) {
    switch (s.kind) {
    case RK_SECT:
        return mkSection(list, s.title);
    case RK_EMPTY: {
        lv_obj_t* l = mkWrapFull(list, s.title, C_SUB, kFont);
        lv_obj_set_style_pad_all(l, lcdPx(6), 0);
        return l;
    }
    case RK_ADD: {
        lv_obj_t* row = mkRowBox(list, lcdPx(6));
        lv_obj_set_style_pad_all(row, lcdPx(6), 0);
        s_addTa = mkField(row, "+ Add by hash (32 hex)", false);
        lv_textarea_set_accepted_chars(s_addTa, "0123456789abcdefABCDEF ");
        lv_textarea_set_max_length(s_addTa, 64);
        lv_obj_set_style_text_color(s_addTa, lv_color_hex(0x7FA6FF), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_add_event_cb(s_addTa, onAddServer, LV_EVENT_READY, nullptr);
        lv_obj_add_event_cb(s_addTa, [](lv_event_t*) {
            if (s_addTa) lv_obj_set_style_border_color(s_addTa, lv_color_hex(C_FIELD), 0);
        }, LV_EVENT_VALUE_CHANGED, nullptr);
        mkButton(row, "Add", BTN_PRIMARY, onAddServer);
        return row;
    }
    case RK_CUR: {
        lv_obj_t* row = mkRow(list);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_event_cb(row, onCurRow, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* l1 = mkRowBox(row, lcdPx(6));
        mkDot(l1, s.dot);
        mkFill(l1, s.title, 0xFFFFFF, kBold);
        mkLabel(l1, LV_SYMBOL_RIGHT, C_ACT, kSmall);
        lv_obj_t* l2 = mkLabel(row, s.sub, C_SUB, kSmall);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l2, lv_pct(100));
        lv_obj_set_height(l2, lv_font_get_line_height(kSmall));
        return row;
    }
    default: {
        lv_obj_t* row = mkRow(list);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, lcdPx(6), 0);
        lv_obj_set_style_pad_ver(row, lcdPx(4), 0);
        bindString(row, onServerRow, s.target);
        mkDot(row, s.dot);
        mkFill(row, s.title, s.color, s.kind == RK_SAVED ? kBold : kFont);
        if (!s.right.empty()) mkLabel(row, s.right, s.rightColor, kSmall);
        return row;
    }
    }
}

int serverDot(const std::string& d) {
    if (d != plServer()) return DOT_NONE;
    std::string st = plState();
    if (st == "live") return DOT_LIVE;
    if (st == "denied") return DOT_BAD;
    return stateActive(st) ? DOT_HOLLOW : DOT_NONE;
}

void rebuildList() {
    if (!s_listRows) return;
    refreshFont();
    g_listStale = false;
    std::vector<RowSpec> t;
    std::string held = plServer();
    std::string st = plState();
    if (!held.empty() && st != "idle") {
        RowSpec s;
        s.kind = RK_CUR; s.key = "\x01.cur";
        std::string rid = curRoomId();
        s.title = (rid.empty() ? std::string("Connecting") : roomName(rid)) + DOT + serverName(held);
        s.sub = plStatus();
        s.dot = serverDot(held);
        s.sig = s.title + "|" + s.sub + "|" + std::to_string(s.dot);
        t.push_back(sectSpec("NOW"));
        t.push_back(std::move(s));
    }
    std::vector<std::pair<std::string, std::string>> saved;
    for (auto& [d, s] : g_saved) saved.push_back({ lower(serverName(d)), d });
    std::sort(saved.begin(), saved.end());
    if (!saved.empty()) t.push_back(sectSpec("SAVED"));
    for (auto& [ln, d] : saved) {
        RowSpec s;
        s.kind = RK_SAVED; s.key = "s." + d; s.target = d;
        s.title = serverName(d);
        s.dot = serverDot(d);
        auto h = g_heard.find(d);
        if (d == held && st != "idle") s.right = plStatus();
        else if (h != g_heard.end()) s.right = accessMarks(h->second.flags);
        s.rightColor = (d == held && st == "denied") ? C_AMBER : C_SUB;
        s.sig = "S|" + s.title + "|" + s.right + "|" + std::to_string(s.dot);
        t.push_back(std::move(s));
    }
    std::vector<const Heard*> heard;
    for (auto& [d, h] : g_heard) if (!isSaved(d)) heard.push_back(&h);
    std::sort(heard.begin(), heard.end(), [](const Heard* a, const Heard* b) { return a->seen > b->seen; });
    if (!heard.empty()) t.push_back(sectSpec("ON THE MESH"));
    long now = monoS();
    for (auto* h : heard) {
        RowSpec s;
        s.kind = RK_HEARD; s.key = "m." + h->dest; s.target = h->dest;
        s.title = serverName(h->dest);
        if (printable(h->name, true).empty()) s.color = C_SUB;
        std::string marks = accessMarks(h->flags);
        s.right = hopsText(h->hops) + DOT + age(now - h->seen) + (marks.empty() ? "" : DOT + marks);
        s.rightColor = marks.empty() ? C_SUB : C_AMBER;
        s.dot = serverDot(h->dest);
        /* Age alone never churns a row. */
        s.sig = "M|" + s.title + "|" + std::to_string(h->hops) + "|" + marks + "|" + std::to_string(s.dot);
        t.push_back(std::move(s));
    }
    if (saved.empty() && heard.empty())
        t.push_back(emptySpec("No servers saved or heard yet. Servers appear here as they announce."));
    RowSpec a; a.kind = RK_ADD; a.key = "\x01.add"; a.sig = "add";
    t.push_back(std::move(a));
    applyRows(s_listRows, g_rowsL, t, buildListRow);
    if (s_listHdrRight) lv_label_set_text(s_listHdrRight, held.empty() || st == "idle" ? "" : st.c_str());
}

void buildList() {
    s_list = mkScreen();
    Header h = mkHeader(s_list, nullptr, nullptr);
    setTitleLine(h.line, "Partyline", "");
    s_listHdrRight = h.right;
    s_listRows = mkBody(s_list);
    lv_obj_set_style_pad_hor(s_listRows, 0, 0);
}

void closePages() {
    closeDialog();
    dropPage(s_server);
    dropPage(s_roomsPg);
    dropPage(s_info);
    dropPage(s_member);
    g_rowsR.clear();
}

void goList() {
    closePages();
    showScreen(SC_LIST);
    if (g_listStale) rebuildList();
}

/* ---- server screen ---- */

std::string g_server;                   /* the server on the server screen */
std::string g_wantRoom;                 /* a Connect made here: open the room once live */
Header    g_srvHdr;
lv_obj_t* s_srvStatus = nullptr;
lv_obj_t* s_srvAs = nullptr;
lv_obj_t* s_srvPick = nullptr;
lv_obj_t* s_srvRoomTa = nullptr;
lv_obj_t* s_srvRoomPwTa = nullptr;
lv_obj_t* s_srvPwRow = nullptr;
lv_obj_t* s_srvPwTa = nullptr;
lv_obj_t* s_srvMotdBox = nullptr;
lv_obj_t* s_srvMotd = nullptr;
lv_obj_t* s_srvConn = nullptr;
lv_obj_t* s_srvConnLbl = nullptr;
lv_obj_t* s_srvOpen = nullptr;
lv_obj_t* s_srvForget = nullptr;
std::string g_pickSel;                  /* identity path chosen in the picker */
bool g_pickOpen = false;

void reflectAs() {
    if (!s_srvAs) return;
    std::string sel = g_pickSel;
    if (sel.empty()) { auto s = g_saved.find(g_server); if (s != g_saved.end()) sel = s->second.ident; }
    if (sel.empty()) sel = identDefault();
    std::string label = identLabel(sel);
    lv_label_set_text(s_srvAs, (label.empty() ? std::string("Choose below") : label + DOT + "change").c_str());
}

void onPickRow(lv_event_t* e) {
    g_pickSel = boundString(e);
    if (s_srvPick) {
        uint32_t n = lv_obj_get_child_count(s_srvPick);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t* c = lv_obj_get_child(s_srvPick, (int32_t)i);
            if (c == lv_event_get_current_target_obj(e)) lv_obj_add_state(c, LV_STATE_CHECKED);
            else lv_obj_remove_state(c, LV_STATE_CHECKED);
        }
    }
    reflectAs();
}

void buildPicker(bool show) {
    if (!s_srvPick) return;
    lv_obj_clean(s_srvPick);
    g_pickOpen = show;
    showIf(s_srvPick, show);
    if (!show) return;
    mkSection(s_srvPick, "CONNECT AS");
    lv_obj_t* cap = mkWrapFull(s_srvPick, "The server and the room know you by this identity. "
                                          "It is kept for this server.", C_SUB, kSmall);
    lv_obj_set_style_pad_hor(cap, lcdPx(6), 0);
    std::string cur = g_pickSel;
    if (cur.empty()) { auto s = g_saved.find(g_server); if (s != g_saved.end()) cur = s->second.ident; }
    for (auto& c : identChoices()) {
        lv_obj_t* row = mkRow(s_srvPick);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        bindString(row, onPickRow, c.path);
        mkLabel(row, c.label, 0xFFFFFF, kBold);
        mkLabel(row, c.sub, C_SUB, kSmall);
        if (c.path == cur) lv_obj_add_state(row, LV_STATE_CHECKED);
    }
}

void onSrvConn(lv_event_t*) {
    if (g_server.empty()) return;
    if (g_server == plServer() && stateActive(plState())) {
        storageSet("partyline.cmd.disconnect", "1");
        return;
    }
    std::string ident = g_pickSel;
    auto sv = g_saved.find(g_server);
    if (ident.empty() && sv != g_saved.end()) ident = sv->second.ident;
    if (ident.empty()) ident = identDefault();
    if (ident.empty()) {
        if (!g_pickOpen) buildPicker(true);
        showToast("Choose who to connect as first.", C_AMBER);
        return;
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "server", g_server.c_str());
    auto addField = [o](const char* name, lv_obj_t* ta) {
        if (!ta) return;
        std::string v = trim(lv_textarea_get_text(ta));
        if (!v.empty()) cJSON_AddStringToObject(o, name, v.c_str());
    };
    addField("room", s_srvRoomTa);
    addField("password", s_srvRoomPwTa);
    addField("server_password", s_srvPwTa);
    cJSON_AddStringToObject(o, "ident", ident.c_str());
    sendJson("partyline.cmd.connect", o);
    if (s_srvRoomPwTa) lv_textarea_set_text(s_srvRoomPwTa, "");
    if (s_srvPwTa) lv_textarea_set_text(s_srvPwTa, "");
    g_wantRoom = g_server;
    if (g_pickOpen) buildPicker(false);
}

void onSrvForget(lv_event_t*) {
    if (g_server.empty()) return;
    showConfirm("Forget " + serverName(g_server) + "? Its saved passwords go too.", "Forget",
                [](lv_event_t*) {
        std::string d = g_server;
        closeDialog();
        if (d == plServer() && stateActive(plState())) storageSet("partyline.cmd.disconnect", "1");
        storageSet("partyline.cmd.forget", d.c_str());
        goList();
    });
}

void refreshServer() {
    if (!s_server || g_server.empty()) return;
    bool held = g_server == plServer();
    std::string st = held ? plState() : "idle";
    auto h = g_heard.find(g_server);
    int hops = h != g_heard.end() ? h->second.hops : 0;
    setTitleLine(g_srvHdr.line, serverName(g_server), hops > 0 ? DOT + hopsText(hops) : "");
    if (s_srvStatus) {
        std::string text = held && st != "idle" ? plStatus() : "Not connected";
        if (text.empty()) text = st;
        lv_label_set_text(s_srvStatus, text.c_str());
        lv_obj_set_style_text_color(s_srvStatus, lv_color_hex(st == "live" ? C_OK : st == "denied" ? C_AMBER : 0xFFFFFF), 0);
    }
    reflectAs();
    std::string motd = held ? printable(storageGetStr("partyline.motd", ""), false) : "";
    if (s_srvMotd) lv_label_set_text(s_srvMotd, motd.c_str());
    showIf(s_srvMotdBox, !motd.empty());
    bool active = held && stateActive(st);
    if (s_srvConnLbl) lv_label_set_text(s_srvConnLbl, active ? "Disconnect" : "Connect");
    showIf(s_srvOpen, held && st == "live");
    showIf(s_srvForget, isSaved(g_server));
}

void openServer(const std::string& dest) {
    refreshFont();
    closePages();
    g_server = dest;
    g_pickSel.clear();
    g_pickOpen = false;

    s_server = mkScreen();
    g_srvHdr = mkHeader(s_server, [](lv_event_t*) { goList(); }, nullptr);
    lv_obj_t* body = mkBody(s_server);

    mkKv(body, "Hash", fours(dest), C_SUB, kSmall);
    auto h = g_heard.find(dest);
    if (h != g_heard.end()) {
        std::string about = printable(h->second.desc, true);
        std::string where = printable(h->second.lang, true);
        std::string c = printable(h->second.country, true);
        if (!c.empty()) where += (where.empty() ? "" : DOT) + c;
        if (!where.empty()) about += (about.empty() ? "" : DOT) + where;
        if (!about.empty()) mkKv(body, "About", about, 0xFFFFFF, kFont);
        std::string marks = accessMarks(h->second.flags);
        mkKv(body, "Access", marks.empty() ? "open" : "needs " + marks, marks.empty() ? 0xFFFFFF : C_AMBER, kFont);
    }
    s_srvStatus = mkKv(body, "Status", "", 0xFFFFFF, kFont);

    lv_obj_t* asRow = nullptr;
    s_srvAs = mkKv(body, "As", "", 0xFFFFFF, kFont, &asRow);
    lv_obj_add_flag(asRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(asRow, [](lv_event_t*) { buildPicker(!g_pickOpen); }, LV_EVENT_CLICKED, nullptr);

    s_srvPick = mkColBox(body);

    mkSection(body, "JOIN");
    auto sv = g_saved.find(dest);
    lv_obj_t* r1 = mkRowBox(body, 0);
    s_srvRoomTa = mkField(r1, "Room (the server's default)", false);
    if (sv != g_saved.end() && !sv->second.room.empty()) lv_textarea_set_text(s_srvRoomTa, sv->second.room.c_str());
    lv_obj_t* r2 = mkRowBox(body, 0);
    s_srvRoomPwTa = mkField(r2, "Room password, if it has one", true);
    s_srvPwRow = mkRowBox(body, 0);
    bool pwSaved = storageGetInt(("partyline.servers." + dest + ".saved_pw").c_str(), 0) != 0;
    s_srvPwTa = mkField(s_srvPwRow, pwSaved ? "Server password (saved)" : "Server password", true);
    bool needPw = h == g_heard.end() || (h->second.flags & 2) || pwSaved;
    showIf(s_srvPwRow, needPw);
    lv_obj_add_event_cb(s_srvRoomTa, onSrvConn, LV_EVENT_READY, nullptr);

    s_srvMotdBox = mkColBox(body);
    mkSection(s_srvMotdBox, "MESSAGE OF THE DAY");
    s_srvMotd = mkWrapFull(s_srvMotdBox, "", C_ACT, kSmall);
    lv_obj_set_style_pad_hor(s_srvMotd, lcdPx(6), 0);

    lv_obj_t* bar = mkActionBar(s_server);
    s_srvForget = mkButton(bar, "Forget", BTN_DANGER, onSrvForget);
    mkSpacer(bar);
    s_srvConn = mkButton(bar, "Connect", BTN_PRIMARY, onSrvConn, nullptr, &s_srvConnLbl);
    s_srvOpen = mkButton(bar, "Open room", BTN_GHOST, [](lv_event_t*) { openRoom(); });

    bool ask = (sv == g_saved.end() || sv->second.ident.empty()) && identDefault().empty();
    buildPicker(ask);

    showScreen(SC_SERVER);
    refreshServer();
    deferFocus(s_srvConn);
}

/* ---- room screen ---- */

Header    g_roomHdr;
lv_obj_t* s_roomNote = nullptr;         /* status while not live */
lv_obj_t* s_talk = nullptr;             /* talk bar: indicator and hold button */
lv_obj_t* s_talkLbl = nullptr;
lv_obj_t* s_muteBtn = nullptr;
lv_obj_t* s_muteLbl = nullptr;
lv_obj_t* s_deafBtn = nullptr;
lv_obj_t* s_deafLbl = nullptr;
lv_obj_t* s_roomsBtn = nullptr;
lv_obj_t* s_memCol = nullptr;           /* members (scrolls) */
lv_obj_t* s_log = nullptr;              /* the log's scroll container */
lv_obj_t* s_logCol = nullptr;
lv_obj_t* s_compose = nullptr;
lv_obj_t* s_count = nullptr;
bool g_pttHeld = false;                 /* the talk bar is held */
bool g_canTalk = false;                 /* the talk bar keys the transmitter */

void pttRelease() {
    if (!g_pttHeld) return;
    g_pttHeld = false;
    storageSet("lxst.cmd.ptt", "0");
}

/* What the talk bar says and whether holding it talks. */
void refreshTalk() {
    if (!s_talk || !s_talkLbl) return;
    bool live = plState() == "live" && !curRoomId().empty();
    bool audio = storageGetInt("partyline.audio", 0) != 0;
    bool can = storageGetInt("partyline.can_speak", 0) != 0;
    bool talking = storageGetInt("lxst.talking", 0) != 0;
    std::string tx = storageGetStr("s.lxst.tx", "ptt");
    const Member* me = selfMember();
    bool srvMuted = me && me->serverMuted;
    std::string route = printable(storageGetStr("partyline.tx_blocked", ""), true);

    std::string text;
    uint32_t bg = C_BAR, fg = C_SUB, border = C_RULE;
    g_canTalk = false;
    if (!live)                     text = "Not in a room";
    else if (!audio)               text = "text only";
    else if (!can || srvMuted)     text = srvMuted ? "listen only" + std::string(DOT) + "muted by the server" : "listen only";
    else if (!route.empty())       { text = route; fg = C_AMBER; }
    else if (selfMuted())          text = "Muted" + std::string(DOT) + "unmute to talk";
    else if (talking) {
        text = LV_SYMBOL_AUDIO "  Talking";
        bg = C_OK; fg = 0x0A0F0A; border = C_OK;
    } else if (storageGetStr("lxst.state", "idle") != "live") {
        text = "Voice starting\xE2\x80\xA6";
    } else if (tx == "vox") {
        text = "Voice activated"; fg = C_ACT; border = C_FIELD; bg = C_TALKRDY;
    } else if (tx == "open") {
        text = "Open mic"; fg = C_ACT; border = C_FIELD; bg = C_TALKRDY;
    } else {
        bool mic = lcdKeyDeclared(LCD_KEY_PTT), space = lcdKeyDeclared(LCD_KEY_SPACE);
        text = mic && space ? "Hold Mic or Space to talk" : mic ? "Hold Mic to talk"
             : space ? "Hold Space or here to talk" : "Hold here to talk";
        fg = C_ACT; border = C_ACCENT; bg = C_TALKRDY;
        g_canTalk = true;
    }
    if (talking && live && audio) g_canTalk = g_canTalk || tx == "ptt" || tx.empty();
    if (g_pttHeld && !talking && g_canTalk) bg = C_TALKDN;
    if (!g_canTalk) pttRelease();
    lv_label_set_text(s_talkLbl, text.c_str());
    lv_obj_set_style_text_color(s_talkLbl, lv_color_hex(fg), 0);
    lv_obj_set_style_bg_color(s_talk, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(s_talk, lv_color_hex(border), 0);

    bool toggles = live && audio;
    showIf(s_muteBtn, toggles);
    showIf(s_deafBtn, toggles);
    if (toggles) {
        bool m = selfMuted(), d = selfDeaf();
        lv_label_set_text(s_muteLbl, m ? "Unmute" : "Mute");
        lv_label_set_text(s_deafLbl, d ? "Undeafen" : "Deafen");
        if (m) lv_obj_add_state(s_muteBtn, LV_STATE_CHECKED); else lv_obj_remove_state(s_muteBtn, LV_STATE_CHECKED);
        if (d) lv_obj_add_state(s_deafBtn, LV_STATE_CHECKED); else lv_obj_remove_state(s_deafBtn, LV_STATE_CHECKED);
    }
}

void onTalkEvent(lv_event_t* e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        if (!g_canTalk || g_pttHeld) return;
        g_pttHeld = true;
        storageSet("lxst.cmd.ptt", "1");
        refreshTalk();
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        pttRelease();
        refreshTalk();
    }
}

/* Room widgets that hold the focus pass a typed character to the composer:
 * typing starts a message without first reaching for the field. */
void focusCompose() {
    if (!s_compose || !lcdInputGroup()) return;
    lv_group_focus_obj(s_compose);
    lcdInputBoxActivate(s_compose);
    updateSpaceClaim();
}
void onRoomKey(lv_event_t* e) {
    uint32_t k = lv_event_get_key(e);
    if (k < 0x21 || k == 0x7F || !s_compose) return;
    focusCompose();
    lv_obj_send_event(s_compose, LV_EVENT_KEY, &k);
}
void roomKeys(lv_obj_t* o) { if (o) lv_obj_add_event_cb(o, onRoomKey, LV_EVENT_KEY, nullptr); }

/* Out of the composer, so Space talks again. */
void leaveCompose() {
    if (!s_roomsBtn) return;
    lcdCaretRelease();
    lcdKeyboardClose();
    if (lcdInputGroup()) lv_group_focus_obj(s_roomsBtn);
    updateSpaceClaim();
}

void onMute(lv_event_t*) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "muted", selfMuted() ? 0 : 1);
    sendJson("partyline.cmd.state", o);
}
void onDeaf(lv_event_t*) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "deaf", selfDeaf() ? 0 : 1);
    sendJson("partyline.cmd.state", o);
}

/* -- members -- */

void onMemberRow(lv_event_t* e) { openMember(boundString(e)); }

lv_color_t memberColor(const Member& m) {
    return hsl(peerHue(m.identity.empty() ? m.name : m.identity), 70, 72);
}
std::string memberName(const Member& m) {
    std::string n = printable(m.name, true);
    return n.empty() ? m.identity.substr(0, 8) : n;
}

lv_obj_t* buildMemberRow(lv_obj_t* list, const RowSpec& s) {
    if (s.kind == RK_EMPTY) {
        lv_obj_t* l = mkWrapFull(list, s.title, C_SUB, kSmall);
        lv_obj_set_style_pad_all(l, lcdPx(4), 0);
        return l;
    }
    lv_obj_t* row = mkRow(list);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, lcdPx(3), 0);
    lv_obj_set_style_pad_hor(row, lcdPx(4), 0);
    bindString(row, onMemberRow, s.target);
    roomKeys(row);
    lv_obj_t* n = mkFill(row, s.title, 0xFFFFFF, kFont);
    lv_obj_set_style_text_color(n, lv_color_hex(s.color), 0);
    if (!s.right.empty()) mkLabel(row, s.right, s.rightColor, kSmall);
    return row;
}

/* Marks after a member's name: muted, deafened, text only. */
std::string memberMarks(const Member& m, uint32_t& color) {
    std::string s;
    color = C_SUB;
    if (m.textOnly) s += LV_SYMBOL_KEYBOARD;
    else if (m.deaf) { s += LV_SYMBOL_MUTE; color = C_AMBER; }
    else if (m.muted || m.serverMuted) s += LV_SYMBOL_MUTE;
    if (m.serverMuted) color = C_FAIL;
    return s;
}

void refreshMembers() {
    if (!s_memCol) return;
    std::string rid = curRoomId();
    std::vector<const Member*> in;
    for (auto& [mid, m] : g_members) if (memberInRoom(m, rid)) in.push_back(&m);
    std::sort(in.begin(), in.end(), [](const Member* a, const Member* b) {
        std::string x = lower(memberName(*a)), y = lower(memberName(*b));
        return x != y ? x < y : a->mid < b->mid;
    });
    std::vector<RowSpec> t;
    for (auto* m : in) {
        RowSpec s;
        s.kind = RK_MEMBER; s.key = "u." + m->mid; s.target = m->mid;
        s.title = (m->op ? "@" : "") + memberName(*m);
        lv_color_t c = memberColor(*m);
        s.color = lv_color_to_u32(c) & 0xFFFFFF;
        s.right = memberMarks(*m, s.rightColor);
        s.speaking = m->speaking != 0 || (m->self && storageGetInt("lxst.talking", 0) != 0);
        s.sig = s.title + "|" + s.right + "|" + std::to_string(s.rightColor) + "|" + std::to_string(s.color) +
                (m->self ? "|me" : "");
        t.push_back(std::move(s));
    }
    if (t.empty()) t.push_back(emptySpec(rid.empty() ? "" : "Nobody here yet"));
    applyRows(s_memCol, g_rowsM, t, buildMemberRow);
    /* Speaking is a style, not a rebuild: it changes many times a second. */
    for (size_t i = 0; i < g_rowsM.size() && i < t.size(); i++) {
        lv_obj_t* o = g_rowsM[i].obj;
        if (!o || t[i].kind != RK_MEMBER) continue;
        bool on = t[i].speaking;
        lv_obj_set_style_bg_color(o, lv_color_hex(C_SPEAK), 0);
        lv_obj_set_style_bg_opa(o, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_side(o, on ? (lv_border_side_t)(LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT)
                                           : LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(o, on ? lcdPx(3) : 1, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(on ? C_OK : C_RULE), 0);
    }
}

/* -- the text log --
 * partyline.log.<n> holds the last 100 lines, n counting up from 0 each boot
 * (older lines are deleted); partyline.log_next is the next n. Lines from the
 * last-seen n up to log_next are appended as they come; a log_next below the
 * last seen one (the count started over) reloads the whole of it, ordered by
 * time and then by n. */

long g_logSeen = 0;                      /* log_next at the last read */
std::map<long, lv_obj_t*> g_logRows;     /* drawn rows by n */
std::vector<long> g_logOrder;            /* drawn n, top to bottom */
std::vector<long> g_logDirty;            /* n whose fields changed */
bool g_logReload = false;

bool readLogLine(long n, LogLine& l) {
    std::string base = "partyline.log." + std::to_string(n) + ".";
    l.n = n;
    l.kind = storageGetStr((base + "kind").c_str(), "");
    l.text = storageGetStr((base + "text").c_str(), "");
    if (l.kind.empty() && l.text.empty()) return false;
    l.ts = (uint32_t)storageGetInt((base + "ts").c_str(), 0);
    l.mid = storageGetStr((base + "mid").c_str(), "");
    l.name = storageGetStr((base + "name").c_str(), "");
    l.room = storageGetStr((base + "room").c_str(), "");
    return true;
}

std::map<long, LogLine>* s_logAcc = nullptr;
void logLeaf(const char* key, const char* val) {
    std::string id; const char* f;
    if (!s_logAcc || !splitRec(key, sizeof("partyline.log.") - 1, id, f)) return;
    char* end = nullptr;
    long n = strtol(id.c_str(), &end, 10);
    if (end == id.c_str() || *end) return;
    LogLine& l = (*s_logAcc)[n];
    l.n = n;
    std::string v = val ? val : "";
    if      (!strcmp(f, "ts"))   l.ts = (uint32_t)strtoul(v.c_str(), nullptr, 10);
    else if (!strcmp(f, "kind")) l.kind = v;
    else if (!strcmp(f, "mid"))  l.mid = v;
    else if (!strcmp(f, "name")) l.name = v;
    else if (!strcmp(f, "room")) l.room = v;
    else if (!strcmp(f, "text")) l.text = v;
}

lv_span_t* addSpan(lv_obj_t* sg, const std::string& t, lv_color_t c, const lv_font_t* f) {
    if (t.empty()) return nullptr;
    lv_span_t* sp = lv_spangroup_add_span(sg);
    lv_style_t* st = lv_span_get_style(sp);
    lv_style_set_text_color(st, c);
    if (f) lv_style_set_text_font(st, f);
    lv_spangroup_set_span_text(sg, sp, t.c_str());
    return sp;
}

std::string lineName(const LogLine& l) {
    auto m = g_members.find(l.mid);
    std::string n = printable(l.name, true);
    if (n.empty() && m != g_members.end()) n = memberName(m->second);
    return n.empty() ? "someone" : n;
}
lv_color_t lineColor(const LogLine& l) {
    auto m = g_members.find(l.mid);
    if (m != g_members.end()) return memberColor(m->second);
    return hsl(peerHue(l.name), 70, 72);
}

lv_obj_t* buildLogRow(const LogLine& l) {
    lv_obj_t* sg = lv_spangroup_create(s_logCol);
    lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(sg, lv_pct(100));
    lv_obj_set_height(sg, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(sg, kFont, 0);
    lv_obj_set_style_text_color(sg, lv_color_white(), 0);
    lv_obj_set_style_pad_ver(sg, 1, 0);
    std::string text = printable(l.text, false);
    std::string t = hhmm(l.ts);
    if (!t.empty()) addSpan(sg, t + " ", lv_color_hex(C_GUTTER), kSmall);
    if (l.kind == "poke") {
        lv_obj_set_style_border_side(sg, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(sg, lcdPx(2), 0);
        lv_obj_set_style_border_color(sg, lv_color_hex(C_AMBER), 0);
        lv_obj_set_style_pad_left(sg, lcdPx(3), 0);
        addSpan(sg, LV_SYMBOL_BELL " " + lineName(l) + " poked you", lv_color_hex(C_AMBER), kBold);
        if (!text.empty()) addSpan(sg, ": " + text, lv_color_hex(C_AMBER), kFont);
    } else if (l.kind == "notice") {
        addSpan(sg, text, lv_color_hex(C_NOTE), kSmall);
    } else if (l.kind == "system") {
        addSpan(sg, text, lv_color_hex(C_PRES), kSmall);
    } else {
        std::string rid = curRoomId();
        if (!l.room.empty() && l.room != rid && roomName(rid) != printable(l.room, true))
            addSpan(sg, roomName(l.room) + " ", lv_color_hex(C_PRES), kSmall);
        addSpan(sg, lineName(l), lineColor(l), kBold);
        addSpan(sg, " " + text, lv_color_white(), kFont);
    }
    lv_spangroup_refresh(sg);
    return sg;
}

bool logAtBottom() { return s_log && lv_obj_get_scroll_bottom(s_log) <= 4; }
void logScrollBottom() {
    if (!s_log) return;
    lv_obj_update_layout(s_log);
    lv_obj_scroll_to_y(s_log, LV_COORD_MAX, LV_ANIM_OFF);
}

void appendLogRow(const LogLine& l) {
    auto ex = g_logRows.find(l.n);
    if (ex != g_logRows.end()) {
        /* A line already drawn: its row is rebuilt at the end. */
        lv_obj_delete(ex->second);
        g_logRows.erase(ex);
        g_logOrder.erase(std::remove(g_logOrder.begin(), g_logOrder.end(), l.n), g_logOrder.end());
    }
    g_logRows[l.n] = buildLogRow(l);
    g_logOrder.push_back(l.n);
    while (g_logOrder.size() > LOG_MAX) {
        long old = g_logOrder.front();
        g_logOrder.erase(g_logOrder.begin());
        auto it = g_logRows.find(old);
        if (it != g_logRows.end()) { lv_obj_delete(it->second); g_logRows.erase(it); }
    }
}

void reloadLog() {
    if (!s_logCol) return;
    lv_obj_clean(s_logCol);
    g_logRows.clear();
    g_logOrder.clear();
    g_logDirty.clear();
    std::map<long, LogLine> acc;
    s_logAcc = &acc;
    storageForEach("partyline.log.", logLeaf);
    s_logAcc = nullptr;
    g_logSeen = storageGetInt("partyline.log_next", 0);
    std::vector<const LogLine*> v;
    for (auto& [n, l] : acc) if (!l.kind.empty() || !l.text.empty()) v.push_back(&l);
    long seen = g_logSeen;
    std::sort(v.begin(), v.end(), [seen](const LogLine* a, const LogLine* b) {
        if (a->ts != b->ts) return a->ts < b->ts;
        bool aw = a->n >= seen, bw = b->n >= seen;   /* at or past log_next: the older lap */
        if (aw != bw) return aw;
        return a->n < b->n;
    });
    size_t from = v.size() > LOG_MAX ? v.size() - LOG_MAX : 0;
    for (size_t i = from; i < v.size(); i++) appendLogRow(*v[i]);
    logScrollBottom();
}

void toastLine(const LogLine& l) {
    if (l.kind == "poke")
        showToast(LV_SYMBOL_BELL " " + lineName(l) + " poked you" +
                  (l.text.empty() ? "" : ": " + printable(l.text, true)), C_AMBER);
    else if (l.kind == "notice")
        showToast(printable(l.text, true), C_NOTE);
}

/* Before the room screen is built there is no log to draw into; pokes and
 * notices still toast. */
void toastNewLines() {
    long next = storageGetInt("partyline.log_next", 0);
    g_logDirty.clear();
    g_logReload = false;
    for (long n = g_logSeen; n < next; n++) {
        LogLine l;
        if (readLogLine(n, l)) toastLine(l);
    }
    g_logSeen = next;
}

void patchLog() {
    if (!s_logCol) return;
    long next = storageGetInt("partyline.log_next", 0);
    if (g_logReload || next < g_logSeen) {
        g_logReload = false;
        reloadLog();
        return;
    }
    bool bottom = logAtBottom();
    bool grew = false;
    for (long n : g_logDirty) {
        if (n >= g_logSeen) continue;               /* drawn below as it arrives */
        auto it = g_logRows.find(n);
        if (it == g_logRows.end()) continue;
        LogLine l;
        if (!readLogLine(n, l)) continue;
        int32_t idx = lv_obj_get_index(it->second);
        lv_obj_t* o = buildLogRow(l);
        lv_obj_move_to_index(o, idx);
        lv_obj_delete(it->second);
        it->second = o;
    }
    g_logDirty.clear();
    for (long n = g_logSeen; n < next; n++) {
        LogLine l;
        if (!readLogLine(n, l)) continue;
        appendLogRow(l);
        grew = true;
        toastLine(l);
    }
    g_logSeen = next;
    if (grew && bottom) logScrollBottom();
}

/* -- composer -- */

void reflectCount() {
    if (!s_count || !s_compose) return;
    int left = TEXT_LIMIT - (int)strlen(lv_textarea_get_text(s_compose));
    if (left > 40) { lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN); return; }
    lv_label_set_text_fmt(s_count, "%d", left);
    lv_obj_set_style_text_color(s_count, lv_color_hex(left < 0 ? C_FAIL : C_AMBER), 0);
    lv_obj_remove_flag(s_count, LV_OBJ_FLAG_HIDDEN);
}

void onSend(lv_event_t*) {
    if (!s_compose) return;
    std::string t = lcdInputBoxText(s_compose);
    if (t.empty()) { leaveCompose(); return; }
    if ((int)t.size() > TEXT_LIMIT) { reflectCount(); return; }
    if (curRoomId().empty() || plState() != "live") {
        showToast("Not in a room: the line is kept here.", C_AMBER);
        return;
    }
    storageSet("partyline.cmd.text", t.c_str());
    lv_textarea_set_text(s_compose, "");
    reflectCount();
    lcdKeyboardClose();
    logScrollBottom();
}

/* Up/down scroll the log while the draft is one line. */
void onComposeKey(lv_event_t* e) {
    uint32_t* kp = (uint32_t*)lv_event_get_param(e);
    if (!kp || !s_log || !s_compose) return;
    if (*kp != LV_KEY_UP && *kp != LV_KEY_DOWN) return;
    if (strchr(lv_textarea_get_text(s_compose), '\n')) return;
    int step = lv_font_get_line_height(kFont) * 3;
    lv_obj_scroll_by_bounded(s_log, 0, *kp == LV_KEY_UP ? step : -step, LV_ANIM_OFF);
    *kp = 0;
}

void refreshRoomHeader() {
    if (!s_room) return;
    std::string rid = curRoomId();
    std::string held = plServer();
    std::string title = rid.empty() ? std::string("No room") : roomName(rid);
    setTitleLine(g_roomHdr.line, title, held.empty() ? "" : DOT + serverName(held));
    std::string right;
    auto r = g_rooms.find(rid);
    if (r != g_rooms.end()) {
        if (r->second.pttJitter > 0) right = "PTT";
        std::string prof = printable(r->second.profileText, true);
        if (prof.empty()) prof = printable(storageGetStr("lxst.profile", ""), true);
        if (!prof.empty()) right += (right.empty() ? "" : DOT) + prof;
    }
    lv_label_set_text(g_roomHdr.right, right.c_str());
    if (s_compose) lv_textarea_set_placeholder_text(s_compose, rid.empty() ? "Message" : ("Message " + title).c_str());
    if (s_roomNote) {
        std::string st = plState();
        std::string text = st == "live" ? "" : plStatus();
        if (st != "live" && text.empty()) text = held.empty() ? "Not connected" : st;
        lv_label_set_text(s_roomNote, text.c_str());
        lv_obj_set_style_text_color(s_roomNote, lv_color_hex(st == "denied" || st == "lost" ? C_AMBER : C_SUB), 0);
        showIf(s_roomNote, !text.empty());
    }
}

void refreshRoom() {
    if (!s_room) return;
    refreshRoomHeader();
    refreshTalk();
    refreshMembers();
}

void buildRoomShell() {
    s_room = mkScreen();
    g_roomHdr = mkHeader(s_room, [](lv_event_t*) { goList(); }, [](lv_event_t*) { openInfo(); });
    lv_obj_set_width(g_roomHdr.right, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(g_roomHdr.right, lcdPx(120), 0);
    lv_label_set_long_mode(g_roomHdr.right, LV_LABEL_LONG_MODE_DOTS);
    roomKeys(g_roomHdr.back);

    s_roomNote = mkLabel(s_room, "", C_SUB, kSmall);
    lv_label_set_long_mode(s_roomNote, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_roomNote, lv_pct(100));
    lv_obj_set_style_pad_hor(s_roomNote, lcdPx(6), 0);
    lv_obj_set_style_pad_ver(s_roomNote, lcdPx(2), 0);
    lv_obj_add_flag(s_roomNote, LV_OBJ_FLAG_HIDDEN);

    /* The talk bar, then Mute, Deafen and Rooms. */
    lv_obj_t* tr = mkRowBox(s_room, lcdPx(4));
    lv_obj_set_style_pad_hor(tr, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(tr, lcdPx(3), 0);
    s_talk = lv_button_create(tr);
    lv_obj_remove_style_all(s_talk);
    lv_obj_set_height(s_talk, lcdPx(28));
    lv_obj_set_width(s_talk, 1);
    lv_obj_set_flex_grow(s_talk, 1);
    lv_obj_set_style_radius(s_talk, lcdPx(6), 0);
    lv_obj_set_style_bg_opa(s_talk, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_talk, 1, 0);
    lv_obj_remove_flag(s_talk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_talk, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_ext_click_area(s_talk, lcdPx(4));
    s_talkLbl = mkLabel(s_talk, "", C_SUB, kBold);
    lv_label_set_long_mode(s_talkLbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_talkLbl, lv_pct(96));
    lv_obj_set_style_text_align(s_talkLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_talkLbl);
    lv_obj_add_event_cb(s_talk, onTalkEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_talk, onTalkEvent, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_talk, onTalkEvent, LV_EVENT_PRESS_LOST, nullptr);
    s_muteBtn = mkButton(tr, "Mute", BTN_GHOST, onMute, nullptr, &s_muteLbl);
    s_deafBtn = mkButton(tr, "Deafen", BTN_GHOST, onDeaf, nullptr, &s_deafLbl);
    s_roomsBtn = mkButton(tr, "Rooms", BTN_GHOST, [](lv_event_t*) { openRooms(); });
    for (lv_obj_t* b : { s_muteBtn, s_deafBtn, s_roomsBtn }) {
        lv_obj_set_style_pad_hor(b, lcdPx(6), 0);
        roomKeys(b);
    }

    /* Members on the left, the log on the right. */
    lv_obj_t* mid = mkRowBox(s_room, 0);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_height(mid, 1);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_border_side(mid, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(mid, 1, 0);
    lv_obj_set_style_border_color(mid, lv_color_hex(C_RULE), 0);

    s_memCol = lv_obj_create(mid);
    lv_obj_remove_style_all(s_memCol);
    lv_obj_set_width(s_memCol, membersW());
    lv_obj_set_height(s_memCol, lv_pct(100));
    lv_obj_set_flex_flow(s_memCol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_side(s_memCol, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(s_memCol, 1, 0);
    lv_obj_set_style_border_color(s_memCol, lv_color_hex(C_RULE), 0);

    s_log = lv_obj_create(mid);
    lv_obj_remove_style_all(s_log);
    lv_obj_set_height(s_log, lv_pct(100));
    lv_obj_set_width(s_log, 1);
    lv_obj_set_flex_grow(s_log, 1);
    lv_obj_set_style_pad_hor(s_log, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(s_log, lcdPx(2), 0);
    s_logCol = lv_obj_create(s_log);
    lv_obj_remove_style_all(s_logCol);
    lv_obj_set_size(s_logCol, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_logCol, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_logCol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_logCol, LV_OBJ_FLAG_CLICKABLE);

    /* The composer. */
    lv_obj_t* bar = mkRowBox(s_room, lcdPx(6));
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_RULE), 0);
    lv_obj_set_style_pad_all(bar, lcdPx(3), 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bar, [](lv_event_t*) { focusCompose(); }, LV_EVENT_CLICKED, nullptr);

    s_compose = lcdInputBoxCreate(bar, 1, 3);
    lv_obj_set_style_text_font(s_compose, kFont, 0);
    lv_obj_set_style_bg_opa(s_compose, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(s_compose, lv_color_white(), 0);
    lv_obj_set_style_text_color(s_compose, lv_color_hex(C_PRES), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(s_compose, 1, 0);
    lv_obj_set_style_border_color(s_compose, lv_color_hex(C_FIELD), 0);
    lv_obj_set_style_border_color(s_compose, lv_color_hex(C_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_radius(s_compose, lcdPx(4), 0);
    lv_obj_set_style_pad_ver(s_compose, lcdPx(2), 0);
    lv_obj_set_style_pad_hor(s_compose, lcdPx(6), 0);
    lv_obj_set_width(s_compose, 1);
    lv_obj_set_flex_grow(s_compose, 1);
    lv_obj_add_flag(s_compose, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), s_compose);
    lv_obj_add_event_cb(s_compose, onSend, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_compose, [](lv_event_t*) { reflectCount(); }, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_compose, onComposeKey, (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), nullptr);
    lv_obj_add_event_cb(s_compose, [](lv_event_t*) { updateSpaceClaim(); }, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(s_compose, [](lv_event_t*) { updateSpaceClaim(); }, LV_EVENT_DEFOCUSED, nullptr);

    s_count = mkLabel(bar, "", C_AMBER, kSmall);
    lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN);
    mkButton(bar, "Send", BTN_PRIMARY, onSend);

    reloadLog();
}

void openRoom() {
    refreshFont();
    closePages();
    if (!s_room) buildRoomShell();
    showScreen(SC_ROOM);
    refreshRoom();
    logScrollBottom();
    /* Focus rests off the composer, so Space talks. */
    deferFocus(s_roomsBtn);
}

/* ---- room list ---- */

Header    g_rlHdr;
lv_obj_t* s_rlRows = nullptr;
std::string g_moveTo;                   /* the room a password is being asked for */

void moveTo(const std::string& id, const std::string& password) {
    cJSON* o = cJSON_CreateObject();
    char* end = nullptr;
    long n = strtol(id.c_str(), &end, 10);
    if (end != id.c_str() && !*end) cJSON_AddNumberToObject(o, "room", (double)n);
    else cJSON_AddStringToObject(o, "room", id.c_str());
    if (!password.empty()) cJSON_AddStringToObject(o, "password", password.c_str());
    sendJson("partyline.cmd.move", o);
    openRoom();
}

void onRoomRow(lv_event_t* e) {
    std::string id = boundString(e);
    if (id == curRoomId()) { openRoom(); return; }   /* never a move to where we are */
    auto r = g_rooms.find(id);
    if (r != g_rooms.end() && (r->second.access & 4)) {
        g_moveTo = id;
        bool saved = storageGetInt(("partyline.rooms." + id + ".saved_pw").c_str(), 0) != 0;
        showPrompt(roomName(id) + " has a password.", saved ? "Password (saved)" : "Password", "Move",
                   [](const std::string& pw) { moveTo(g_moveTo, pw); });
        return;
    }
    moveTo(id, "");
}

lv_obj_t* buildRoomRow(lv_obj_t* list, const RowSpec& s) {
    if (s.kind == RK_EMPTY) {
        lv_obj_t* l = mkWrapFull(list, s.title, C_SUB, kFont);
        lv_obj_set_style_pad_all(l, lcdPx(6), 0);
        return l;
    }
    lv_obj_t* row = mkRow(list);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    bindString(row, onRoomRow, s.target);
    lv_obj_t* l1 = mkRowBox(row, lcdPx(6));
    mkFill(l1, s.title, 0xFFFFFF, kBold);
    if (!s.right.empty()) mkLabel(l1, s.right, s.rightColor, kSmall);
    if (!s.sub.empty()) {
        lv_obj_t* l2 = mkLabel(row, s.sub, C_SUB, kSmall);
        lv_label_set_long_mode(l2, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l2, lv_pct(100));
        lv_obj_set_height(l2, lv_font_get_line_height(kSmall));
    }
    if (s.checked) lv_obj_add_state(row, LV_STATE_CHECKED);
    return row;
}

void refreshRoomsPg() {
    if (!s_rlRows) return;
    std::string held = plServer();
    setTitleLine(g_rlHdr.line, "Rooms", held.empty() ? "" : DOT + serverName(held));
    std::string cur = curRoomId();
    std::vector<const RoomRec*> rs;
    for (auto& [id, r] : g_rooms) rs.push_back(&r);
    std::sort(rs.begin(), rs.end(), [](const RoomRec* a, const RoomRec* b) {
        long x = atol(a->id.c_str()), y = atol(b->id.c_str());
        return x != y ? x < y : a->id < b->id;
    });
    std::vector<RowSpec> t;
    for (auto* r : rs) {
        RowSpec s;
        s.kind = RK_ROOM; s.key = "r." + r->id; s.target = r->id;
        s.title = roomName(r->id);
        s.checked = r->id == cur;
        std::string right;
        if (s.checked) right = "here";
        if (r->pttJitter > 0) right += (right.empty() ? "" : DOT) + std::string("PTT");
        if (r->access & 4) { right += (right.empty() ? "" : DOT) + std::string("password"); s.rightColor = C_AMBER; }
        right += (right.empty() ? "" : DOT) + std::to_string(r->members);
        s.right = right;
        std::string prof = printable(r->profileText, true);
        std::string desc = printable(r->desc, true);
        s.sub = prof + (prof.empty() || desc.empty() ? "" : DOT) + desc;
        s.sig = s.title + "|" + s.right + "|" + s.sub + (s.checked ? "|c" : "");
        t.push_back(std::move(s));
    }
    if (t.empty()) t.push_back(emptySpec("The room list arrives once the server is connected."));
    applyRows(s_rlRows, g_rowsR, t, buildRoomRow);
}

void openRooms() {
    refreshFont();
    closePages();
    s_roomsPg = mkScreen();
    g_rlHdr = mkHeader(s_roomsPg, [](lv_event_t*) { openRoom(); }, nullptr);
    s_rlRows = mkBody(s_roomsPg);
    lv_obj_set_style_pad_hor(s_rlRows, 0, 0);
    g_rowsR.clear();
    showScreen(SC_ROOMS);
    refreshRoomsPg();
    lv_obj_t* first = nullptr;
    for (auto& r : g_rowsR) if (r.obj && lv_obj_check_type(r.obj, &lv_button_class)) { first = r.obj; break; }
    deferFocus(first ? first : g_rlHdr.back);
}

/* ---- info: server, room, voice, MOTD ---- */

Header    g_infoHdr;
lv_obj_t* s_infoBody = nullptr;

std::vector<std::pair<std::string, std::string>>* s_statAcc = nullptr;
void statLeaf(const char* key, const char* val) {
    if (!s_statAcc || !key) return;
    s_statAcc->push_back({ key + sizeof("lxst.stats.") - 1, val ? val : "" });
}

void refreshInfo() {
    if (!s_info || !s_infoBody) return;
    lv_obj_clean(s_infoBody);
    std::string held = plServer();
    std::string rid = curRoomId();
    setTitleLine(g_infoHdr.line, rid.empty() ? "Partyline" : roomName(rid), held.empty() ? "" : DOT + serverName(held));
    lv_obj_t* body = s_infoBody;
    if (!held.empty()) {
        mkKv(body, "Server", serverName(held), 0xFFFFFF, kFont);
        mkKv(body, "", fours(held), C_SUB, kSmall);
    }
    std::string st = plState();
    mkKv(body, "Status", plStatus().empty() ? st : plStatus(), st == "live" ? C_OK : 0xFFFFFF, kFont);
    auto r = g_rooms.find(rid);
    if (r != g_rooms.end()) {
        std::string prof = printable(r->second.profileText, true);
        if (r->second.frameMs > 0) prof += (prof.empty() ? "" : DOT) + std::to_string(r->second.frameMs) + " ms frames";
        if (r->second.pttJitter > 0) prof += DOT + std::string("push to talk");
        if (!prof.empty()) mkKv(body, "Profile", prof, 0xFFFFFF, kFont);
        std::string desc = printable(r->second.desc, false);
        if (!desc.empty()) mkKv(body, "About", desc, 0xFFFFFF, kFont);
    }
    const Member* me = selfMember();
    if (me) {
        std::string you = memberName(*me);
        if (me->op) you += DOT + std::string("operator");
        mkKv(body, "You", you, 0xFFFFFF, kFont);
    }
    std::string voice = storageGetStr("lxst.state", "idle");
    if (storageGetInt("partyline.audio", 0) == 0) voice = "text only";
    else {
        std::string p = printable(storageGetStr("lxst.profile", ""), true);
        if (!p.empty()) voice += DOT + p;
    }
    mkKv(body, "Voice", voice, 0xFFFFFF, kFont);

    std::string motd = printable(storageGetStr("partyline.motd", ""), false);
    if (!motd.empty()) {
        mkSection(body, "MESSAGE OF THE DAY");
        lv_obj_t* m = mkWrapFull(body, motd, C_ACT, kSmall);
        lv_obj_set_style_pad_hor(m, lcdPx(6), 0);
    }

    std::vector<std::pair<std::string, std::string>> stats;
    s_statAcc = &stats;
    storageForEach("lxst.stats.", statLeaf);
    s_statAcc = nullptr;
    if (!stats.empty()) {
        mkSection(body, "VOICE COUNTERS");
        for (auto& [k, v] : stats) {
            lv_obj_t* row = mkRowBox(body, lcdPx(4));
            mkFill(row, k, C_SUB, kSmall);
            mkLabel(row, v, 0xFFFFFF, kSmall);
        }
    }
}

void openInfo() {
    refreshFont();
    closePages();
    s_info = mkScreen();
    g_infoHdr = mkHeader(s_info, [](lv_event_t*) { openRoom(); }, nullptr);
    s_infoBody = mkBody(s_info);
    lv_obj_t* bar = mkActionBar(s_info);
    if (!plServer().empty())
        mkButton(bar, "Server", BTN_GHOST, [](lv_event_t*) { if (!plServer().empty()) openServer(plServer()); });
    mkSpacer(bar);
    if (heldActive())
        mkButton(bar, "Disconnect", BTN_DANGER, [](lv_event_t*) {
            showConfirm("Disconnect from " + serverName(plServer()) + "?", "Disconnect", [](lv_event_t*) {
                closeDialog();
                storageSet("partyline.cmd.disconnect", "1");
                goList();
            });
        });
    showScreen(SC_INFO);
    refreshInfo();
    deferFocus(g_infoHdr.back);
}

/* ---- member card ---- */

Header    g_memHdr;
std::string g_memId;
lv_obj_t* s_pokeTa = nullptr;

void onPoke(lv_event_t*) {
    if (g_memId.empty() || !s_pokeTa) return;
    std::string text = trim(lv_textarea_get_text(s_pokeTa));
    cJSON* o = cJSON_CreateObject();
    char* end = nullptr;
    long n = strtol(g_memId.c_str(), &end, 10);
    if (end != g_memId.c_str() && !*end) cJSON_AddNumberToObject(o, "mid", (double)n);
    else cJSON_AddStringToObject(o, "mid", g_memId.c_str());
    cJSON_AddStringToObject(o, "text", text.c_str());
    sendJson("partyline.cmd.poke", o);
    auto m = g_members.find(g_memId);
    showToast("Poked " + (m != g_members.end() ? memberName(m->second) : std::string("them")) + ".", C_ACCENT);
    openRoom();
}

void openMember(const std::string& mid) {
    refreshFont();
    closePages();
    auto it = g_members.find(mid);
    if (it == g_members.end()) { openRoom(); return; }
    const Member m = it->second;
    g_memId = mid;
    s_member = mkScreen();
    g_memHdr = mkHeader(s_member, [](lv_event_t*) { openRoom(); }, nullptr);
    lv_style_set_text_color(lv_span_get_style(g_memHdr.line.title), memberColor(m));
    setTitleLine(g_memHdr.line, memberName(m), m.self ? DOT + std::string("you") : "");
    lv_obj_t* body = mkBody(s_member);
    if (!m.identity.empty()) mkKv(body, "Identity", fours(m.identity), C_SUB, kSmall);
    std::string what;
    auto add = [&what](const char* s) { what += (what.empty() ? "" : DOT) + std::string(s); };
    if (m.op) add("operator");
    if (m.textOnly) add("text only");
    else {
        if (m.serverMuted) add("muted by the server");
        else if (m.muted) add("muted");
        if (m.deaf) add("deafened");
        if (!m.canSpeak) add("listen only");
        if (m.speaking) add("speaking");
    }
    mkKv(body, "Here", what.empty() ? "talking freely" : what, 0xFFFFFF, kFont);
    std::string path;
    if (m.hops > 0) path = hopsText(m.hops);
    std::string rt = rttText(m.rtt);
    if (!rt.empty()) path += (path.empty() ? "" : DOT) + rt;
    if (!path.empty()) mkKv(body, "Path", path, 0xFFFFFF, kFont);

    s_pokeTa = nullptr;
    lv_obj_t* first = g_memHdr.back;
    if (!m.self) {
        mkSection(body, "POKE");
        lv_obj_t* pr = mkRowBox(body, lcdPx(6));
        s_pokeTa = mkField(pr, "A word with the poke (optional)", false);
        lv_textarea_set_max_length(s_pokeTa, 120);
        lv_obj_add_event_cb(s_pokeTa, onPoke, LV_EVENT_READY, nullptr);
        first = mkButton(pr, "Poke", BTN_PRIMARY, onPoke);
    }
    showScreen(SC_MEMBER);
    deferFocus(first);
}

/* ---- live updates ---- */

bool g_dSaved = false, g_dHeard = false, g_dRooms = false, g_dMembers = false, g_dConn = false;
bool g_dLog = false, g_dStats = false;
lv_timer_t* g_metaTimer = nullptr;
lv_timer_t* g_tickTimer = nullptr;
std::string g_lastState;

void metaTimerCb(lv_timer_t*) {
    g_metaTimer = nullptr;
    if (!s_layer) return;
    bool saved = g_dSaved, heard = g_dHeard, rooms = g_dRooms, members = g_dMembers, conn = g_dConn;
    bool log = g_dLog, stats = g_dStats;
    g_dSaved = g_dHeard = g_dRooms = g_dMembers = g_dConn = g_dLog = g_dStats = false;
    refreshFont();
    if (saved) loadSaved();
    if (heard) loadHeard();
    if (rooms) loadRooms();
    if (members) loadMembers();

    /* A Connect made on the server screen opens the room once it is live. */
    std::string st = plState();
    if (conn && st != g_lastState) {
        g_lastState = st;
        if (st == "live" && !g_wantRoom.empty() && g_wantRoom == plServer() && g_screen == SC_SERVER) {
            g_wantRoom.clear();
            openRoom();
        }
        if (st == "idle" || st == "denied") g_wantRoom.clear();
    }

    if (saved || heard || conn || rooms) {
        if (g_screen == SC_LIST) rebuildList();
        else g_listStale = true;
    }
    if (g_screen == SC_SERVER && (saved || heard || conn)) refreshServer();
    if (s_room) {
        if (conn || rooms || members) refreshRoom();
        if (log) patchLog();
    } else if (log) {
        toastNewLines();
    }
    if (g_screen == SC_ROOMS && (rooms || conn)) refreshRoomsPg();
    if (g_screen == SC_INFO && (conn || rooms || stats)) refreshInfo();
}

void scheduleMeta() {
    if (g_metaTimer) return;
    g_metaTimer = lv_timer_create(metaTimerCb, DEBOUNCE_MS, nullptr);
    lv_timer_set_repeat_count(g_metaTimer, 1);
}

void onStorageChange(const char* key, const char* val) {
    if (!s_layer || !key) return;
    if (startsWith(key, "s.partyline.servers.")) g_dSaved = true;
    else if (startsWith(key, "s.partyline.")) g_dConn = true;          /* muted / deaf */
    else if (startsWith(key, "partyline.heard.")) g_dHeard = true;
    else if (startsWith(key, "partyline.rooms.")) g_dRooms = true;
    else if (startsWith(key, "partyline.members.")) g_dMembers = true;
    else if (startsWith(key, "partyline.log.")) {
        std::string id; const char* f;
        if (splitRec(key, sizeof("partyline.log.") - 1, id, f)) g_logDirty.push_back(atol(id.c_str()));
        else g_logReload = true;
        g_dLog = true;
    } else if (!strcmp(key, "partyline.log_next")) g_dLog = true;
    else if (startsWith(key, "partyline.cmd.")) {
        /* A refused command says why on <cmd>.error. */
        if (endsWith(key, ".error") && val && *val) showToast(printable(val, true), C_AMBER);
        return;
    } else if (!strcmp(key, "partyline.tx_blocked")) { refreshTalk(); return; }
    else if (startsWith(key, "partyline.")) g_dConn = true;
    else return;
    scheduleMeta();
}

/* The voice session: the talk bar follows at once; the counters can wait. */
void onLxstChange(const char* key, const char*) {
    if (!s_layer || !key) return;
    if (startsWith(key, "lxst.stats.")) { g_dStats = true; scheduleMeta(); return; }
    if (startsWith(key, "lxst.cmd.")) return;
    refreshTalk();
    if (!strcmp(key, "lxst.talking")) refreshMembers();
    if (!strcmp(key, "lxst.profile")) { g_dConn = true; scheduleMeta(); }
}

void onStandbyChange(const char*, const char*) {
    if (!s_layer) return;
    if (storageGetInt("sys.standby", 0)) pttRelease();
    updateSpaceClaim();
}

/* Once a second: the Space claim follows focus moves made behind our back,
 * and the talk bar follows a key declared after the screen was built. */
void tickCb(lv_timer_t*) {
    if (!s_layer) return;
    updateSpaceClaim();
    if (g_screen == SC_ROOM) refreshTalk();
}

bool g_subscribed = false;

/* ---- entry (lcd task, on a fresh layer) ---- */

void resetHandles() {
    s_list = s_server = s_room = s_roomsPg = s_info = s_member = nullptr;
    s_listHdrRight = s_listRows = s_addTa = nullptr;
    g_rowsL.clear(); g_rowsM.clear(); g_rowsR.clear();
    g_listStale = false;
    s_toast = s_toastLbl = nullptr;
    s_dialog = s_dialogTa = nullptr;
    g_promptOk = nullptr;
    g_server.clear(); g_srvHdr = Header{};
    s_srvStatus = s_srvAs = s_srvPick = s_srvRoomTa = s_srvRoomPwTa = s_srvPwRow = s_srvPwTa = nullptr;
    s_srvMotdBox = s_srvMotd = s_srvConn = s_srvConnLbl = s_srvOpen = s_srvForget = nullptr;
    g_pickSel.clear(); g_pickOpen = false;
    g_roomHdr = Header{};
    s_roomNote = s_talk = s_talkLbl = s_muteBtn = s_muteLbl = s_deafBtn = s_deafLbl = s_roomsBtn = nullptr;
    s_memCol = s_log = s_logCol = s_compose = s_count = nullptr;
    g_logRows.clear(); g_logOrder.clear(); g_logDirty.clear();
    g_logReload = false; g_logSeen = 0;
    g_rlHdr = Header{}; s_rlRows = nullptr;
    g_infoHdr = Header{}; s_infoBody = nullptr;
    g_memHdr = Header{}; g_memId.clear(); s_pokeTa = nullptr;
    g_focusTarget = nullptr;
    g_canTalk = false;
}

void partylineApp(lv_obj_t* root) {
    refreshFont();
    s_layer = root;
    /* A fresh layer: every handle and guard starts over, so a reopen after a
     * stop or an eviction rebuilds instead of trusting widgets that are gone. */
    resetHandles();
    g_metaTimer = nullptr; g_toastTimer = nullptr;
    g_dSaved = g_dHeard = g_dRooms = g_dMembers = g_dConn = g_dLog = g_dStats = false;
    g_screen = SC_LIST;
    g_shown = true;
    g_spaceClaimed = false;
    g_pttHeld = false;

    /* Subscribe before the first read, so nothing published in between is
     * missed. The subscriptions outlive the layer (each callback tests
     * s_layer first), so they are made once. */
    if (!g_subscribed) {
        storageSubscribeChanges("s.partyline.", onStorageChange);
        storageSubscribeChanges("partyline.",   onStorageChange);
        storageSubscribeChanges("lxst.",        onLxstChange);
        storageSubscribeChanges("s.lxst.tx",    onLxstChange);
        storageSubscribeChanges("sys.standby",  onStandbyChange);
        g_subscribed = true;
    }
    g_tickTimer = lv_timer_create(tickCb, 1000, nullptr);

    loadSaved();
    loadHeard();
    loadRooms();
    loadMembers();
    g_lastState = plState();
    g_logSeen = storageGetInt("partyline.log_next", 0);
    buildList();
    if (heldActive()) {
        rebuildList();
        openRoom();
    } else {
        showScreen(SC_LIST);
        rebuildList();
        for (auto& r : g_rowsL)
            if (r.obj && lv_obj_check_type(r.obj, &lv_button_class)) { deferFocus(r.obj); break; }
    }
}

}  // namespace

PartylineApp::PartylineApp() : LcdApp({ .name = "Partyline", .iconBasename = "partyline" }) {}

void PartylineApp::onCreate(lv_obj_t* root) { partylineApp(root); }

bool PartylineApp::onBack()
{
    if (!s_layer) return false;
    if (s_dialog) { closeDialog(); return true; }
    switch (g_screen) {
    case SC_LIST:   return false;
    case SC_SERVER: goList(); return true;
    case SC_ROOM:
        if (textFocused()) { leaveCompose(); return true; }
        goList();
        return true;
    case SC_ROOMS:
    case SC_INFO:
    case SC_MEMBER: openRoom(); return true;
    }
    return false;
}

void PartylineApp::onShow()
{
    g_shown = true;
    if (!s_layer) return;
    if (g_screen == SC_ROOM) refreshTalk();
    updateSpaceClaim();
    /* The shell restores the saved focus around this call: look again once
     * that has settled. */
    lv_timer_t* t = lv_timer_create([](lv_timer_t*) { if (s_layer) updateSpaceClaim(); }, 50, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

void PartylineApp::onHide()
{
    g_shown = false;
    if (!s_layer) return;
    pttRelease();
    updateSpaceClaim();
}

void PartylineApp::onClose()
{
    pttRelease();
    releaseSpace();
    /* The layer first: it is what every subscription callback tests. */
    s_layer = nullptr;
    if (g_metaTimer)  { lv_timer_delete(g_metaTimer);  g_metaTimer = nullptr; }
    if (g_toastTimer) { lv_timer_delete(g_toastTimer); g_toastTimer = nullptr; }
    if (g_tickTimer)  { lv_timer_delete(g_tickTimer);  g_tickTimer = nullptr; }
    resetHandles();
    g_screen = SC_LIST;
}
