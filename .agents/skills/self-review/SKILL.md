---
name: self-review
description: Self-review the full module repository, current uncommitted changes (default), or exact staged changes.
disable-model-invocation: true
license: MIT
metadata:
  version: "1.1"
  upstream-self-review-version: "0.14"
  upstream-fresh-eyes-review-version: "1.3"
---

# Self-review

Adapted from Engineering in the age of AI's MIT-licensed
[self-review](https://github.com/eai-org/agent-toolkit/blob/main/skills/self-review/SKILL.md) and
[fresh-eyes-review](https://github.com/eai-org/agent-toolkit/blob/main/skills/fresh-eyes-review/SKILL.md).
Upstream license and copyright are preserved in [LICENSE](LICENSE). This adaptation is self-contained; neither upstream
skill nor another review toolkit needs to be installed.

Audit the current module repository or its uncommitted changes, walk concrete findings with the user, and record what
was actually reviewed. Full audits include existing defects, not just code authored in this task. For someone else's PR,
use the module's ordinary code-review process instead.

## Invocation

- `/self-review` or `/self-review current`: all current uncommitted changes, including staged, unstaged, and non-ignored
  untracked files. Already committed branch work is excluded.
- `/self-review full`: every tracked file as currently present plus non-ignored untracked files in this module.
  This is a repository audit, not a diff against a branch.
- `/self-review staged`: exactly the staged patch and index versions of affected files, not their working-tree versions.
  Unstaged changes and untracked files are excluded.

The direct `/skill:self-review` entry point uses the same arguments and defaults. An optional `autoaccept` follows the
mode; `autoaccept` alone means `current autoaccept`. Unknown modes, multiple modes, and branch/base arguments require
clarification, never silently falling back to another scope. No mode fetches or requires a remote or merge base.

## 1. Load policy and resolve scope

Paths below are relative to the module repository root, not this skill directory. Resolve that root as `../../..` from
this directory and verify its `AGENTS.md` and Git repository before running commands. Never operate on the parent core's
Git index by mistake.

Read `AGENTS.md`, `.coderabbit.yaml`, `.agents/docs/code-review.md`, `.agents/docs/self-review-rules.md`,
`.agents/docs/build.md`, and applicable C++, telemetry, playerbots, SQL, or PR guidance. Module rules override upstream
workflow, including allowing uncommitted changes and running non-build verification.

State the selected mode and HEAD SHA (or `unborn`); detached HEAD is valid. Use NUL-delimited Git path lists and safe
argument handling so spaces, newlines, and leading dashes in paths do not alter scope. Record intentional exclusions.

### `current` (default)

Use `git diff HEAD` for combined tracked changes, `git diff --cached` for staged changes, and `git diff` for unstaged
changes. Include the union, not just the combined diff: staged edits canceled by unstaged edits still need review. Read
index and disk versions where they differ and label findings by layer. Enumerate non-ignored untracked files using
`git ls-files --others --exclude-standard -z` and inspect disk content. Previously reviewed untracked files must be
read again if modified; Git does not label their edits as tracked modifications. Do not stage them.

On an unborn branch, compare the index against the empty tree and include unstaged and untracked content; do not require
an initial commit. Include additions, deletions, renames, type changes, and mode-only changes in both layers.

### `staged`

Use `git diff --cached HEAD` (empty-tree baseline when unborn) and obtain scoped file bytes from the index, for example
via `git show :path`. Never substitute disk content for an index blob. Resolve unchanged module context from the index
as well; use HEAD for deleted-file history. Disk-only policy or external dependencies may be consulted as explicitly
labeled context, not certified as staged content. A staged deletion is reviewed even if a disk copy exists.

### `full`

Enumerate the union of `git ls-files -z` and `git ls-files --others --exclude-standard -z`, then inspect current disk
content for every present file. Record tracked paths missing on disk as deletions, not unread files or existing blobs.
Include tracked files even if an ignore pattern matches them. Review all file types, including docs, workflows, agent
guidance, configuration, and licensing; do not restrict the audit to source or recent changes. Use a repository-health
mandate rather than asking only whether a new change is mergeable.

### Shared scope rules

- Scope is this nested module repository, not the parent core or sibling modules. Inspect dependencies for grounding,
  without claiming they were audited. Do not follow symlinks outside scope or recursively audit submodule repositories;
  record link targets/gitlinks and flag unavailable required context.
- Exclude `.git`, ignored untracked files, review reports, and non-shipping local scratch/plans. Name exclusions;
  intended-to-ship agent guidance and planning documents remain in scope. Never silently exclude an ambiguous file.
- Unmerged index entries block an exact staged snapshot. For current/full, identify conflicts and ask how to proceed;
  never certify conflict-marker content or choose a resolution silently.
- If a file is binary, too large, or otherwise unreadable, record the inspection method or missing coverage. Batch large
  full audits with explicit file coverage; never claim the whole repository was reviewed after sampling it.
- An empty current/staged scope is a valid no-op: report no changes for that mode. Do not switch to full automatically.

State the intent briefly from the user's requirements, ticket, or change. For full, use repository correctness, safety,
and policy compliance as the audit goal; no new feature intent is required. Ask only if the intended scope is unclear.
Record available submission metadata (branch subjects and supplied PR title/body/linked issues) without inventing a PR
or requiring network access. Missing metadata is unavailable, not a review failure.

## 2. Capture the reviewed state

Use scratch storage outside the repository for captured patches/content. Capture a deterministic sorted manifest with
paths, file types/modes, deletion markers, and byte hashes (link targets for symlinks), plus:

- `current`: HEAD baseline, both staged/unstaged patches, index and disk versions of changed paths, and untracked bytes.
- `staged`: HEAD baseline, cached patch, and index blobs/modes. Excluded disk edits must not affect this fingerprint.
- `full`: every included current disk file and tracked deletion. No branch diff is needed.

Hash the mode, applicable baseline, patches, and manifest deterministically. Hash available submission metadata
separately, distinguishing unavailable metadata from empty text. State the algorithm and inputs; a plain diff hash
cannot cover untracked files or a full audit. Recheck the captured state after reading to detect concurrent edits.

Label reports `current: index + working tree on <SHA>`, `staged: index against <SHA>`, or `full: working tree on <SHA>`
(use `unborn` when appropriate), with fingerprints and included paths. Never label a staged review as certifying the
working tree. Neither a dirty tree nor untracked files blocks review; never require staging or committing to run it.

## 3. Obtain a fresh review

If an isolated reviewer/subagent tool is available, start one fresh-context reviewer. Supply only:

- mode, resolved scope, selected snapshot (index-based for staged), manifest, intent, and submission metadata;
- module root, governing documents, and the local core/playerbots/WoWMin paths needed for implementation grounding;
- exclusions for non-shipping plans, previous reports, and author rationale;
- the mandate below and a request to return actual harness/model provenance, coverage, and unavailable sources.

Do not pass author reasoning, previous dispositions, or the full authoring conversation. The reviewer may inspect
surrounding source from the selected snapshot (not disk substitutes for staged context), but writes no project files,
runs no build/configuration, and makes no Git or remote mutations.

If isolation is unavailable, perform an adversarial pass in the current session over the same artifacts. State
`same-context review; weaker independence` visibly in the report. Never fabricate a reviewer, tool, or model name.

### Reviewer mandate

For current/staged, would a concrete correctness issue or documented-rule violation block merging the scoped changes?
For full, identify concrete existing defects and documented-rule violations throughout the repository, even in unchanged
code. Check every scoped file and its relevant call paths. Cite location, failure scenario or exact violated rule,
severity, and fix direction. Speculation and taste are not findings; zero findings is valid. Verify mirrored patterns
instead of assuming existing code is correct. Include missed counterparts, debug leftovers, and inaccurate claims.

Apply module-specific checks:

- Map/world thread ownership, object lifetime, lock order, and bounded snapshot copying under protecting locks.
- No network I/O or expensive processing under shared locks; no synchronous database queries on hot paths.
- Disabled collection, text histories independent of UDP subscriptions, expiry without live maps, reload, and cleanup.
- Wire meaning/version, all datagram families, byte sizes/offsets, count/length caps, splitting, and empty clears.
- Server-owned UDP destination, range validation, failure retry/log volume, and per-map/unit collection costs.
- Both `MOD_PLAYERBOTS` paths, actual local API signatures, and observational-only integration.
- README/configuration/consumer consistency. If a paired repository is unavailable, report compatibility as unverified,
  not broken without evidence. Do not silently edit that repository.

## 4. Verify and disposition

Run applicable non-build checks from `.agents/docs/build.md`, recording commands, versions, exit statuses, and actual
results. Validate the selected snapshot: for staged, use cached checks or an isolated read-only index export, not disk
versions. In current mode, distinguish index and disk check coverage where versions differ. Label checks requiring a
different snapshot as context or unavailable. Never overwrite the working tree to
run checks. Check full/untracked files directly: `git diff --check` does not inspect them. Do not configure,
build, install tools, add live-stack tests, or run a server unless explicitly requested. Disabled cppcheck is not a
required check, and its known `emplace_back` false positives are not defects.

Walk findings one at a time. Explain each simply, recommend a disposition with a reason, and offer **fix**, **dismiss**,
**defer**, or **discuss**. Discussion does not close a finding. Read relevant requirements/plans before recommending a
fix, but never leak author rationale to the independent reviewer.

- **Fix:** apply only the user-approved working-tree change, verify it, then review the delta. Do not stage or commit.
  In staged mode, an on-disk fix does not resolve the index finding. Mark it **open: fix awaiting user staging** until
  the user stages it and a staged re-review verifies the fix. Preserve unrelated staged/unstaged edits; ask if a fix
  cannot safely be applied to the disk version.
- **Dismiss:** record the user's reason; verify factual claims such as an existing guard before accepting them as fact.
- **Defer:** record the reason and destination (a follow-up draft or local note suffices). Do not file or post anything.

Keep every finding in the report, including dismissed and deferred ones. A recurring fixed finding reopens; an unchanged
dismissed/deferred finding retains its disposition. If stopped mid-walk, record remaining findings as **open**.

Only explicit `autoaccept` authorizes the agent to choose remaining dispositions. It never authorizes commits, remote
operations, cross-repository edits, destructive changes, or redesigns; ask separately for those. Record where autoaccept
began and the reason for every agent-selected disposition.

## 5. Repeat with a bound

The first round reviews the entire selected scope. A scoped fix or uncovered file requires another round: provide the
exact delta and its reach, the complete current scope snapshot as reference, and previously uncovered files. Re-review
changed metadata too. If the previous snapshot is unavailable, repeat the whole selected scope, not another mode.
In staged mode, wait for user staging before re-reviewing an on-disk fix; do not loop over the unchanged index.

Stop after a round with no new fixes, metadata changes, or uncovered files, or after three rounds per invocation. The
user may stop earlier or explicitly authorize more. At the cap, disclose open findings, uncovered files, failed checks,
and fixes not re-reviewed. Never call that state clean or merge-ready.

Re-capture fingerprints after approved fixes and verification. If unrelated edits appear during review, reconcile scope
with the user and review them before claiming coverage.

## 6. Save a compact local report

Use the existing task slug or derive one. Save `.agents/plans/<slug>/<slug>.SELF-REVIEW.md`; the directory is ignored.
Within an invocation update that report. On another invocation use `SELF-REVIEW-2.md`, `-3.md`, etc., carrying
forward dispositions; do not overwrite a previous review except a verified stamp update.

The visible report contains:

- heading, intent-sized scope label, and outcome with fixed/dismissed/deferred/open counts;
- caveats: same-context fallback, uncovered files, failed/unavailable required checks, and unreviewed fixes;
- mode, reviewed HEAD/baseline, index versus disk scope, fingerprint, date, and actual harness/model (or `unknown`);
- verification actually run, clearly separating static checks, builds, and runtime tests.

Put intent, applied project rules, exclusions, metadata hash/availability, fingerprint inputs, coverage, and each
round inside `<details><summary>Review details</summary>` with a blank line after the summary. Each finding gets one
compact line: location, concrete problem, final disposition, and reason/destination. Keep full explanations in-session.

A report records scrutiny, not approval. Never claim merge readiness with open/deferred blockers, failed checks, missing
coverage, unreviewed fixes, or unverified compatibility required by this change.

### Re-run and stamp

On re-invocation resolve the requested mode again; reports and dispositions are mode-specific. Current/staged may now
be empty after a commit: report that no-op, not an automatic review of branch history. A prior report can be stamped to
a commit only on explicit request after verifying equivalent reviewed paths, modes, bytes, and context. A staged fix
requires index verification, not a matching disk version. Changed metadata needs metadata review; changed content needs
another review within the selected mode. Never certify another mode or content the prior round did not cover.

## Boundaries and wrap-up

Write only the report and approved fixes. Git stays read-only; `fetch` requires approval. Do not stage, commit, reset,
stash, checkout, push, publish a PR, post comments, or change PR metadata. Drafting suggestions is allowed.

Finish with the report path, disposition counts, remaining blockers, and limitations. Explain that the report is ignored
and should not be committed. It may be pasted into a PR by the user, but must describe uncommitted work honestly. No
commit is required to complete local self-review; if the user later commits or edits the change, re-run to verify/stamp.
