---
name: pr-title
description: Propose three concise PR or commit titles for the current mod-wowmin-telemetry changes.
metadata:
  version: "1.1"
---

# PR title

Print exactly three candidate titles for the current changes, best first, followed by one line naming the recent-history
precedent used. Output text only; never create a PR, commit, push, stage, or run another state-changing Git command.

## Gather changes

Use the union of:

- committed branch work against the requested base, otherwise `origin/main...HEAD`;
- staged changes;
- unstaged and untracked changes;
- recent commit subjects touching the same files, as naming context only.

If the user names a PR, inspect that PR's diff instead. If there are no changes, say so and stop.

## Format

Follow `.agents/docs/pull-requests.md`:

- imperative, observable behavior, no trailing period, at most 72 characters;
- use `fix:`, `feat:`, `docs:`, `chore:`, or another conventional prefix only when supported by nearby history or the
  user request;
- name the compatibility or user-visible result rather than the implementation mechanism;
- do not claim testing, runtime behavior, or paired-repository changes not established by the diff.

## Output

Print three title lines and then:

`precedent: <hash> <recent title>`

Nothing else.
