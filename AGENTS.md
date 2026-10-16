# AGENTS.md

mod-wowmin-telemetry is an MIT-licensed AzerothCore C++20 module that exposes authenticated text telemetry and an
optional, server-addressed UDP stream to WoWMin. It can integrate with mod-playerbots, but must build and remain useful
without it.

This directory is a nested Git repository normally checked out at `modules/mod-wowmin-telemetry/`. The parent
AzerothCore guidance applies as a base. This file and `.agents/docs/` take precedence for this module when rules differ.
Do not silently modify the parent core, mod-playerbots, or WoWMin repositories as part of a module-only task.

## Agent rules

- **Do not configure or build unless explicitly asked.** Use the checks in `.agents/docs/build.md` by default.
- Read `.coderabbit.yaml` before changing or reviewing the module. It is the compact policy shared with automated
  review.
- Formatting follows `.clang-format` and `.editorconfig`: UTF-8, LF, spaces, a trailing newline, no trailing whitespace,
  and a 120-column limit. C++ uses four spaces; YAML, JSON, and shell use two.
- Preserve both supported configurations. Keep all mod-playerbots includes and symbols behind `MOD_PLAYERBOTS`.
  Core-only behavior must not depend on that module being installed.
- Treat map updates and combat/player hooks as hot paths. Gate disabled features before scans, bound retained state and
  wire fields, and never perform synchronous database work there.
- Respect thread ownership. Read map-owned objects on their map thread and keep only owned values or GUIDs across
  calls. Synchronize state shared by map and world threads. Do not hold shared locks during network I/O or formatting.
- Telemetry is observational. Never let commands choose the UDP destination or change playerbot strategies, movement,
  roles, actions, or other gameplay state.
- Wire compatibility is an API. A layout or semantic change must update the correct protocol version, byte counts,
  split calculations, docs, and the paired WoWMin parser/tests when that repository is in scope.
- The module currently needs no SQL. Do not edit parent AzerothCore SQL for a module task. If SQL becomes necessary,
  read `.agents/docs/sql-guidelines.md` first and keep it inside this module's `data/sql/db-*` tree.
- Planning documents go in `.agents/plans/<task-slug>/` and are gitignored.
- Preserve upstream authorship and licensing. Before adapting external code, inspect its license and history. Do not
  copy code whose license is incompatible with this MIT module.

## Mandatory reading per task

Read the matching documents before starting:

- Configuring, building, or running checks → `.agents/docs/build.md`
- Writing or modifying C++ → `.agents/docs/cpp-guidelines.md`
- Changing stream packets, commands, shared state, hooks, configuration, or subscription behavior →
  `.agents/docs/telemetry.md`
- Changing optional mod-playerbots integration → `.agents/docs/playerbots-integration.md`
- Creating or modifying SQL → `.agents/docs/sql-guidelines.md`
- Reviewing a changeset or PR → `.agents/docs/code-review.md`
- Self-reviewing or preparing a PR → `.agents/docs/self-review-rules.md`; use
  `.agents/skills/self-review/SKILL.md` for `/self-review [full|current|staged]` (default: uncommitted `current`)
- Writing a commit or PR title/body → `.agents/docs/pull-requests.md`
- Adding or updating agent guidance → `.agents/docs/README.md`

## Repository layout

- `src/WowMinTelemetry.cpp` — configuration, commands, snapshots, UDP serialization, hooks, and optional playerbots
  data.
- `src/wowmin_telemetry_loader.cpp` — module script registration entry point.
- `conf/mod-wowmin-telemetry.conf.dist` — distributed configuration and security/default documentation.
- `data/sql/db-{auth,characters,world}/` — module SQL roots; currently placeholders only.
- `README.md` — installation, command protocol, stream protocol, compatibility, and concurrency contract.
- `.github/workflows/core_codestyle.yml` — reusable AzerothCore codestyle plus the module-owned clang-format check.
- `.coderabbit.yaml` — automated-review policy mirrored by these agent docs.

## Related repositories

- Parent core: `../..`
- Optional mod-playerbots API: `../mod-playerbots`
- WoWMin reader: commonly `../../../wowmin`, but verify it exists before relying on it

Local checked-out source wins over remembered upstream behavior. Mark anything that cannot be verified as unverified.
