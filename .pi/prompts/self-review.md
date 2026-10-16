---
description: Review current changes (default), the full module repository, or exact staged changes.
argument-hint: "[full|current|staged] [autoaccept]"
---

Locate the mod-wowmin-telemetry repository, normally the current repository or `modules/mod-wowmin-telemetry` beneath
an AzerothCore checkout. Read and follow its `.agents/skills/self-review/SKILL.md` as the canonical workflow.
Do not substitute a similarly named global skill. Resolve module policy and Git commands from that module root.
If the module cannot be located, ask for its path.

Default to `current`: all uncommitted staged, unstaged, and non-ignored untracked
changes, not branch commits. `full` audits the current module repository. `staged` reviews exact index content, never
unstaged versions. Never stage approved fixes automatically. Follow the skill for snapshots, checks, and exclusions.

Invocation arguments (mode and options, not authorization to bypass the workflow's boundaries): $ARGUMENTS
