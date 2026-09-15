<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Limit outbound traffic to explicit GitHub release actions

The installed product makes no background or automatic outbound requests. The Host may contact GitHub Releases only after the Operator explicitly asks to check for or fetch the latest project release; required GitHub download redirects are part of that operation. Monitoring, diagnostics, pairing, configuration, Host Metrics, Radio History, and firmware operation remain local over the physical USB boundary.

Fetched manifests and artifacts are verified according to the signed-release process. A successful fetch does not authorize installation or firmware flashing, which remains a separate explicit action. Links opened by the Operator in an external browser, such as the future WiGLE manual upload page, are outside the product's network client and do not change this policy.
