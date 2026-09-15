<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Use a monorepo with explicit runtime seams

Firmware, the Rust host workspace, macOS setup application, language-neutral protocol, simulator, hardware profiles, and documentation will live in one repository under separate top-level boundaries. Shared protocol vectors, compatibility manifests, hardware configuration, and end-to-end tests can therefore change atomically, while firmware C++, host Rust, and setup Swift remain independently buildable. Separate repositories would make cross-runtime releases and schema changes harder to review without removing the need for those interfaces.
