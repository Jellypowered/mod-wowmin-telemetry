# Commits and pull requests

## Base and scope

The repository's default development base is `origin/main`; `testing` also exists, so use a different base only when the
user or repository workflow explicitly names it. Do not reuse mod-playerbots' `test-staging`/`master` policy.

Keep module, parent-core, mod-playerbots, and WoWMin changes in their respective repositories. Link companion changes
and state required revisions rather than combining nested repositories into one commit.

## Titles and commits

Follow the established history: use a concise imperative subject that describes observable behavior, with no trailing
period. Use a conventional prefix such as `fix:`, `feat:`, or `docs:` only when it matches surrounding branch history or
user requests it. Keep the subject at most 72 characters and wrap any explanatory body at 72 characters.

A body explains why the change is needed, compatibility implications, and non-obvious tradeoffs. Do not claim tests,
issues, sources, or runtime results that are not established by the diff or conversation.

## PR description

There is currently no repository PR template. A useful description contains only applicable sections:

- Problem: the observable failure, cost, or compatibility gap.
- Change: what the module now does, including enablement and defaults.
- Protocol/compatibility: text/UDP version effects and companion WoWMin, core, or playerbots revisions.
- Threading/performance: affected hooks/threads, scan frequency, bounds, locks, and cleanup.
- Testing: checks and runtime scenarios actually performed, plus exact revisions/configuration when relevant.
- Known limitations: unverified consumer/build combinations or deliberate truncation/fallback behavior.
- Provenance: upstream source and original authors for adapted code.

Disclose AI assistance accurately when requested by the hosting project or maintainer.

## Upstream code

This repository is MIT-licensed. Before copying code, inspect the source license and history. Preserve required notices,
credit original authors in commit trailers or the PR, and do not import incompatible code. Reimplementing an idea does
not remove the obligation to identify copied expression or data.
