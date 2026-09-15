<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Update firmware over verified USB

The daemon CLI will verify a signed release manifest and artifact checksum, enter the ESP32 serial bootloader, flash over USB, and confirm the running firmware version. The 4 MB MVP hardware profile keeps one application image so retained history is not displaced by a second firmware slot. An interrupted or unbootable update is recovered through the ESP32 ROM serial bootloader using documented BOOT/RESET steps and standard Espressif tools. Profiles with more flash may add dual application slots later.

WiFi updates would compete with the monitored radio and manual IDE flashing would undermine appliance operation. Secure boot, flash encryption, and network update infrastructure are outside the personal desk MVP.
