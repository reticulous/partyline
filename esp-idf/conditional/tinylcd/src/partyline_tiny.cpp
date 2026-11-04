/**
 * partyline_tiny.cpp — the Partyline status page on the tiny OLED.
 *
 *   Partyline                 live
 *   Lobby
 *   MeshVoice · 4 here
 *   TALKING  /  dana speaking
 *
 * Storage is partyline's API surface (see partyline.h). Everything is read at
 * draw time, which only runs while the page is on screen; the subscriptions
 * turn any change into a redraw request that tinylcd drops unless the page
 * shows.
 */
#include "partyline_tiny.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "storage.h"
#include "tinylcd.h"

namespace {

tinylcd_page_t s_page = TINYLCD_NO_PAGE;

/* Accumulators for the storageForEach walks (draw runs on the tinylcd task
 * only, so file statics are safe). */
std::string s_room, s_roomName, s_speaker;
int s_here = 0;
struct Rec { std::string id, name, room; int speaking = 0, self = 0; };
Rec s_cur;

void flushMember() {
    if (s_cur.id.empty()) return;
    if (s_cur.room == s_room || (!s_roomName.empty() && s_cur.room == s_roomName)) {
        s_here++;
        if (s_cur.speaking && !s_cur.self && s_speaker.empty()) s_speaker = s_cur.name;
    }
    s_cur = Rec{};
}

void memberLeaf(const char* key, const char* val)
{
    /* partyline.members.<mid>.<field> — fields of one record arrive together. */
    const char* tail = key + sizeof("partyline.members.") - 1;
    const char* dot = strchr(tail, '.');
    if (!dot || !val || strchr(dot + 1, '.')) return;
    std::string id(tail, dot - tail);
    if (id != s_cur.id) { flushMember(); s_cur.id = id; }
    const char* f = dot + 1;
    if      (!strcmp(f, "name"))     s_cur.name = val;
    else if (!strcmp(f, "room"))     s_cur.room = val;
    else if (!strcmp(f, "speaking")) s_cur.speaking = atoi(val);
    else if (!strcmp(f, "self"))     s_cur.self = atoi(val);
}

void fitWidth(u8g2_t* g, std::string& s, int w)
{
    while (!s.empty() && (int)u8g2_GetUTF8Width(g, s.c_str()) > w) {
        size_t n = s.size() - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        s.erase(n);
    }
}

bool drawPartylinePage(tinylcd_page_t, u8g2_t* g, tinylcd_ev_t)
{
    u8g2_SetFont(g, u8g2_font_7x13B_tr);
    u8g2_DrawStr(g, 0, TINYLCD_TITLE_Y, "Partyline");

    std::string server = storageGetStr("partyline.server", "");
    std::string state = storageGetStr("partyline.state", "idle");
    u8g2_SetFont(g, u8g2_font_5x8_tr);
    if (!server.empty() && state != "idle") {
        int w = u8g2_GetStrWidth(g, state.c_str());
        u8g2_DrawStr(g, 128 - w, 12, state.c_str());
    }
    if (server.empty() || state == "idle") {
        u8g2_SetFont(g, u8g2_font_6x10_tf);
        u8g2_DrawStr(g, 0, 34, "Not connected");
        return false;
    }

    s_room = storageGetStr("partyline.room", "");
    s_roomName = storageGetStr(("partyline.rooms." + s_room + ".name").c_str(), "");
    std::string title = s_roomName.empty() ? s_room : s_roomName;
    s_here = 0;
    s_speaker.clear();
    s_cur = Rec{};
    storageForEach("partyline.members.", memberLeaf);
    flushMember();

    if (title.empty()) title = "joining";
    /* The bold face is ASCII only; a name beyond it takes the plain one. */
    bool ascii = true;
    for (unsigned char c : title) if (c >= 0x80) { ascii = false; break; }
    u8g2_SetFont(g, ascii ? u8g2_font_7x13B_tr : u8g2_font_6x10_tf);
    fitWidth(g, title, 128);
    u8g2_DrawUTF8(g, 0, 31, title.c_str());

    u8g2_SetFont(g, u8g2_font_6x10_tf);
    std::string name = storageGetStr("partyline.name", "");
    if (name.empty()) name = server.substr(0, 8);
    char here[24];
    snprintf(here, sizeof here, " \xC2\xB7 %d here", s_here);
    std::string line = name;
    fitWidth(g, line, 128 - (int)u8g2_GetUTF8Width(g, here));
    line += here;
    u8g2_DrawUTF8(g, 0, 43, line.c_str());

    std::string bottom;
    if (storageGetInt("lxst.talking", 0)) bottom = "TALKING";
    else if (!s_speaker.empty()) bottom = s_speaker + " speaking";
    else if (storageGetInt("partyline.audio", 0) == 0) bottom = "text only";
    else if (storageGetInt("partyline.can_speak", 0) == 0) bottom = "listen only";
    if (!bottom.empty()) {
        u8g2_SetFont(g, bottom == "TALKING" ? u8g2_font_7x13B_tr : u8g2_font_6x10_tf);
        fitWidth(g, bottom, 128);
        u8g2_DrawUTF8(g, 0, 56, bottom.c_str());
    }
    return false;   /* draw-only page: no events handled */
}

}  // namespace

void PartylineTinyPage::onInit()
{
    s_page = tinylcdAddPage("partyline", drawPartylinePage);
    tinylcdRun(ON_TINYLCD {
        /* On the tinylcd task. The room, the member list and the talk state
         * all live under these two prefixes. */
        storageSubscribeChanges("partyline.", ON_CHANGE { tinylcdDraw(s_page); });
        storageSubscribeChanges("lxst.talking", ON_CHANGE { tinylcdDraw(s_page); });
    });
}
