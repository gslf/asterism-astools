# astools plugin changelog

## 0.3.0 — 2026-08-14

- First release of the Agent Plugins 1.0.0 package (PATH strategy, D17):
  one stdio MCP server (`astools-mcp`) anchored to `${PLUGIN_DATA}`.
- Bundled skill `astools` with the cross-tool conventions (workspace-relative
  paths, safe defaults, how to read permission errors).
- Standard suite exposed over MCP: `fs`, `grep`, `edit`, `git`, `proc`,
  `sys`, `env` (git and proc stay inert until the operator grants `proc`).
