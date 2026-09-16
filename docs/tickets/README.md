<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Tracer-bullet ticket backlog

This is the repository-local MVP backlog. The [implementation plan](../../IMPLEMENTATION_PLAN.md) defines milestone acceptance; ticket prerequisites define the order in which work can start. A tracer bullet carries one observable behavior through the required layers. Toolchains, contract foundations, and measurement gates are identified separately rather than presented as user features.

**No ticket has been implemented.** Eleven tickets are fully specified; twenty later tickets are outlines. The first working Host path is C01 → TB01 → TB02: agreed snapshot bytes, the shared display state under replay, then a physically paired Mac showing live CPU with stale/reconnect behavior.

## Start here

1. Assign **F01**. It is the only immediately eligible ticket in this initial backlog.
2. Once F01 is integrated, **F02 and F03** can run independently. **F04** integrates their toolchains and closes M0.
3. Complete **F05**, then develop **F06 and F07** in separate owned modules. Schedule their physical checks and shared diagnostic registration serially.
4. **F08** is the hard hardware feasibility gate. Production schema/domain/Host/UI work waits for its measured result.
5. Execute **C01, TB01, and TB02** in order under coordinated contract ownership. Each contributes a runnable intermediate result, and TB02 closes the first real Host-data path.
6. Refine **C02** against that evidence. After its interfaces are integrated, promote and assign independent feature tickets from the table below.

Implementation commands shown in tickets are proposed deliverables. F01 creates the dispatcher; runtime owners add their targets and scenario commands. This backlog does not claim those commands or modules exist today.

## Dispatch and completion

Before assigning a ticket:

1. Read the ticket, [CONTEXT.md](../../CONTEXT.md), and its linked design inputs. Product scope comes from the [brief](../../BRAINSTORM.md), [roadmap](../roadmap.md), and ADRs; the backlog does not override them.
2. Check that every linked prerequisite is **Done**, with an integrated commit and required evidence. A detailed ticket can still be blocked by prerequisites. Draft research/fixture preparation is not permission to bypass a gate.
3. For an **Outline**, first resolve its promotion questions, set concrete acceptance scenarios/commands and resource limits, name exact owned modules, and change Detail to **Executable**. Split an oversized outline into linked children while preserving its parent outcome and dependency links.
4. Assign one slice owner and an integration owner. A slice can span Host/firmware/UI paths: the owner is responsible for the observable result across them. Shared interface edits require the designated contract owner's coordination before consumers diverge.
5. Use a separate branch/worktree per concurrent ticket, based on integrated prerequisite commits. Agents are not alone in the repository: preserve others' changes, adapt to them, and avoid reverting or replacing work outside assigned ownership. Shared files and generated outputs are handled as described below.
6. Reserve the physical board when required. Build/simulator work can overlap; flashing, serial capture, BOOT/RESET, power interruption, and RF measurement require exclusive access. Record claimant, ticket, port, flashed commit, and release of the device in the coordinator's task log. Confirm the actual port rather than relying on its former path.

**Done** means owned changes are integrated, acceptance checks pass, and the ticket links exact commands/results, base/result commits, and physical evidence where required. A code-complete branch awaiting board access is **Awaiting hardware**, not Done. Missing consent, signing credentials, test equipment, or a clean supported Mac is **Blocked** with the specific missing input recorded. Use **In progress** while actively executing and **Not started** otherwise. The coordinator updates the ticket's status and evidence; the index is a navigation view, not a second status register.

The integration owner keeps the shared runnable build healthy, schedules hardware sessions, and reruns affected cross-module checks after merges. Passing independent feature tests is not a substitute for an integrated physical milestone. Publishing releases and sending external messages are separate actions; the backlog itself is local Markdown and creates no hosted issues.

## Shared ownership and interfaces

Proposed implementation paths below become concrete during their owning tickets. They reserve responsibility rather than scaffolding code in this planning change.

