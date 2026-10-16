# Repository Grounding

This repository is mod-wowmin-telemetry, an AzerothCore module normally checked out inside an AzerothCore source tree.
It owns telemetry commands, protocol serialization, configuration, and optional read-only mod-playerbots integration.

Do not substitute remembered World of Warcraft, AzerothCore, TrinityCore, playerbots, Boost.Asio, or WoWMin behavior for
inspection of the checked-out implementation.

For claims or changes:

- Read this repository's `AGENTS.md`, `.coderabbit.yaml`, and task-specific `.agents/docs/` first.
- Inspect `src/`, `conf/`, `README.md`, local Git history, and CI configuration as appropriate.
- Trace AzerothCore APIs into the parent checkout (`../..`) before relying on hook thread, lifetime, lock, map, session,
  command, module-loader, enum, or serialization behavior.
- Trace optional playerbots APIs into `../mod-playerbots`; verify signatures and thread ownership against that exact
  checkout. Keep all dependencies guarded by `MOD_PLAYERBOTS`.
- When a task changes the consumer contract, inspect the paired WoWMin checkout if available (commonly
  `../../../wowmin`). If it is unavailable or outside scope, state that compatibility is unverified or needs a companion
  change rather than guessing.
- Prefer module-local policy over parent policy for this nested repository. Parent AzerothCore rules still apply when
  they do not conflict; sibling mod-playerbots rules are evidence for integration constraints, not blanket rules for
  unrelated telemetry code.
- Before modifying code, read `.coderabbit.yaml`. If a task changes parent-core or playerbots files too, also read that
  repository's CodeRabbit and agent guidance before touching it.
- Verify wire sizes, offsets, versions, count widths, limits, and cleanup paths from code. Do not infer them from
  protocol names or old documentation.
- Treat statements such as "AzerothCore normally does" or "playerbots is thread-safe" as unverified until established
  from the local call path.

Grounding policy identifier: WOWMIN_TELEMETRY_V1
