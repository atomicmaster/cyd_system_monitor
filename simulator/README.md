<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# simulator/

Desktop LVGL target: links the same `firmware/domain` source as the ESP32
image so scenario replay exercises real domain behavior, not a second copy
of it.

At M0 there are no screens yet (`firmware/ui/` is still empty), so this
builds a headless `smoke` scenario proving the shared link, profile
validation, and a controllable clock. LVGL rendering and the desktop window
are wired in starting with F05/TB01 once `firmware/ui/` has something to
show.

```sh
./dev build simulator
./dev check simulator
./dev run simulator -- smoke
```
