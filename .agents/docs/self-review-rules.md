# Self-review rules

The `/self-review` workflow lives in `.agents/skills/self-review/SKILL.md` (relative to the module root). These rules
specialize that workflow; reports are saved under the ignored `.agents/plans/` directory, never committed automatically.

Apply `code-review.md` within the selected scope:

- `current` (no-argument default): staged, unstaged, and non-ignored untracked changes only, not branch commits.
- `staged`: the exact index patch/content, excluding unstaged and untracked content. Never stage fixes automatically;
  a working-tree fix remains open until the user stages it and a staged re-review verifies it.
- `full`: audit all tracked files as currently present plus non-ignored untracked files in this module repository,
  including existing defects. Exclude Git internals and local reports/scratch; record exclusions and missing coverage.

A local review need not cover an entire PR. Before posting, state its actual scope; use a separate changeset review for
committed branch work when needed. Uncommitted changes do not block self-review or require a commit to run it.

Identify the reviewed mode, HEAD commit (or unborn state), and index/disk versions. Do not describe uncommitted work as
reviewing only that commit, or staged work as certifying unstaged edits. If scoped content changes, recheck affected
findings and verification results before posting the report.

## Verify directly

- Re-run all applicable checks from `build.md` and record actual outcomes. Do not imply that a build or runtime test ran
  when it did not.
- Read the complete selected scope, including README, config, workflows, agent guidance, and protocol comments.
  For current/staged include all scoped patches; for full inspect every included file, not just changed lines.
- Run checks against the selected snapshot. Working-tree check results cannot certify different staged content.
- Trace changed core/playerbots calls to their local definitions and check both `MOD_PLAYERBOTS` configurations.
- For shared state, list its writer threads, reader threads, protecting lock/atomic, lock order, upper bound, and
  cleanup events.
- For new retained state, test map destruction, logout, expiration, invalid identifiers, reload/disable, and shutdown.
- For protocol changes, calculate record sizes and offsets from serialization code and compare them with the consumer.
  Check version-1/legacy behavior separately from the new path.
- Confirm configuration defaults are conservative, documented, cached, range-validated, and cheap while disabled.

## Regression surface

- `src/WowMinTelemetry.cpp` is cross-cutting: a small hook or lock change can affect every map or combat event.
  Review all call sites, not only the feature being changed.
- Stream changes can affect every packet family because they share sender state and `STREAM_VERSION`.
- Playerbots API changes can break only the optional build while core-only checks continue to pass.
- Documentation is part of the external contract. A code-only fix is incomplete when command fields, packet layouts,
  defaults, installation, or compatibility changed.

## Runtime testing

Do not invent runtime results. When the author has tested in-game or against WoWMin, record the exact core/module/reader
revisions, configuration, map/instance type, player/bot cases, and failure branches covered. Ask for targeted follow-up
when the main path was tested but expiry, disable/reload, malformed config, packet splitting, or core-only operation was
not.
