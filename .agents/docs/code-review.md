# Code review

Review the complete relevant change against `AGENTS.md`, `.coderabbit.yaml`, and every applicable document in this
directory. A posted review contains actionable findings only; no summary or praise. If nothing is wrong, say so in one
line.

Each finding identifies the offending line, the violated invariant, the concrete impact, and the smallest safe direction
for a fix. Bugs, crashes, races, lifetime errors, protocol incompatibility, unbounded state, security regressions, and
written-rule violations are findings; unsupported preferences are not.

## Required checks

- Run AzerothCore C++ codestyle, clang-format dry-run, and `git diff --check` as described in `build.md`.
- Review every changed line for `.editorconfig`, including documentation and configuration.
- Do not treat disabled standalone cppcheck/clang tooling as validation. If a diagnostic is considered, reproduce it on
  the base revision and trace the C++ semantics before reporting it.
- Verify claims against the checked-out core, optional playerbots source, configuration, protocol docs, and paired
  WoWMin parser when available.

## Control flow and lifetime

- Trace every new early return, branch, lock transition, lookup, and feature-disable path into cleanup and consumers.
- Check behavior for null targets, absent maps/sessions/groups, map destruction, logout, config reload, invalid IDs,
  expired subscriptions, disabled telemetry, and builds without mod-playerbots.
- Raw object pointers remain inside their valid call/update. Shared state has an owner, synchronization, a bound, and a
  cleanup path.
- Establish lock ordering. Bounded snapshot copying under the protecting lock is valid; flag expensive processing,
  network I/O, large formatting, or callbacks under shared locks.

## Protocol review

For any text or UDP change, verify field meaning and order, fixed sizes, offsets, count widths, length caps, split math,
empty/clear messages, version negotiation, backward compatibility, README, configuration, and consumer parser/tests.
Remember that `STREAM_VERSION` is shared by every datagram family and that semantic changes can require a version bump
even when field count is unchanged.

## Performance and security

- Determine what executes per world update, map update, unit hook, combat event, streamed player, creature, or game
  object.
- Disabled features return before locks, allocations, scans, or history collection.
- Network destinations remain server-configured; commands cannot redirect UDP traffic.
- Invalid configuration and network failures cannot create per-tick log storms or unbounded retries/state.
- Optional playerbots integration observes only and remains absent from the core-only build.

On an existing PR, inspect all conversation, review, and inline threads and verify each against the current head rather
than assuming old comments are resolved.
