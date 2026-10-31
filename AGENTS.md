<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Working in this repository

CYD System Monitor is a desk display (LCDWiki E32R28T, ESP32) fed by a Rust Host on an Apple Silicon Mac. [README.md](README.md) has the product, [CONTEXT.md](CONTEXT.md) the domain terms to use in code, UI text and docs.

## Before you change anything

- **Tickets:** read [docs/tickets/README.md](docs/tickets/README.md) first ("Start here", then "Dispatch and completion"), then your ticket and its prerequisites. Status words (Done, Awaiting hardware, Blocked) mean exactly what that file says.
- **Tooling:** run `./dev doctor`, and read [docs/development.md](docs/development.md) for prerequisites and their pitfalls. Every target builds and checks through `./dev build|check|run <target>`; run `./dev check <target>` for what you touched before committing.
- **Licences:** new files carry the SPDX header for their area ([CONTRIBUTING.md](CONTRIBUTING.md)): firmware and protocol code compiled into it `Apache-2.0`, other code `GPL-3.0-only`, docs `CC-BY-SA-4.0`.

## Rules that are easy to miss

- **ESP-IDF lives in one shell.** `source ~/esp/esp-idf/export.sh` and the `idf.py`/`./dev ... firmware` command it serves go in the same shell invocation; each agent tool call is a fresh shell.
- **Host tests need `link_cli`.** The daemon and link-parity tests drive the real firmware link through `firmware/domain/tests/build/link_cli`, which `./dev check host` (or `./dev check firmware`) builds. In a fresh clone or worktree, run that before a bare `cargo test`; otherwise every one of those tests fails with `link_cli is missing`.
- **Firmware build settings:** a local `firmware/sdkconfig` overrides `firmware/sdkconfig.defaults`. After pulling a change to the defaults, build fresh (see "Board sessions" in development.md).
- **The board is exclusive and physical.** Building and the simulator overlap freely; flashing, opening the serial port, BOOT/RESET and RF measurements take the board for one session at a time. Physical checks (looking at the display, touching it, starting agents) are done with the owner: collect them in the ticket's physical check list and run them together.
- **Protocol changes** go through the [protocol change gate](protocol/CORE_PROTOCOL.md#protocol-change-gate): schema, golden fixtures, both codecs, and a clean C++/Rust parity run.
- **Display colours:** red, orange, amber and yellow are reserved for Alerts, and colour always travels with a shape or text ([docs/ui-design.md](docs/ui-design.md)).
- **Agent activity** comes only from each agent's own hooks through `cyd-host agent-event`, never from CPU use or transcripts. Adapters read nothing from the hook payload ([docs/agent-integrations.md](docs/agent-integrations.md)). Install them with `host/target/release/cyd-host hooks install --trust`.
- **The Host feeds the board** with `cyd-host run --port /dev/cu.usbserial-*` in the foreground until the TB08 service is installed; confirm the port each time, it changes between plugs.

## Commits and branches

- One branch or worktree per concurrent ticket; preserve other agents' changes.
- Commit messages: an imperative subject ending in the ticket tag, e.g. `Cap WiFi buffer pools for a receive-only radio (TB09)`, a blank line, a short wrapped body.
- Pushing, merging to `main`, and anything that leaves the machine wait for the owner's go-ahead.

## Multi-agent workflow (the owner's current setup)

One main runner coordinates and integrates; it delegates by role, each child in its own worktree:

| Role | Models |
| --- | --- |
| Writing code; first attempt at fixing a bug | Claude Sonnet 5.5, GPT-6.1 Terra (medium) |
| Validating complex decisions | GPT-6.1 Sol (high) |
| Adversarial review | Grok 4.7, Gemini 4 Argon |
| Local tasks: searching files, running checks, commit messages | Claude Haiku 5.5 (medium) |

If a writer's fix does not solve a bug, the runner fixes it. If a model is out of usage, the runner does that work itself or gives it to Sonnet. Every child brief states what it owns, what it must not touch (other sections, real user config, the board), and the checks that must pass; the runner reviews and verifies each result before merging.
