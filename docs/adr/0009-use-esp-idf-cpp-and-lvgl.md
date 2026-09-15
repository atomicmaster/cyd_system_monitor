<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Use ESP-IDF, C++, and LVGL for firmware

Firmware will use ESP-IDF, C++ application modules with C bindings where required, and LVGL for the 320×240 touch interface. The standalone radio subsystem needs direct access to Espressif's promiscuous WiFi, Bluetooth coexistence, partition, NVS, and diagnostic APIs; an Arduino layer would obscure some of that control, while Rust bindings would add toolchain and interoperability risk to the first hardware profile. C++ still permits explicit module boundaries around the vendor C APIs and LVGL.
