# Build and checks

## Default verification

Do not configure or build unless the user explicitly asks. From the module root, run the checks applicable to the files
changed:

```bash
python3 ../../apps/codestyle/codestyle-cpp.py
find src -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print0 \
  | xargs -0 --no-run-if-empty clang-format --dry-run --Werror
git diff --check
```

For YAML changes, parse the changed files with an available YAML parser. Inspect workflow changes with `actionlint` when
installed; do not install tools or use network access merely to obtain it.

The repository-owned CI uses AzerothCore's reusable module codestyle workflow and a separate `clang-format-18` job.
Local clang-format may be a nearby version; report the version when output differs from CI.

CodeRabbit's standalone clang, FBInfer, and cppcheck tools are disabled. They lack the core build context, and cppcheck
2.17 reports false `invalidContainerReference` errors for references returned by `vector::emplace_back()`. Do not report
those baseline diagnostics as module defects.

There are currently no module-owned unit or live-stack tests. Do not create ad-hoc live-server tests unless explicitly
asked.

## When a build is requested

Read the parent core's `../../.agents/docs/build.md`, use an existing out-of-source build when possible, and state the
exact core and module revisions tested. Do not alter CMake configuration implicitly.

For integration-sensitive C++ changes, verify both configurations when the available build setup permits:

1. AzerothCore with mod-wowmin-telemetry and without mod-playerbots.
2. AzerothCore with both modules, where `MOD_PLAYERBOTS` is defined.

A successful build does not replace protocol, thread-ownership, or lifetime review.