| Shared surface | Steward established by | Consumer rule |
| --- | --- | --- |
| Root developer command and global bootstrap conventions | F01; integration owner maintains it | Add target-specific scripts without editing the root dispatcher in every feature |
| C++ build targets and domain/renderer seams | F02 | ESP32 and desktop link the same C++ domain source; Rust owns Host collection/transport, not a second radio rule engine |
| Rust/Swift workspace roots | F03 | Per-feature modules avoid simultaneous workspace-root edits |
| Shared CI, dependency pins, generation bootstrap | F04 | Runtime owners supply commands; integration owner registers jobs/pins serially |
| Schema, generated C++/Rust types, golden vectors | C01, extended through C02 | Coordinate schema changes with one steward and regenerate outputs; consumers never hand-edit generated types |
| Host state, CPU widget, initial UI shell | TB01 | Feature widgets receive stable state inputs; shared navigation/status registration is integrated serially |
| USB sessions, pairing, time and Host producer loop | TB02 | Collectors return bounded results; they do not each open serial ports or own transport |
| Metric/widget extension points and radio/episode/storage interfaces | C02 | Each feature gets a narrow path and fixtures; each new interface includes ordering, errors, bounds, and examples |
| Radio scheduling and coverage | TB09 | WiFi/BLE parsers consume events; only the scheduler arbitrates radio windows |
| Alert lifecycle and rule interface | TB12 | Later rules return evidence through the shared lifecycle; they do not duplicate acknowledgment/recovery logic |
| Flash journal and bounded retention | TB13 | Rules request persistence through the interface; only the journal owns flash commit/reclamation |
| Integrated screen composition and navigation | TB17 | Preserve feature widgets and arrange them through established interfaces |

Stewardship is a responsibility that the coordinator may reassign, not a requirement that one original agent stay alive indefinitely. `firmware/` is not one exclusive ownership bucket: individual slices own its narrow modules. A shared CMake file, Rust workspace manifest, schema, or app entrypoint has one writer at a time even if the rest of those slices run concurrently.

## Parallel waves after the first path

- After C02: TB03–TB08 can develop distinct Host/UI slices; TB09 can develop radio scheduling. Start a small number that fit review and integration capacity rather than all seven at once.
- TB05 and TB08 agree the WiFi-context provider IPC before starting concurrently. TB05 can use fixtures for permission states; TB08 owns the real consent/lifecycle integration and R03 verifies the joined path.
- After TB09: TB10 and TB11 own separate WiFi/BLE parsers and views. Their physical tests take turns.
- After TB13: TB14 and TB15 can add independent BLE rules while TB16 adds configuration behavior, using agreed rule/config fragments and a single journal owner.
- R02 update work may overlap remaining UI work after its prerequisites. R01/R03 close integrated installation and qualification gates; signing/equipment access must be scheduled explicitly.

## Tickets

The dependency column repeats each ticket's prerequisite links for navigation. Edit the ticket first, then keep this view consistent when dependencies change. All outlined dependencies are provisional until promotion; cycles or omitted physical integration must be resolved before dispatch.

