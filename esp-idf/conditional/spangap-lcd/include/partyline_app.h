/**
 * partyline_app.h — the on-device Partyline app as a boot-registered Service.
 *
 * PartylineApp is an LcdApp: the straddle's `services:` entry points the
 * generated boot code at this header. Methods are defined in partyline_lcd.cpp,
 * where the file-static UI state lives. Compiled only under
 * conditional/spangap-lcd/.
 *
 * The app is a view over the partyline task's storage and a writer of its
 * command keys (partyline.h has the contract): the servers list, a server's
 * screen, the room with its members, talk indicator and text log, the room
 * list to move between rooms, the room and server info page and a member's
 * card. While the room screen is up and its text field is not focused the app
 * claims the held Space key (lcd_keys.h), so holding Space talks.
 */
#pragma once

#include "lcd_app.h"
#include "lvgl.h"

class PartylineApp : public LcdApp {
public:
    PartylineApp();
    void onCreate(lv_obj_t* root) override;
    /** Back to the screen the current one was opened from (a dialog first, and
     *  out of the composer before out of the room); the servers list falls
     *  through to Home. */
    bool onBack() override;
    /** Showing the app re-claims Space on the room screen; hiding it releases
     *  the claim and lets go of the on-screen talk button. */
    void onShow() override;
    void onHide() override;
    /** Releases the Space claim, lets go of the talk button, nulls every widget
     *  handle (the shared subscriptions outlive the layer) and deletes pending
     *  timers. */
    void onClose() override;
};
