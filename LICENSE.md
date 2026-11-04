# License

This repository, **partyline** (a Partyline voice-room client on spangap,
with a browser window and an on-device LVGL app), is released under the
**Apache License, Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by reticulous project contributors.

## Third-party software

### Vendored in this repository

None.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml` and `browser/package.json`:

| Component / package | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |
| Browser peer deps (Vue, Quasar, Pinia) | npm | MIT |

Partyline is developed by RFnexus (`RFnexus/partyline`) and released under the
Unlicense. It is the reference for the room protocol; this implementation is
independent, and no partyline source code is incorporated. Audio goes through
the lxst straddle, whose licence notes cover LXST and the codecs.

The host test vectors (`esp-idf/test/host/vectors.h`) are packets captured
from a partyline 1.0.5 server and its reference clients, encoded with the
umsgpack encoder vendored in RNS (MIT).
