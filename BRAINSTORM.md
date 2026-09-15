# Brainstorm: Elegoo Cheap Yellow Display (CYD) System and WiFi/BT Monitor
* **Date:** September 15, 2026
* **Participants:** atomicmaster 
* **Goal:** Define core features for this project

---

## Core Problem Statement
I want a little display that will show me the status of my system (CPU/GPU/NPU/RAM/Network/Disk Utilization/Disk Usage), and since we also have WiFi and Bluetooth hardware built into the display, we should also monitor the local airwaves.

## Language
* CYD - refers to Eletoo Cheap Yellow Display ESP32-32E-based device
* Local - referenced without other qualifiers means the current, local system
* Device - refers to the CYD display device
* AP - WiFi Access Point
* BT - Bluetooth or BLE
* Deauth - deauthentication/disassociation
* DoS - denial of service

## Feature Ideas

### 1. System Monitoring
* Local telemetry will require a local daemon, this should probably be built in Rust
* For raw hardware, we should monitor the CPU, GPU, NPU (if present), RAM, Disk Usage, Disk I/O, and Battery capacity and health (if present)
* For local Networking, we should list currently connected devices(s) if there are multiple devices that are actively used for networking, IPs, RX/TX

### 2. WiFi Monitoring
* Local to the device
* Shows active channel occupancy and congestion visualization
* Near-by AP density and beacon detection
* Promiscuous mode packet sniffer statistics (packet rates/sec, distribution of management vs control vs data frames)
* Shows a list of all hardware devices seen in the latest scan
* Remembers previously-seen devices
* Deauth frame anomaly counter
* Identify Deauth Storms (Storms/Handshake Forcing)
* Identify Beacon Flooding/Fake Access Point Floods
* Identify Rogue Access Points/Evil Twins/Karma Attacks
* Identify Probe Request Flooding & Harvesting
* EAPOL/PMKID Capture Indicator

### 3. BT Monitoring
* Local to the device 
* Device density counter (number of unique MAC addresses advertising nearby)
* Proximity tracking (sorts detected BLE beacons by RSSI/signal strength)
* Identify common beacon types (Apple AirTags/Find My network tags, iBeacon, Eddystone)
* Identify BLE Advertisement Flooding/Pairing DoS
* Identify MAC Address Randomization Exhaustion/Churn
* Identify Malicious/Clone AirTags & Tracker Floods
* Identify Bluetooth Skimmer Proximity Indicators
* Identify Rogue Peripheral Impersonation (Spoofed HID/Audio)

### 3. User Interface
* Needs to be a neat and compact for the overview
* Should be able to expand any metric and see more details
* Should have a visible alert any time anything malicious is detected (BT/WiFi Monitoring)
* Should retain history of alerts
* For any detection, should have a description of what was detected
* Should have a clock
* Dark theme
* Complimentary colors
* Graphs should compress the time scale showing most details most recently and gradually compressing time with respect to time

### 4. Other Monitoring
* If Claude or Codex or Grok or other local agents installed, we should have a simple overview of current agent activity (perhaps an animated icon)
* If there are multiple agents, we should have a count of agents and their status
* We should be able to see current usage statistics/limits at a glance
* We should be able to expand to dive into expanded usage statistics/limits, and also to get more information about the agent(s) and what they are doing

### 5. Portability to Other Platforms
* Local daemon should be expandable to multiple platforms with an easy ability to add a platform without having to alter the whole daemon

## MVP
* Local daemon, CYD firmware, and a local testing environment
* Basic Local system monitoring
* Basic WiFi monitoring
* Basic BT monitoring
* Local daemon should support OS X
* Basic agent activity

## V.1
* Adding the ability to expand the metrics and view monitoring graphs
* Implementing any BT or WiFi malicious airwave detection not implemented as part of MVP

## V.2
* Agent details and detailed agent usage statistics
* Adding Linux and Windows support for the daemon