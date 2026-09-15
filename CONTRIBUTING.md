<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Contributing

Contributions are welcome when they preserve the accepted product boundaries and evidence language.

Before changing behavior, read [BRAINSTORM.md](BRAINSTORM.md), [CONTEXT.md](CONTEXT.md), and the relevant design document. Use established domain terms in code, UI text, tests, and documentation. A change that reverses a costly or surprising architectural choice must update or supersede its ADR.

For ticket-based implementation or parallel agent work, start with the [tracer-bullet backlog](docs/tickets/README.md). Read its dispatch rules and the assigned ticket's prerequisites, ownership, and acceptance before editing. Outline tickets must be refined before dispatch; hardware sessions and shared-interface changes have explicit owners.

Submit focused changes with the tests appropriate to their risk. Protocol changes require schema updates and shared golden vectors. Radio-rule changes require boundary fixtures, coverage behavior, evidence/claim review, and false-positive measurements. Hardware changes stay inside a profile and include physical smoke evidence for any new support claim.

Software contributions are submitted under `GPL-3.0-only`, except contributions to the firmware image or to `protocol/` code compiled into it, which are submitted under `Apache-2.0` (see [ADR 0027](docs/adr/0027-license-firmware-under-apache-2-0.md)); documentation contributions are submitted under `CC-BY-SA-4.0`. Preserve third-party copyright, license text, attribution, and source provenance. Do not commit vendor material unless its redistribution terms are documented and compatible with the repository.

Use SPDX identifiers in new project-authored files. Most software, including `host/` and `macos/setup-app/`, uses:

```text
SPDX-License-Identifier: GPL-3.0-only
```

Firmware, and any `protocol/` code compiled into it, uses:

```text
SPDX-License-Identifier: Apache-2.0
```

Documentation uses:

```text
SPDX-License-Identifier: CC-BY-SA-4.0
```

By submitting a contribution, you represent that you have the right to provide it under the applicable project license. The project does not require a separate contributor license agreement.
