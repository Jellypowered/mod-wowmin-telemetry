# Agent documentation

Task-scoped guidance routed from `AGENTS.md`. Keep documents concise, repository-specific, and usable by any coding
agent.

## Taxonomy

- Task documents: `build.md`, `code-review.md`, `self-review-rules.md`, `pull-requests.md`, and `sql-guidelines.md`.
- Language documents: `cpp-guidelines.md`.
- Cross-cutting systems: `telemetry.md` and `playerbots-integration.md`.
- Add `systems/<name>.md` only when a subsystem outgrows an existing document.

## Placement rules

- The most specific applicable document wins.
- General module policy belongs in `AGENTS.md`.
- Generic C++ conventions belong in `cpp-guidelines.md`; packet, hook, configuration, lifetime, or concurrency details
  belong in `telemetry.md`.
- Optional playerbots API and behavior belongs in `playerbots-integration.md`.
- Extend an existing document before creating another.
- Every new document requires a matching routing bullet in `AGENTS.md` in the same change.
- Keep `.coderabbit.yaml` aligned when a review rule is added, removed, or materially changed.

## Capturing lessons

Record only lessons supported by a user correction, verified bug, or recurring review failure. State the invariant and
where it applies; do not preserve conversation history. If the lesson is generic to AzerothCore rather than this module,
propose it to the parent repository instead of duplicating it here.
