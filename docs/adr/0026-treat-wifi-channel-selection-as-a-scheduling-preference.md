<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Treat WiFi channel selection as a scheduling preference, not a regulatory gate

The Monitor Device will offer a WiFi Channel Plan preference, selecting a regional channel-count preset or a world-safe default, rather than a first-start "Regulatory Region" confirmation. The setting selects listening channels and has no effect on BLE observation. It does not establish legal compliance or authorize reception. The board remains passive: no association, probe requests, active BLE scan requests, or other intentional transmission.

The original rationale overstated the legal findings by describing receivers as outside the reviewed regulatory frameworks. EU RED Article 2 explicitly includes receiving equipment, and US Part 15 contains receiver-specific provisions. The corrected [research note](../research/regulatory-region.md) distinguishes equipment requirements from the narrower UI decision; absence of an app-level region-confirmation requirement is not proof that passive reception is universally unregulated.

The Monitor Device defaults to a documented passive-listening channel range validated with the pinned ESP-IDF version in M1a/M5. The Operator may change the plan from ordinary Settings; an imported plan applies like any other portable setting without separate reconfirmation. This lets the scheduler avoid spending dwell time on irrelevant channels while keeping the setting's meaning technical. Product documentation retains a general reminder that Operators are responsible for complying with their own local law, without presenting a channel selection as a compliance check.