| Ticket | Observable outcome or foundation | Detail | Prerequisites |
| --- | --- | --- | --- |
| [F01](F01.md) | Establish repository ownership and developer entrypoints | Executable | None |
| [F02](F02.md) | Build firmware and desktop targets from one C++ core | Executable | [F01](F01.md) |
| [F03](F03.md) | Build the Rust Host and Swift setup targets | Executable | [F01](F01.md) |
| [F04](F04.md) | Integrate reproducible builds and CI | Executable | [F02](F02.md), [F03](F03.md) |
| [F05](F05.md) | Boot the board into a visible diagnostic screen | Executable | [F04](F04.md) |
| [F06](F06.md) | Calibrate touch and recover persisted calibration | Executable | [F05](F05.md) |
| [F07](F07.md) | Exercise the remaining board peripherals | Executable | [F05](F05.md) |
| [F08](F08.md) | Prove combined hardware feasibility and resource budgets | Executable | [F06](F06.md), [F07](F07.md) |
| [C01](C01.md) | Define and exercise the first USB snapshot contract | Executable | [F08](F08.md) |
| [TB01](TB01.md) | Replay a CPU snapshot into the shared display model | Executable | [C01](C01.md) |
| [TB02](TB02.md) | Pair a Mac and display live CPU over USB | Executable | [TB01](TB01.md) |
| [C02](C02.md) | Publish the extension interfaces for parallel feature slices | Outline | [TB02](TB02.md) |
| [TB03](TB03.md) | Display Physical RAM Occupied with truthful availability | Outline | [C02](C02.md) |
| [TB04](TB04.md) | Display root storage capacity and disk activity | Outline | [C02](C02.md) |
| [TB05](TB05.md) | Display network rates and Host WiFi coverage | Outline | [C02](C02.md) |
| [TB06](TB06.md) | Display battery charge and condition without blocking collection | Outline | [C02](C02.md) |
| [TB07](TB07.md) | Show supported agent process presence | Outline | [C02](C02.md) |
| [TB08](TB08.md) | Run the Host as a per-user service with setup status | Outline | [C02](C02.md) |
| [TB09](TB09.md) | Show the real radio schedule and coverage | Outline | [C02](C02.md) |
| [TB10](TB10.md) | Observe WiFi traffic through to an inspectable view | Outline | [TB09](TB09.md) |
| [TB11](TB11.md) | Observe BLE advertisements through to an inspectable view | Outline | [TB09](TB09.md) |
| [TB12](TB12.md) | Carry one deauthentication anomaly through an Alert episode | Outline | [TB10](TB10.md), [TB05](TB05.md) |
| [TB13](TB13.md) | Recover durable Alerts and baselines after power loss | Outline | [TB12](TB12.md), [TB11](TB11.md) |
| [TB14](TB14.md) | Add BLE advertisement flooding episodes | Outline | [TB13](TB13.md) |
| [TB15](TB15.md) | Add BLE identifier-churn episodes | Outline | [TB13](TB13.md) |
| [TB16](TB16.md) | Change settings consistently from touch and Host commands | Outline | [TB13](TB13.md), [TB08](TB08.md) |
| [TB17](TB17.md) | Complete and verify the six-card appliance interface | Outline | [TB03](TB03.md), [TB04](TB04.md), [TB05](TB05.md), [TB06](TB06.md), [TB07](TB07.md), [TB08](TB08.md), [TB10](TB10.md), [TB11](TB11.md), [TB14](TB14.md), [TB15](TB15.md), [TB16](TB16.md) |
| [TB18](TB18.md) | Export diagnostics and erase only explicitly selected data | Outline | [TB13](TB13.md), [TB16](TB16.md) |
| [R01](R01.md) | Install and remove a signed macOS appliance package | Outline | [TB08](TB08.md), [TB17](TB17.md), [TB18](TB18.md) |
| [R02](R02.md) | Fetch a verified release and recover USB firmware updates | Outline | [TB08](TB08.md), [TB16](TB16.md) |
| [R03](R03.md) | Qualify the integrated MVP and publish release evidence | Outline | [R01](R01.md), [R02](R02.md) |
| [FR01](FR01.md) | Add and verify optional speaker alerts | Outline (deferred) | Alert lifecycle and Settings interfaces |

## Milestone coverage

Tickets can cross several milestones; completing a tracer bullet does not close every milestone it touches. M1a remains a hard start gate. Other milestones close when all their relevant ticket outcomes and the plan's exit criteria pass on the integrated build.

| Milestone | Tickets supplying its evidence |
| --- | --- |
| M0 | F01–F04 |
| M1 | F05–F07 |
| M1a | F08 |
| M2 | C01, TB01, TB02, C02, TB09, TB12, TB13, TB16 |
| M3 | TB02–TB08 |
| M4 | Feature slices plus TB12/TB16, integrated by TB17 |
| M5 | TB09–TB11 and radio configuration in TB16 |
| M6 | TB12–TB16 and Radio History export/erase in TB18 |
| M7 | TB16/TB18, R01–R03 |

The final [validation gate](../mvp-validation.md) remains authoritative. Per-rule evidence/Confidence/recovery semantics must be documented before rule fixtures are fixed; numeric defaults are validated against physical observations. F08 counts are provisional until durable storage validates actual maximum record sizes and write behavior.

## Deferred scope

V1 history/graphs, V2 GPU/ANE/helper/platform/agent/survey features, optional speaker alerts ([FR01](FR01.md)), and unassigned advanced detections/Extreme Severity are outside this MVP backlog. See the [roadmap](../roadmap.md). Unsupported optional capability data stays explicitly unavailable; future features are not filled with fabricated values or silently pulled into foundation tickets.
