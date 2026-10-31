// SPDX-License-Identifier: GPL-3.0-only
// CYD System Monitor: reports OpenCode busy/idle by running `cyd-host agent-event opencode`.
// Reads only the event type name; never a prompt, message, tool call or session record.
import { spawnSync } from "node:child_process"

// Set this to the installed host binary's absolute path (or export CYD_HOST_BIN).
const CYD_HOST = process.env.CYD_HOST_BIN || "/opt/cyd/cyd-host"

const BUSY = ["session.execution.started", "permission.replied"]
const IDLE = [
  "session.execution.succeeded",
  "session.execution.failed",
  "session.execution.interrupted",
  "permission.asked",
  "session.deleted",
]

// Synchronous so an idle report is not lost when the process exits right after a turn.
const report = (state) => {
  spawnSync(CYD_HOST, ["agent-event", "opencode", state], { stdio: "ignore", timeout: 2000 })
}

export default {
  id: "cyd-system-monitor",
  setup: async (ctx) => {
    ctx.tool.hook("execute.before", () => report("busy"))
    ctx.tool.hook("execute.after", () => report("busy"))
    ;(async () => {
      for await (const event of ctx.event.subscribe({ types: [...BUSY, ...IDLE] })) {
        report(BUSY.includes(event.type) ? "busy" : "idle")
      }
    })().catch(() => {})
  },
}
