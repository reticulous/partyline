/**
 * partyline_tiny.h — the Partyline status page on the tiny OLED.
 *
 * The when:-gated tinylcd slice of partyline (conditional/tinylcd/): compiled
 * only when spangap/tinylcd is staged. One page: the room and the server, how
 * many are in the room, and TALKING while our transmitter is keyed (or who is
 * speaking). See partyline_tiny.cpp.
 */
#pragma once

#include "service.h"

class PartylineTinyPage : public Service {
public:
    void onInit() override;
};
