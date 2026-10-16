# .agents

Source of truth for this module's task-scoped agent guidance:

- `docs/` — engineering, review, build, and repository-policy documents
- `skills/<name>/SKILL.md` — reusable module-specific agent workflows
- `plans/` — temporary task plans, gitignored

`AGENTS.md` routes tasks to the relevant documents. Keep guidance agent-agnostic and avoid duplicating rules in several
files; link to the canonical document instead.

## Relationship to the parent core

The module is normally located at `modules/mod-wowmin-telemetry/`, so parent AzerothCore guidance is available at
`../../AGENTS.md` and `../../.agents/docs/`. Parent guidance is the baseline, while this module's guidance wins for
local layout, protocol, threading, SQL placement, branch, build, and review details.

The optional playerbots checkout at `../mod-playerbots` is an implementation dependency only when `MOD_PLAYERBOTS` is
defined. Its local source and guidance must be consulted for integration changes, but AI-engine rules do not apply to
unrelated telemetry code.

## Agent entry points

- Claude Code reads `CLAUDE.md`, which imports `AGENTS.md`.
- GitHub Copilot reads `AGENTS.md` natively.
- Other agents should be pointed at `AGENTS.md` and `.pi/APPEND_SYSTEM.md` using their supported include mechanism.

## Self-review command

The canonical workflow is `skills/self-review/SKILL.md`, adapted from the MIT-licensed eai-org/agent-toolkit skills.
It provides three modes, module checks, finding dispositions, and ignored local review reports. It never stages fixes.

In Pi, open the module as the working directory and grant project trust to load project resources. Run `/reload` after
adding the skill. Use `/self-review` (the `.pi/prompts/self-review.md` alias) or `/skill:self-review` directly:

- `/self-review` or `/self-review current`: all uncommitted staged, unstaged, and non-ignored untracked changes.
- `/self-review full`: the entire current module repository, including non-ignored untracked files.
- `/self-review staged`: exactly the staged patch and index versions, excluding unstaged edits.

`current` does not include branch commits. Optional `autoaccept` follows the mode. Other agents can read the canonical
skill directly; slash-command availability depends on their skill discovery mechanism.

When adding a skill, keep its implementation under `.agents/skills/` and expose it through any agent-specific skill
folder only when that integration exists in this repository.
