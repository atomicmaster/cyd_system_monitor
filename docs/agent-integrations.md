<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->

# Agent integration boundary

The MVP reports exact same-user process presence for supported agents and uses `running with activity unknown` unless a trustworthy adapter supplies stronger evidence. V2 may add separately enabled adapters that distinguish busy, idle, and bounded usage through documented local APIs, structured logs, or hooks supplied by the agent.

Each adapter declares the source, fields read, availability states, sampling rate, and retention. It does not inspect private conversation files, prompt or response contents, source contents, command arguments, or credentials. Metric Availability uses the shared available/unsupported/absent/temporarily unavailable/error states. Adapter enablement and Metric Freshness are separate fields: disabled is an adapter setting, and stale describes the age of a retained sample rather than adding another availability enum value. An adapter may report only the evidence its source supports and may not infer semantic task activity from process load alone.
