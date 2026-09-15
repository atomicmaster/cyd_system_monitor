<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Use USB for the host link

The Monitored Host will send Host Metrics to the desk-mounted Monitor Device over USB serial, with the same cable supplying power. A network link would consume airtime and constrain channel observation on the ESP32's single shared radio, while Bluetooth would compete directly with both WiFi and Bluetooth observation. The physical link also gives the one-host MVP a simple trust and pairing boundary. Standalone radio monitoring requires continued power: stopping Host software or losing the data connection can leave monitoring running, but removing the sole USB power cable stops it. MVP does not require uninterrupted operation across physical unplugging.
