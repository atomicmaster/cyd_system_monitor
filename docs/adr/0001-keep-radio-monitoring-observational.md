<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Keep radio monitoring observational

The Monitor Device will passively observe WiFi traffic and BLE advertisements. The MVP will not send WiFi probe requests, BLE scan requests, deauthenticate, impersonate, force handshakes, challenge nearby equipment, or otherwise disrupt the Radio Environment; Classic Bluetooth inquiry is deferred. Because radio evidence rarely proves intent or identity, the product will report suspicious observations with evidence, confidence, and severity rather than claim that an attack was confirmed. This keeps a desk system monitor useful for ambient awareness without turning it into an active wireless assessment tool or overstating what its hardware can establish.
